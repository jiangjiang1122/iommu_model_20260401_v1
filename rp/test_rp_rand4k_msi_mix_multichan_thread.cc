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

// ============================================================================
// 单设备多通道激励注入场景 (Multi-Channel Scenario)
//
// 目标: 在场景13-v4 单设备基线之上, 把"1个设备顺序发 GROUPS 组(每组8x512B连续
//       + SQ/CQ/MSI)"改为"同一设备内 n 路并发通道, 每通道各自在 IOVA 范围内随机
//       选 4KB, 组内 8x512B 地址连续, 控制包不变, 逐包轮询仲裁后经统一输出口注入"。
//
// 关键设计(与多设备三进程模型同构, 但全部通道共享 device_id=0):
//   - n 个 producer 进程: 各自把本通道 Request 流压入自有有界队列(反压), 不调用入口;
//   - 1 个 RR scheduler: 唯一 nb_transport_fw 调用者, 逐包轮询(每次取1个Request),
//     保证入口 4ns(数据)/2ns(控制) 阻塞节拍与单流基线逐包一致;
//   - 1 个 watchdog: 沿用多设备超时诊断。
//   - 统计只按设备(=1个)聚合(md_trace), 无需按通道追踪。
//
// 本文件仅在 TEST=rand4k_msi_mix_multichan 时经 TEST_THREAD_SRC 编入; 其余场景不编译,
// 故 send_translation_request_1_thread 无重复定义, 对基线零影响。
// 复用多设备追踪(md_trace/md_response_handler), 需 Makefile 同时置
// TEST_CFG_MULTI_DEVICE_SCENE=1 + TEST_CFG_NUM_DEVICES=1。
// ============================================================================

// ---- 多通道场景编译宏安全默认(Makefile 正常注入; 此处仅兜底, 不改共享头) ----
#ifndef TEST_CFG_MULTI_CHANNEL_SCENE
#define TEST_CFG_MULTI_CHANNEL_SCENE 0
#endif
#ifndef TEST_CFG_NUM_CHANNELS
#define TEST_CFG_NUM_CHANNELS 8
#endif
#ifndef TEST_CFG_MC_QUEUE_DEPTH
#define TEST_CFG_MC_QUEUE_DEPTH 32
#endif
#ifndef TEST_CFG_MC_RR_START
#define TEST_CFG_MC_RR_START 0
#endif
#ifndef TEST_CFG_MC_LOAD_MODE            // 0=per_channel(每通道GROUPS组); 1=fixed_total(总组数=NUM_PAGES)
#define TEST_CFG_MC_LOAD_MODE 0
#endif
#ifndef TEST_CFG_NUM_PAGES               // 兜底: 每通道组数(per_channel)或总组数(fixed_total)
#define TEST_CFG_NUM_PAGES 909
#endif

// 多通道复用多设备追踪(md_trace/md_response_handler), 必须同时开启 MULTI_DEVICE_SCENE。
#if TEST_CFG_MULTI_CHANNEL_SCENE && !TEST_CFG_MULTI_DEVICE_SCENE
#error "TEST_CFG_MULTI_CHANNEL_SCENE 需要 TEST_CFG_MULTI_DEVICE_SCENE=1 (复用 md_trace)"
#endif

#if TEST_CFG_MULTI_CHANNEL_SCENE
namespace {
using iommu_md::require;
constexpr unsigned NCHAN = TEST_CFG_NUM_CHANNELS;
constexpr unsigned QUEUE_DEPTH = TEST_CFG_MC_QUEUE_DEPTH;
constexpr bool FIXED_TOTAL = (TEST_CFG_MC_LOAD_MODE != 0);
constexpr unsigned RR_START = TEST_CFG_MC_RR_START;
// 负载口径:
//   per_channel(FIXED_TOTAL=0): 每通道 NUM_PAGES 组 + 1 tail MSI = 10000 包/通道, 总 n x 10000 包;
//   fixed_total (FIXED_TOTAL=1): 全部通道合计 NUM_PAGES(909) 组, 余数分给前 GROUPS_REM 个通道
//               (n=8: 114x5 + 113x3), 末尾 1 个 tail MSI 只给通道0 -> 总包数 = 909x11 + 1 = 10000,
//               与单流基线逐包同量。默认口径: 总包数=基线, 用于对比性能变化;
//               且工作集小(建表~3.5k页), 可规避 per_channel 大工作集触发的两阶段预取无效页问题。
constexpr unsigned GROUPS_BASE = FIXED_TOTAL ? (TEST_CFG_NUM_PAGES / NCHAN) : TEST_CFG_NUM_PAGES;
constexpr unsigned GROUPS_REM  = FIXED_TOTAL ? (TEST_CFG_NUM_PAGES % NCHAN) : 0u;
constexpr unsigned MAX_CHAN_GROUPS = GROUPS_BASE + (GROUPS_REM > 0u ? 1u : 0u);
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
static_assert(NCHAN >= 1 && NCHAN <= 16, "通道数必须为1..16");
static_assert(QUEUE_DEPTH > 0 && QUEUE_DEPTH <= 1048576, "就绪队列深度非法");
static_assert(MAX_CHAN_GROUPS > 0 && MAX_CHAN_GROUPS <= RANGE_PAGES, "组数超过候选页数");
static_assert(PT_DEDUP_PREFETCH_DEPTH <= 16, "预取深度超过结果槽容量");
static_assert(RR_START < NCHAN, "轮询起点非法");
static_assert(!FIXED_TOTAL || NCHAN == 1 || NCHAN == 2 || NCHAN == 4 || NCHAN == 8 || NCHAN == 16,
              "等总负载模式要求n为1/2/4/8/16");

// 单一共享设备: SPA 偏移与 MSI PA 均取 device_id=0。
uint64_t spa_offset(unsigned d) { return 0x80000000ULL + uint64_t(d) * 0x10000000ULL; }
uint64_t msi_pa(unsigned d, unsigned v) { return 0x40000000ULL + uint64_t(d)*0x100000ULL + uint64_t(v)*4096 + 0x40; }

// 每通道组数/tail数: fixed_total 下前 GROUPS_REM 个通道各多1组, 且仅通道0补1个tail MSI,
// 使 total_count(3) 精确等于 NUM_PAGES*11+1 (=10000, 与单流基线一致)。
constexpr unsigned chan_groups(unsigned c) { return GROUPS_BASE + (c < GROUPS_REM ? 1u : 0u); }
constexpr unsigned chan_tails(unsigned c) { return FIXED_TOTAL ? (c == 0u ? 1u : 0u) : 1u; }
constexpr uint64_t chan_count(unsigned c, unsigned e) {
    return e == 1 ? 2 : e == 2 ? 16 : uint64_t(chan_groups(c)) * 11 + chan_tails(c);
}
constexpr uint64_t total_count(unsigned e) {
    uint64_t s = 0;
    for (unsigned c = 0; c < NCHAN; ++c) s += chan_count(c, e);
    return s;
}

// 每通道只持有独立的随机页选择与就绪队列; 设备上下文/VS映射/MSI/控制包全场景共享。
struct Channel {
    std::vector<unsigned> pages;
    std::deque<iommu_md::Request> queue;
    sc_event space_event;
    bool ready = false, done = false;
    uint64_t injected = 0, peak_queue = 0, full_waits = 0;
    double full_wait_ns = 0;
};

// 所有SystemC进程属于单个RP，只有scheduler访问唯一的initiator socket。
class MultiChannelScenario {
    RP_Module& rp;
    iommu_top& top;
    // ---- 单一共享设备状态(device_id=0, GSCID=1) ----
    device_context_t dc{};
    uint64_t dc_address = 0, msi_iova = 0, probe_gpa = 0;
    std::vector<unsigned> msi_pages;
    std::vector<uint64_t> gpas;               // 共享VS映射: gpas[RANGE_PAGES]
    // ---- n 路并发通道 ----
    std::array<Channel, NCHAN> channels;
    uint64_t total_responded = 0;
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
    void map_vs(uint64_t iova, uint64_t gpa) {
        spte_t p{};
        p.V=p.R=p.W=p.U=p.A=p.D=1;
        p.PBMT=PMA;
        p.PPN=gpa/4096;
        require(rp.add_vs_stage_pte(&top,dc.fsc.iosatp,iova,p,0,dc.iohgatp,0)!=uint64_t(-1),
                "构建VS页表失败");
    }
    void map_g(uint64_t gpa, uint64_t spa, unsigned level) {
        gpte_t p{};
        p.V=p.R=p.W=p.U=p.A=p.D=1;
        p.PBMT=PMA;
        p.PPN=spa/4096;
        require(rp.add_g_stage_pte(&top,dc.iohgatp,gpa,p,level)!=uint64_t(-1), "构建G-stage失败");
    }
    void setup() {
        // 保留现有低地址寄存器/CQ区域，只初始化一次物理分配游标。
        next_free_page=std::max<uint64_t>(next_free_page,240);
        // 单设备共享页表预算(远小于多设备 N x): 每2MiB IOVA区一张L0表 + 目录/G-stage/MSI裕量。
        const uint64_t device_pages=(uint64_t(RANGE_MB)+2)/2+64;
        require((next_free_page+1+device_pages)*4096 <= DDR_Module::DDR_MEMORY_SIZE,
                "单设备页表预算超过32MiB DDR");
        require(rp.enable_iommu(&top,DDT_1LVL)>=0,"启用IOMMU失败");

        // ---- 单一共享设备上下文(device_id=0, GSCID=1, Sv48x4/Sv48, MSIPTP Flat) ----
        next_free_gpage[1]=0x10000;
        probe_gpa=0x100000ULL;
        msi_pages=choose(256,1024,777,777);
        msi_iova=MSI_IOVA_BASE+uint64_t(msi_pages[0])*4096+0x40;
        dc_address=rp.add_device(&top,0,1,0,0,0,0,0,1,1,0,0,0,
                                 IOHGATP_Sv48x4,IOSATP_Sv48,PDTP_Bare,MSIPTP_Flat,1,0xFF,MSI_PATTERN);
        require(dc_address!=uint64_t(-1),"添加设备失败");
        rp.read_memory_test_rp(dc_address,sizeof(dc),reinterpret_cast<char*>(&dc));
        dc.ta.PSCID=0;
        rp.write_memory_test_rp(reinterpret_cast<char*>(&dc),dc_address,sizeof(dc));
        device_context_t readback{};
        rp.read_memory_test_rp(dc_address,sizeof(readback),reinterpret_cast<char*>(&readback));
        require(readback.iohgatp.GSCID==1 && readback.ta.PSCID==0 && readback.tc.PDTV==0,
                "DC读回上下文不匹配");

        // G-stage: HUGE_COUNT 个 2MB 大页(GPA 0~RANGE), SPA=GPA+spa_offset(0)。
        for(unsigned h=0;h<HUGE_COUNT;++h) map_g(uint64_t(h)*HUGE,uint64_t(h)*HUGE+spa_offset(0),1);
        // MSIPT(Flat, 256 vectors) + MSI IOVA->GPA 映射, 全通道共享。
        for(unsigned v=0;v<256;++v) {
            msipte_t mp{};
            mp.V=1; mp.M=3; mp.translate_rw.PPN=msi_pa(0,v)/4096;
            rp.write_memory_test_rp(reinterpret_cast<char*>(&mp),dc.msiptp.PPN*4096+v*16,16);
            map_vs(MSI_IOVA_BASE+uint64_t(msi_pages[v])*4096,(MSI_PATTERN<<12)+uint64_t(v)*4096);
        }
        // 共享 VS 映射 gpas[RANGE_PAGES]: 512MB 多对一随机 GPA(同基线)。
        gpas.resize(RANGE_PAGES);
        std::mt19937 mapping(42);
        std::array<unsigned,HUGE_COUNT> slots{};
        for(unsigned i=0;i<RANGE_PAGES;++i) {
            unsigned h=mapping()%HUGE_COUNT, slot=0;
            if(RANGE_MB==16) {
                while(slots[h]>=512) h=(h+1)%HUGE_COUNT;
                slot=slots[h]++;
            } else slot=mapping()%512;
            gpas[i]=uint64_t(h)*HUGE+uint64_t(slot)*4096;
        }
        // 每通道独立随机选 chan_groups(c) 个 4KB 页(确定性可复现, 通道间可重叠)。
        for(unsigned c=0;c<NCHAN;++c)
            channels[c].pages=choose(chan_groups(c),RANGE_PAGES,2024+c,123+c);
        // VS 建表 = 所有通道 pages 的并集 + 各自 D 个预取相邻页。
        std::set<unsigned> mapped;
        if(RANGE_MB==16) {
            for(unsigned i=0;i<RANGE_PAGES;++i) mapped.insert(i);
        } else {
            for(unsigned c=0;c<NCHAN;++c)
                for(unsigned page:channels[c].pages) {
                    unsigned vpn0=((IOVA_BASE>>12)+page)&511;
                    unsigned depth=std::min<unsigned>(PT_DEDUP_PREFETCH_DEPTH,511-vpn0);
                    for(unsigned k=0;k<=depth && page+k<RANGE_PAGES;++k) mapped.insert(page+k);
                }
        }
        for(unsigned page:mapped) map_vs(IOVA_BASE+uint64_t(page)*4096,gpas[page]);
        // 控制包共享且不变: SQ(32B读)/CQ(16B写) 只映射一次, 全通道复用。
        map_vs(SQ_IOVA,CTRL_GPA);
        map_vs(CQ_IOVA,CTRL_GPA+4096);
        map_g(CTRL_GPA,CTRL_GPA+spa_offset(0),0);
        map_g(CTRL_GPA+4096,CTRL_GPA+4096+spa_offset(0),0);
        // 独立哨兵 IOVA(不计入正式性能请求) + 预取相邻页占位。
        map_vs(PROBE_IOVA,probe_gpa);
        for(uint64_t base:{SQ_IOVA,CQ_IOVA,PROBE_IOVA}) {
            unsigned depth=std::min<unsigned>(PT_DEDUP_PREFETCH_DEPTH,511-((base>>12)&511));
            for(unsigned k=1;k<=depth;++k) {
                uint64_t page=base+uint64_t(k)*4096;
                if(page==SQ_IOVA || page==CQ_IOVA || page==PROBE_IOVA) continue;
                map_vs(page,0x200000ULL+uint64_t(k)*4096);
            }
        }
        uint64_t digest=1469598103934665603ULL;
        for(unsigned c=0;c<NCHAN;++c)
            for(unsigned page:channels[c].pages) digest=(digest^page)*1099511628211ULL;
        std::cout << "[MC_SETUP] did=0 GSCID=1 PSCID=0 channels=" << NCHAN
                  << " groups_base=" << GROUPS_BASE << " groups_rem=" << GROUPS_REM
                  << " mapped_pages=" << mapped.size()
                  << " sequence_hash=" << digest << " root=" << dc.iohgatp.PPN
                  << " free_ppn=" << next_free_page << '\n';
        top.md_trace.progress();
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
    iommu_md::Request descriptor(unsigned c,unsigned e,uint64_t seq) {
        auto& chan=channels[c];
        iommu_md::Request r;
        r.device=0; r.epoch=e; r.sequence=seq;   // 全部通道同属 device_id=0。
        if(e==1) {
            r.bytes=512; r.iova=PROBE_IOVA; r.expected_pa=probe_gpa+spa_offset(0);
            return r;
        }
        unsigned type;
        if(e==2) type=seq<8 ? 3 : (seq%2 ? 2 : 1);   // 预热: MSI(x8) + SQ/CQ 交替。
        else {
            r.group=seq/11;
            unsigned offset=seq%11;
            type=seq>=uint64_t(chan_groups(c))*11 ? 3 : offset<8 ? 0 : offset-7;
            if(type==0) {
                unsigned page=chan.pages[r.group];
                r.iova=IOVA_BASE+uint64_t(page)*4096+offset*512;
                r.expected_pa=gpas[page]+spa_offset(0)+offset*512;
                r.bytes=512; r.write=(r.group%2)!=0;   // 奇读偶写交替(组号%2)。
            }
        }
        r.type=type;
        if(type==1) { r.iova=SQ_IOVA; r.expected_pa=CTRL_GPA+spa_offset(0); r.bytes=32; }
        if(type==2) { r.iova=CQ_IOVA; r.expected_pa=CTRL_GPA+4096+spa_offset(0); r.bytes=16; r.write=true; }
        if(type==3) { r.iova=msi_iova; r.expected_pa=msi_pa(0,0); r.bytes=4; r.write=true; }
        return r;
    }
    void producer(unsigned c) {
        unsigned previous=0;
        while(!stopped) {
            while(epoch==previous && !stopped) wait(phase_event);
            if(stopped) return;
            const unsigned current=epoch;
            auto& chan=channels[c];
            for(uint64_t seq=0;seq<chan_count(c,current);++seq) {
                if(chan.queue.size()>=QUEUE_DEPTH) {
                    double begin=iommu_md::now_ns(); ++chan.full_waits;
                    while(chan.queue.size()>=QUEUE_DEPTH && !stopped) wait(chan.space_event);
                    chan.full_wait_ns+=iommu_md::now_ns()-begin;
                }
                if(stopped) return;
                auto r=descriptor(c,current,seq);
                r.ready_ns=iommu_md::now_ns();
                chan.queue.push_back(r);
                chan.peak_queue=std::max<uint64_t>(chan.peak_queue,chan.queue.size());
                chan.ready=true;
                ready_event.notify(SC_ZERO_TIME);
            }
            chan.done=true; previous=current;
            ready_event.notify(SC_ZERO_TIME);
        }
    }
    void scheduler() {
        unsigned previous=0;
        while(!stopped) {
            while(epoch==previous && !stopped) wait(phase_event);
            if(stopped) return;
            unsigned current=epoch,next=RR_START;
            while(true) {
                bool all=true; for(auto& c:channels) all=all && (c.ready || c.done);
                if(all) break;
                wait(ready_event);
            }
            while(!stopped) {
                int selected=-1;
                for(unsigned k=0;k<NCHAN;++k) {
                    unsigned c=(next+k)%NCHAN;
                    if(!channels[c].queue.empty()) { selected=c; break; }
                }
                if(selected<0) {
                    bool done=true; for(auto& c:channels) done=done && c.done;
                    if(done) break;
                    wait(ready_event); continue;
                }
                unsigned c=static_cast<unsigned>(selected);
                auto& chan=channels[c];
                auto record=chan.queue.front(); chan.queue.pop_front();
                chan.space_event.notify(SC_ZERO_TIME);
                require(record.epoch==current && record.sequence==chan.injected,"队列内请求顺序错误");
                record.grant_ns=iommu_md::now_ns();
                auto trans=std::make_unique<tlm_generic_payload>();
                trans->set_address(record.iova);
                trans->set_data_ptr(new unsigned char[record.bytes]());
                trans->set_data_length(record.bytes);
                trans->set_streaming_width(record.bytes);
                trans->set_command(record.write ? TLM_WRITE_COMMAND : TLM_READ_COMMAND);
                trans->set_response_status(TLM_INCOMPLETE_RESPONSE);
                auto* ext=new PayloadExtention();
                ext->requester_id=0; ext->pid_valid=0; ext->process_id=0;   // 单一设备 device_id=0。
                ext->exec_req=0; ext->priv_req=0; ext->no_write=record.write ? 0 : 1;
                ext->at=0; ext->is_ctrl=record.type!=0;
                trans->set_extension(ext);
                auto* ptr=trans.get();
                payloads.push_back(std::move(trans));
                top.md_trace.register_request(ptr,record);
                sc_time delay=SC_ZERO_TIME; tlm_phase phase=BEGIN_REQ;
                auto status=rp.axi_master_to_pcie_noc_0_socket->nb_transport_fw(*ptr,phase,delay);
                require(status==TLM_UPDATED && phase==END_REQ,"入口握手异常，禁止重发");
                ++chan.injected;
                next=(c+1)%NCHAN;
                std::cout << "[MD_GRANT] epoch=" << current << " chan=" << c << " did=0 seq=" << record.sequence
                          << " type=" << record.type << " grant=" << record.grant_ns
                          << " accept=" << iommu_md::now_ns() << '\n';
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
                for(unsigned c=0;c<NCHAN;++c) std::cerr << "[MC_QUEUE] chan=" << c << " queued=" << channels[c].queue.size()
                    << " sent=" << channels[c].injected << '\n';
                std::cerr << "[MC_STATE] responded=" << total_responded
                          << " buffer=" << top.cache_sub.get_pt_dedup_buffer()->get_valid_count()
                          << " groups=" << top.prefetch_groups.size() << " ptw=" << top.ptw_active_walks.size() << '\n';
                require(false,"多通道测试超时，未释放在途payload");
            }
        }
    }
    void run_epoch(unsigned e) {
        for(auto& c:channels) {
            require(c.queue.empty(),"切换epoch时队列非空");
            c.ready=c.done=false; c.injected=c.peak_queue=c.full_waits=0; c.full_wait_ns=0;
        }
        total_responded=0; epoch=e; sent=false; top.md_trace.progress();
        phase_event.notify(SC_ZERO_TIME);
        while(true) {
            if(sent && total_responded==total_count(e) && top.md_quiescent()) break;
            wait(1,SC_NS);
        }
        uint64_t total_injected=0;
        for(unsigned c=0;c<NCHAN;++c) {
            require(channels[c].injected==chan_count(c,e),"发送/完成数量不一致");
            total_injected+=channels[c].injected;
            std::cout << "[MC_RP] epoch=" << e << " chan=" << c << " injected=" << channels[c].injected
                      << " queue_peak=" << channels[c].peak_queue
                      << " full_waits=" << channels[c].full_waits << " full_wait_ns=" << channels[c].full_wait_ns << '\n';
        }
        require(total_responded==total_injected,"响应总数与注入总数不一致");
    }
public:
    explicit MultiChannelScenario(RP_Module& r) : rp(r),top(*r.iommu_ptr) {}
    ~MultiChannelScenario() {
        // 正常结束只在所有响应及异步工作排空后清理；异常停止不提前清理。
        for(auto& trans:payloads) {
            PayloadExtention* ext=nullptr; trans->get_extension(ext);
            trans->clear_extension(ext); delete ext;
            delete[] trans->get_data_ptr(); trans->set_data_ptr(nullptr);
        }
    }
    void response(tlm_generic_payload& trans) {
        PayloadExtention* ext=nullptr; trans.get_extension(ext);
        require(ext && ext->requester_id<1,"响应缺少有效设备ID");
        auto& r=top.md_trace.request(&trans);
        require(r.epoch==epoch,"收到旧epoch响应");
        top.md_trace.response(trans,0);   // 单一设备聚合(did=0)。
        ++total_responded;
    }
    void run() {
        wait(20,SC_NS);
        start_ns=last_progress_ns=iommu_md::now_ns();
        std::cout << "[MC_CONFIG] n=" << NCHAN << " IOVA_MB=" << RANGE_MB
                  << " groups_base=" << GROUPS_BASE << " groups_rem=" << GROUPS_REM
                  << " queue=" << QUEUE_DEPTH << " D=" << PT_DEDUP_PREFETCH_DEPTH
                  << " rr_start=" << RR_START << " load_mode=" << (FIXED_TOTAL ? "fixed_total" : "per_channel")
                  << " total_meas_pkts=" << total_count(iommu_md::MEASUREMENT_EPOCH) << '\n';
        setup();
        for(unsigned c=0;c<NCHAN;++c) sc_spawn([this,c](){ producer(c); });
        sc_spawn([this](){ scheduler(); });
        sc_spawn([this](){ watchdog(); });
        invalidate_all();
        run_epoch(1); // 哨兵: 打通翻译/响应路由(单设备, 全通道同结果)。
        invalidate_all();
        run_epoch(2); // 预热不进入正式统计(MSIPT + SQ/CQ PT Cache)。
        const uint64_t ptw_before=top.ptw_total_completed, ddr_before=top.axi_master_1_read_req_total;
        run_epoch(iommu_md::MEASUREMENT_EPOCH);
        top.md_trace.report("multichan_n"+std::to_string(NCHAN)+"_"+std::to_string(RANGE_MB)+"mb");
        std::cout << "[MC_DELTA] PTW=" << top.ptw_total_completed-ptw_before
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
        for(auto& c:channels) c.space_event.notify(SC_ZERO_TIME);
        std::cout << "[MULTICHAN_RESULT] PASS expected=" << total_count(iommu_md::MEASUREMENT_EPOCH)
                  << " completed=" << total_responded << "\n";
        sc_stop();
    }
};
} // namespace

void RP_Module::send_translation_request_1_thread() {
    auto scenario=std::make_shared<MultiChannelScenario>(*this);
    md_response_handler=[scenario](tlm_generic_payload& trans){ scenario->response(trans); };
    scenario->run();
}
#endif
