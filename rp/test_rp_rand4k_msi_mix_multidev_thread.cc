#include "test_rp.hh"
#include "iommu_top.hh"
#include "iommu_registers.hh"
#include "iommu_utils.hh"
#include <array>
#include <deque>
#include <memory>
#include <random>
#include <set>
#include <numeric>

#if TEST_CFG_MULTI_DEVICE_SCENE
namespace {
using iommu_md::require;
constexpr unsigned N = TEST_CFG_NUM_DEVICES;
constexpr unsigned QUEUE_DEPTH = TEST_CFG_MD_QUEUE_DEPTH;
constexpr bool FIXED_TOTAL = TEST_CFG_MD_FIXED_TOTAL;
constexpr unsigned GROUPS = FIXED_TOTAL ? 1440 / N : TEST_CFG_NUM_PAGES;
constexpr unsigned TAILS = FIXED_TOTAL ? 16 / N : 1;
constexpr uint64_t IOVA_BASE = 0x100000ULL;
#ifdef TEST_CFG_S13_IOVA_512MB
constexpr unsigned RANGE_MB = 512, HUGE_COUNT = 50;
constexpr uint64_t MSI_PATTERN = 0x7000, MSI_IOVA_BASE = 0x21000000;
constexpr uint64_t SQ_IOVA = 0x22000000, CQ_IOVA = 0x22001000, CTRL_GPA = 0x8000000;
#else
constexpr unsigned RANGE_MB = 16, HUGE_COUNT = 20;
constexpr uint64_t MSI_PATTERN = 0x3000, MSI_IOVA_BASE = 0x2000000;
constexpr uint64_t SQ_IOVA = 0x5000000, CQ_IOVA = 0x5001000, CTRL_GPA = 0x4000000;
#endif
constexpr unsigned RANGE_PAGES = RANGE_MB * 256;
constexpr uint64_t PROBE_IOVA = SQ_IOVA + 0x10000;
constexpr uint64_t HUGE = 0x200000ULL;
static_assert(N >= 1 && N <= 16, "设备数必须为1..16");
static_assert(QUEUE_DEPTH > 0 && QUEUE_DEPTH <= 1048576, "就绪队列深度非法");
static_assert(GROUPS > 0 && GROUPS <= RANGE_PAGES, "组数超过候选页数");
static_assert(PT_DEDUP_PREFETCH_DEPTH <= 16, "预取深度超过结果槽容量");
static_assert(TEST_CFG_MD_RR_START >= 0 && TEST_CFG_MD_RR_START < N, "轮询起点非法");
static_assert(!FIXED_TOTAL || N == 1 || N == 2 || N == 4 || N == 8 || N == 16,
              "等总负载模式要求N为1/2/4/8/16");

uint64_t spa_offset(unsigned d) { return 0x80000000ULL + uint64_t(d) * 0x10000000ULL; }
uint64_t msi_pa(unsigned d, unsigned v) { return 0x40000000ULL + uint64_t(d)*0x100000ULL + uint64_t(v)*4096 + 0x40; }

struct Device {
    device_context_t dc{};
    uint64_t dc_address = 0, msi_iova = 0, probe_gpa = 0;
    std::vector<unsigned> pages, msi_pages;
    std::vector<uint64_t> gpas;
    std::deque<iommu_md::Request> queue;
    sc_event space_event;
    bool ready = false, done = false;
    uint64_t injected = 0, responded = 0, peak_queue = 0, full_waits = 0;
    double full_wait_ns = 0;
};

// 所有SystemC进程属于单个RP，只有scheduler访问唯一的initiator socket。
class MultiDeviceScenario {
    RP_Module& rp;
    iommu_top& top;
    std::array<Device, N> devices;
    std::vector<std::unique_ptr<tlm_generic_payload>> payloads;
    sc_event phase_event, ready_event;
    unsigned epoch = 0;
    bool sent = false, stopped = false;
    double start_ns = 0;
    uint64_t last_progress = 0;
    double last_progress_ns = 0;

    static std::vector<unsigned> choose(unsigned count, unsigned range, unsigned seed, unsigned shuffle_seed) {
        std::mt19937 rng(seed), permutation(shuffle_seed);
        std::set<unsigned> chosen;
        while (chosen.size() < count) chosen.insert(rng() % range);
        std::vector<unsigned> result(chosen.begin(), chosen.end());
        for (size_t i=result.size();i>1;--i) std::swap(result[i-1],result[permutation()%i]);
        return result;
    }
    uint64_t count(unsigned e) const { return e == 1 ? 2 : e == 2 ? 16 : uint64_t(GROUPS)*11 + TAILS; }
    void map_vs(Device& dev, uint64_t iova, uint64_t gpa) {
        spte_t p{};
        p.V=p.R=p.W=p.U=p.A=p.D=1;
        p.PBMT=PMA;
        p.PPN=gpa/4096;
        require(rp.add_vs_stage_pte(&top,dev.dc.fsc.iosatp,iova,p,0,dev.dc.iohgatp,0)!=uint64_t(-1),
                "构建VS页表失败");
    }
    void map_g(Device& dev, uint64_t gpa, uint64_t spa, unsigned level) {
        gpte_t p{};
        p.V=p.R=p.W=p.U=p.A=p.D=1;
        p.PBMT=PMA;
        p.PPN=spa/4096;
        require(rp.add_g_stage_pte(&top,dev.dc.iohgatp,gpa,p,level)!=uint64_t(-1), "构建G-stage失败");
    }
    void setup() {
        // 保留现有低地址寄存器/CQ区域，只初始化一次物理分配游标。
        next_free_page=std::max<uint64_t>(next_free_page,240);
        // 最坏页表预算：每个2MiB IOVA区一张L0表，另留目录、G-stage及MSI页裕量。
        const uint64_t per_device_pages=(uint64_t(RANGE_MB)+2)/2+64;
        require((next_free_page+1+N*per_device_pages)*4096 <= DDR_Module::DDR_MEMORY_SIZE,
                "所有设备页表预算超过32MiB DDR");
        require(rp.enable_iommu(&top,DDT_1LVL)>=0,"启用IOMMU失败");
        std::vector<unsigned> shared_pages;
        if(FIXED_TOTAL) shared_pages=choose(1440,RANGE_PAGES,2024,123);
        for(unsigned d=0;d<N;++d) {
            auto& dev=devices[d];
            const unsigned seed_delta=(!FIXED_TOTAL && TEST_CFG_MD_INDEPENDENT) ? d : 0;
            dev.pages=FIXED_TOTAL ? std::vector<unsigned>(shared_pages.begin()+d*GROUPS,shared_pages.begin()+(d+1)*GROUPS)
                                 : choose(GROUPS,RANGE_PAGES,2024+seed_delta,123+seed_delta);
            dev.msi_pages=choose(256,1024,777+seed_delta,777+seed_delta);
            dev.msi_iova=MSI_IOVA_BASE+uint64_t(dev.msi_pages[0])*4096+0x40;
            dev.probe_gpa=0x100000ULL+uint64_t(d)*4096;
            next_free_gpage[d+1]=0x10000;
            dev.dc_address=rp.add_device(&top,d,d+1,0,0,0,0,0,1,1,0,0,0,
                                        IOHGATP_Sv48x4,IOSATP_Sv48,PDTP_Bare,MSIPTP_Flat,1,0xFF,MSI_PATTERN);
            require(dev.dc_address!=uint64_t(-1),"添加设备失败");
            rp.read_memory_test_rp(dev.dc_address,sizeof(dev.dc),reinterpret_cast<char*>(&dev.dc));
            dev.dc.ta.PSCID=0;
            rp.write_memory_test_rp(reinterpret_cast<char*>(&dev.dc),dev.dc_address,sizeof(dev.dc));
            device_context_t readback{};
            rp.read_memory_test_rp(dev.dc_address,sizeof(readback),reinterpret_cast<char*>(&readback));
            require(readback.iohgatp.GSCID==d+1 && readback.ta.PSCID==0 && readback.tc.PDTV==0,
                    "DC读回上下文不匹配");
            for(unsigned h=0;h<HUGE_COUNT;++h) map_g(dev,uint64_t(h)*HUGE,uint64_t(h)*HUGE+spa_offset(d),1);
            for(unsigned v=0;v<256;++v) {
                msipte_t mp{};
                mp.V=1; mp.M=3; mp.translate_rw.PPN=msi_pa(d,v)/4096;
                rp.write_memory_test_rp(reinterpret_cast<char*>(&mp),dev.dc.msiptp.PPN*4096+v*16,16);
                map_vs(dev,MSI_IOVA_BASE+uint64_t(dev.msi_pages[v])*4096,(MSI_PATTERN<<12)+uint64_t(v)*4096);
            }
            dev.gpas.resize(RANGE_PAGES);
            std::mt19937 mapping(42+seed_delta);
            std::array<unsigned,HUGE_COUNT> slots{};
            // 先生成全范围映射值，fixed_total不因每设备访问子集改变映射。
            for(unsigned i=0;i<RANGE_PAGES;++i) {
                unsigned h=mapping()%HUGE_COUNT, slot=0;
                if(RANGE_MB==16) {
                    while(slots[h]>=512) h=(h+1)%HUGE_COUNT;
                    slot=slots[h]++;
                } else slot=mapping()%512;
                dev.gpas[i]=uint64_t(h)*HUGE+uint64_t(slot)*4096;
            }
            std::set<unsigned> mapped;
            if(RANGE_MB==16) for(unsigned i=0;i<RANGE_PAGES;++i) mapped.insert(i);
            else for(unsigned page:dev.pages) {
                unsigned vpn0=((IOVA_BASE>>12)+page)&511;
                unsigned depth=std::min<unsigned>(PT_DEDUP_PREFETCH_DEPTH,511-vpn0);
                for(unsigned k=0;k<=depth && page+k<RANGE_PAGES;++k) mapped.insert(page+k);
            }
            for(unsigned page:mapped) map_vs(dev,IOVA_BASE+uint64_t(page)*4096,dev.gpas[page]);
            map_vs(dev,SQ_IOVA,CTRL_GPA);
            map_vs(dev,CQ_IOVA,CTRL_GPA+4096);
            map_g(dev,CTRL_GPA,CTRL_GPA+spa_offset(d),0);
            map_g(dev,CTRL_GPA+4096,CTRL_GPA+4096+spa_offset(d),0);
            // 独立哨兵使各设备S1结果也不同；不计入正式性能请求。
            map_vs(dev,PROBE_IOVA,dev.probe_gpa);
            for(uint64_t base:{SQ_IOVA,CQ_IOVA,PROBE_IOVA}) {
                unsigned depth=std::min<unsigned>(PT_DEDUP_PREFETCH_DEPTH,511-((base>>12)&511));
                for(unsigned k=1;k<=depth;++k) {
                    uint64_t page=base+uint64_t(k)*4096;
                    if(page==SQ_IOVA || page==CQ_IOVA || page==PROBE_IOVA) continue;
                    map_vs(dev,page,0x200000ULL+uint64_t(k)*4096);
                }
            }
            uint64_t digest=1469598103934665603ULL;
            for(unsigned page:dev.pages) digest=(digest^page)*1099511628211ULL;
            std::cout << "[MD_SETUP] DID=" << d << " GSCID=" << d+1 << " PSCID=0 pages=" << mapped.size()
                      << " sequence_hash=" << digest << " root=" << dev.dc.iohgatp.PPN
                      << " free_ppn=" << next_free_page << '\n';
            top.md_trace.progress();
        }
    }
    void invalidate_all() {
        iommu::CacheMessage cmd;
        cmd.msg_type=iommu::CacheMsgType::DC_INVALIDATE;
        cmd.cmd_type=iommu::InvalidCmdType::GLOBAL_INVAL;
        cmd.invalidate_mode=iommu::CacheInvalidateMode::GLOBAL;
        top.cache_sub.invalidation_request_fifo.write(cmd);
        auto response=top.cache_sub.invalidation_response_fifo.read();
        require(response.msg_type==iommu::CacheMsgType::CACHE_INVALIDATE_RESPONSE,"全局失效未完成");
        top.md_trace.progress();
    }
    iommu_md::Request descriptor(unsigned d,unsigned e,uint64_t seq) {
        auto& dev=devices[d];
        iommu_md::Request r;
        r.device=d; r.epoch=e; r.sequence=seq;
        if(e==1) {
            r.bytes=512; r.iova=PROBE_IOVA; r.expected_pa=dev.probe_gpa+spa_offset(d);
            return r;
        }
        unsigned type;
        if(e==2) type=seq<8 ? 3 : (seq%2 ? 2 : 1);
        else {
            r.group=seq/11;
            unsigned offset=seq%11;
            type=seq>=uint64_t(GROUPS)*11 ? 3 : offset<8 ? 0 : offset-7;
            if(type==0) {
                unsigned page=dev.pages[r.group];
                r.iova=IOVA_BASE+uint64_t(page)*4096+offset*512;
                r.expected_pa=dev.gpas[page]+spa_offset(d)+offset*512;
                r.bytes=512; r.write=(r.group%2)!=0;
            }
        }
        r.type=type;
        if(type==1) { r.iova=SQ_IOVA; r.expected_pa=CTRL_GPA+spa_offset(d); r.bytes=32; }
        if(type==2) { r.iova=CQ_IOVA; r.expected_pa=CTRL_GPA+4096+spa_offset(d); r.bytes=16; r.write=true; }
        if(type==3) { r.iova=dev.msi_iova; r.expected_pa=msi_pa(d,0); r.bytes=4; r.write=true; }
        return r;
    }
    void producer(unsigned d) {
        unsigned previous=0;
        while(!stopped) {
            while(epoch==previous && !stopped) wait(phase_event);
            if(stopped) return;
            const unsigned current=epoch;
            auto& dev=devices[d];
            for(uint64_t seq=0;seq<count(current);++seq) {
                if(dev.queue.size()>=QUEUE_DEPTH) {
                    double begin=iommu_md::now_ns(); ++dev.full_waits;
                    while(dev.queue.size()>=QUEUE_DEPTH && !stopped) wait(dev.space_event);
                    dev.full_wait_ns+=iommu_md::now_ns()-begin;
                }
                if(stopped) return;
                auto r=descriptor(d,current,seq);
                r.ready_ns=iommu_md::now_ns();
                dev.queue.push_back(r);
                dev.peak_queue=std::max<uint64_t>(dev.peak_queue,dev.queue.size());
                dev.ready=true;
                ready_event.notify(SC_ZERO_TIME);
            }
            dev.done=true; previous=current;
            ready_event.notify(SC_ZERO_TIME);
        }
    }
    void scheduler() {
        unsigned previous=0;
        while(!stopped) {
            while(epoch==previous && !stopped) wait(phase_event);
            if(stopped) return;
            unsigned current=epoch,next=TEST_CFG_MD_RR_START;
            while(true) {
                bool all=true; for(auto& d:devices) all=all && (d.ready || d.done);
                if(all) break;
                wait(ready_event);
            }
            while(!stopped) {
                int selected=-1;
                for(unsigned k=0;k<N;++k) {
                    unsigned d=(next+k)%N;
                    if(!devices[d].queue.empty()) { selected=d; break; }
                }
                if(selected<0) {
                    bool done=true; for(auto& d:devices) done=done && d.done;
                    if(done) break;
                    wait(ready_event); continue;
                }
                unsigned d=static_cast<unsigned>(selected);
                auto& dev=devices[d];
                auto record=dev.queue.front(); dev.queue.pop_front();
                dev.space_event.notify(SC_ZERO_TIME);
                require(record.epoch==current && record.sequence==dev.injected,"队列内请求顺序错误");
                record.grant_ns=iommu_md::now_ns();
                auto trans=std::make_unique<tlm_generic_payload>();
                trans->set_address(record.iova);
                trans->set_data_ptr(new unsigned char[record.bytes]());
                trans->set_data_length(record.bytes);
                trans->set_streaming_width(record.bytes);
                trans->set_command(record.write ? TLM_WRITE_COMMAND : TLM_READ_COMMAND);
                trans->set_response_status(TLM_INCOMPLETE_RESPONSE);
                auto* ext=new PayloadExtention();
                ext->requester_id=d; ext->pid_valid=0; ext->process_id=0;
                ext->exec_req=0; ext->priv_req=0; ext->no_write=record.write ? 0 : 1;
                ext->at=0; ext->is_ctrl=record.type!=0;
                trans->set_extension(ext);
                auto* ptr=trans.get();
                payloads.push_back(std::move(trans));
                top.md_trace.register_request(ptr,record);
                sc_time delay=SC_ZERO_TIME; tlm_phase phase=BEGIN_REQ;
                auto status=rp.axi_master_to_pcie_noc_0_socket->nb_transport_fw(*ptr,phase,delay);
                require(status==TLM_UPDATED && phase==END_REQ,"入口握手异常，禁止重发");
                ++dev.injected;
                next=(d+1)%N;
                std::cout << "[MD_GRANT] epoch=" << current << " did=" << d << " seq=" << record.sequence
                          << " grant=" << record.grant_ns << " accept=" << iommu_md::now_ns() << '\n';
            }
            previous=current; sent=true;
        }
    }
    void watchdog() {
        while(!stopped) {
            wait(1000,SC_NS);
            if(top.md_trace.progress_serial!=last_progress) {
                last_progress=top.md_trace.progress_serial; last_progress_ns=iommu_md::now_ns();
            }
            if(iommu_md::now_ns()-last_progress_ns>50000 || iommu_md::now_ns()-start_ns>1000000) {
                top.md_trace.diagnostic();
                for(unsigned d=0;d<N;++d) std::cerr << "[MD_QUEUE] did=" << d << " queued=" << devices[d].queue.size()
                    << " sent=" << devices[d].injected << " done=" << devices[d].responded << '\n';
                std::cerr << "[MD_STATE] buffer=" << top.cache_sub.get_pt_dedup_buffer()->get_valid_count()
                          << " groups=" << top.prefetch_groups.size() << " ptw=" << top.ptw_active_walks.size() << '\n';
                require(false,"多设备测试超时，未释放在途payload");
            }
        }
    }
    void run_epoch(unsigned e) {
        for(auto& d:devices) {
            require(d.queue.empty(),"切换epoch时队列非空");
            d.ready=d.done=false; d.injected=d.responded=d.peak_queue=d.full_waits=0; d.full_wait_ns=0;
        }
        epoch=e; sent=false; top.md_trace.progress();
        phase_event.notify(SC_ZERO_TIME);
        while(true) {
            bool complete=sent;
            for(auto& d:devices) complete=complete && d.responded==count(e);
            if(complete && top.md_quiescent()) break;
            wait(1,SC_NS);
        }
        for(unsigned d=0;d<N;++d) {
            require(devices[d].injected==count(e),"发送/完成数量不一致");
            std::cout << "[MD_RP] epoch=" << e << " did=" << d << " injected=" << devices[d].injected
                      << " completed=" << devices[d].responded << " queue_peak=" << devices[d].peak_queue
                      << " full_waits=" << devices[d].full_waits << " full_wait_ns=" << devices[d].full_wait_ns << '\n';
        }
    }
public:
    explicit MultiDeviceScenario(RP_Module& r) : rp(r),top(*r.iommu_ptr) {}
    ~MultiDeviceScenario() {
        // 正常结束只在所有响应及异步工作排空后清理；异常停止不提前清理。
        for(auto& trans:payloads) {
            PayloadExtention* ext=nullptr; trans->get_extension(ext);
            trans->clear_extension(ext); delete ext;
            delete[] trans->get_data_ptr(); trans->set_data_ptr(nullptr);
        }
    }
    void response(tlm_generic_payload& trans) {
        PayloadExtention* ext=nullptr; trans.get_extension(ext);
        require(ext && ext->requester_id<N,"响应缺少有效设备ID");
        auto& r=top.md_trace.request(&trans);
        require(r.epoch==epoch,"收到旧epoch响应");
        top.md_trace.response(trans,ext->requester_id);
        ++devices[ext->requester_id].responded;
    }
    void run() {
        wait(20,SC_NS);
        start_ns=last_progress_ns=iommu_md::now_ns();
        std::cout << "[MD_CONFIG] N=" << N << " IOVA_MB=" << RANGE_MB << " groups/device=" << GROUPS
                  << " tails/device=" << TAILS << " queue=" << QUEUE_DEPTH << " D=" << PT_DEDUP_PREFETCH_DEPTH
                  << " mode=" << (FIXED_TOTAL ? "partition" : TEST_CFG_MD_INDEPENDENT ? "independent" : "overlap") << '\n';
        setup();
        for(unsigned d=0;d<N;++d) sc_spawn([this,d](){ producer(d); });
        sc_spawn([this](){ scheduler(); });
        sc_spawn([this](){ watchdog(); });
        invalidate_all();
        run_epoch(1); // 同IOVA、不同S1/S2结果的哨兵。
        invalidate_all();
        run_epoch(2); // 预热不进入正式统计。
        const uint64_t ptw_before=top.ptw_total_completed, ddr_before=top.axi_master_1_read_req_total;
        run_epoch(iommu_md::MEASUREMENT_EPOCH);
        top.md_trace.report("multidev_n"+std::to_string(N)+"_"+std::to_string(RANGE_MB)+"mb");
        std::cout << "[MD_DELTA] PTW=" << top.ptw_total_completed-ptw_before
                  << " DDR_reads=" << top.axi_master_1_read_req_total-ddr_before << '\n';
        // 与单设备基线同口径的IOMMU侧统计，便于横向对比。
        printf("\n========== PTW DDR Access Statistics ==========\n");
        printf("  PTW total completed tasks: %lu\n", (unsigned long)top.ptw_total_completed);
        printf("  PTW total DDR reads:       %lu\n", (unsigned long)top.ptw_total_ddr_reads);
        if (top.ptw_total_completed > 0)
            printf("  PTW avg DDR reads/task:    %.2f\n",
                   (double)top.ptw_total_ddr_reads / top.ptw_total_completed);
        printf("================================================\n");
        auto* dedup_buf = top.cache_sub.get_pt_dedup_buffer();
        if (dedup_buf) {
            printf("\n========== PT Dedup Buffer Statistics ==========\n");
            printf("  Buffer Size:        %u entries\n", PT_DEDUP_BUFFER_SIZE);
            printf("  Peak Valid Count:   %u entries\n", dedup_buf->get_peak_valid_count());
            printf("  Current Valid:      %u entries\n", dedup_buf->get_valid_count());
            printf("  Peak Usage:         %.1f%%\n", 100.0 * dedup_buf->get_peak_valid_count() / PT_DEDUP_BUFFER_SIZE);
            printf("  Buffer Full Bypass: %lu tasks\n", (unsigned long)top.cache_sub.get_dedup_buffer_full_bypass_count());
            printf("  --- Buffer 实际占用率 (时间加权平均) ---\n");
            printf("    Avg Occupancy:    %.2f entries / %u (%.2f%%)\n",
                   dedup_buf->get_avg_occupancy(), PT_DEDUP_BUFFER_SIZE, dedup_buf->get_avg_occupancy_pct());
            printf("    Occupancy window: %.1f ns\n", dedup_buf->get_occupancy_window_ns());
            printf("  --- Buffer Task Hold Time (alloc -> PTW-done free) ---\n");
            printf("    Avg hold:  %.1f ns\n", dedup_buf->get_avg_hold_ns());
            printf("    Max hold:  %.1f ns\n", dedup_buf->get_max_hold_ns());
            printf("    Min hold:  %.1f ns\n", dedup_buf->get_min_hold_ns());
            printf("    Samples:   %lu\n", (unsigned long)dedup_buf->get_hold_count());
            printf("==================================================\n");
        }
        if (top.md_hold_main_cnt || top.md_hold_susp_cnt) {
            printf("\n========== Dedup Buffer Hold Decomposition ==========\n");
            printf("  W1=alloc->group_complete, W2=group_complete->agg_flush_exec, W3=agg_flush_exec->entry_free\n");
            printf("  MAIN entries: n=%lu  avg W1=%.1f W2=%.1f W3=%.1f ns  |  max %.1f/%.1f/%.1f\n",
                   (unsigned long)top.md_hold_main_cnt,
                   top.md_hold_main_cnt ? top.md_hold_main_sum[0]/top.md_hold_main_cnt : 0.0,
                   top.md_hold_main_cnt ? top.md_hold_main_sum[1]/top.md_hold_main_cnt : 0.0,
                   top.md_hold_main_cnt ? top.md_hold_main_sum[2]/top.md_hold_main_cnt : 0.0,
                   top.md_hold_main_max[0], top.md_hold_main_max[1], top.md_hold_main_max[2]);
            printf("  SUSP entries: n=%lu  avg W1=%.1f W2=%.1f W3=%.1f ns  |  max %.1f/%.1f/%.1f\n",
                   (unsigned long)top.md_hold_susp_cnt,
                   top.md_hold_susp_cnt ? top.md_hold_susp_sum[0]/top.md_hold_susp_cnt : 0.0,
                   top.md_hold_susp_cnt ? top.md_hold_susp_sum[1]/top.md_hold_susp_cnt : 0.0,
                   top.md_hold_susp_cnt ? top.md_hold_susp_sum[2]/top.md_hold_susp_cnt : 0.0,
                   top.md_hold_susp_max[0], top.md_hold_susp_max[1], top.md_hold_susp_max[2]);
            printf("======================================================\n");
        }
        top.print_cache_statistics();
        stopped=true; phase_event.notify(SC_ZERO_TIME); ready_event.notify(SC_ZERO_TIME);
        for(auto& d:devices) d.space_event.notify(SC_ZERO_TIME);
        std::cout << "[MULTIDEV_RESULT] PASS expected=" << N*count(iommu_md::MEASUREMENT_EPOCH)
                  << " completed=" << N*count(iommu_md::MEASUREMENT_EPOCH) << "\n";
        sc_stop();
    }
};
} // namespace

void RP_Module::send_translation_request_1_thread() {
    auto scenario=std::make_shared<MultiDeviceScenario>(*this);
    md_response_handler=[scenario](tlm_generic_payload& trans){ scenario->response(trans); };
    scenario->run();
}
#endif
