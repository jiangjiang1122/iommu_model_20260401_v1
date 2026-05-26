#include "test_rp.hh"
#include "iommu_struct.hh"
#include "iommu_registers.hh"
#include "iommu_utils.hh"
#include "iommu_top.hh"  // 包含完整的iommu_top定义
#include <cstdint>

#include <iostream>
using namespace std;

uint8_t RP_Module::read_memory_test_rp(uint64_t addr, uint8_t size, char *data)
{
    memcpy(data, &ddr_ptr->memory[addr], size);
    return 0;
}

uint8_t RP_Module::write_memory_test_rp(char *data, uint64_t addr, uint32_t size)
{
    // 如果是写入 DC 区域，打印详细信息
    if (addr <= 0x200 && addr + size >= 0x140) {
        printf("[WRITE_MEMORY_RP] Writing to addr=0x%lx, size=%d, data[0]=0x%lx\n", 
               addr, size, *(uint64_t*)data);
    }
    memcpy(&ddr_ptr->memory[addr], data, size);
    return 0;
}

void RP_Module::iommu_translate_iova_rp(iommu_top *iommu,hb_to_iommu_req_t *req, iommu_to_hb_rsp_t *rsp_msg)
{
    printf("[DEBUG_IOVA] Entering iommu_translate_iova_rp\n");
    fflush(stdout);
    // 创建一个TLM传输事务
    tlm_generic_payload trans;
    sc_time delay = SC_ZERO_TIME;
                        
    // 设置传输地址（IOVA）
    sc_dt::uint64 addr = req->tr.iova;
    trans.set_address(addr);
                        
    // 设置数据指针和长度
    unsigned char data[1024] = {0};
    trans.set_data_ptr(data);
    trans.set_data_length(req->tr.length);
                        
    // 设置命令为读取或写入 (基于 read_writeAMO，而不是 no_write)
    tlm_command cmd = (req->tr.read_writeAMO == READ) ? TLM_READ_COMMAND : TLM_WRITE_COMMAND;
    trans.set_command(cmd);
                        
    // 创建扩展负载
    PayloadExtention* ext = new PayloadExtention();
    ext->requester_id = req->device_id;   // 使用正确的字段名
    ext->pid_valid = req->pid_valid;      // PID有效标志
    ext->process_id = req->process_id;    // 进程ID
    ext->exec_req = req->exec_req;        // 执行请求标志
    ext->priv_req = req->priv_req;        // 特权请求标志
    ext->at = req->tr.at;                 // 地址类型

    // 将扩展添加到传输中
    trans.set_extension(ext);

    // 发送传输请求到 IOMMU (AT non-blocking mode)
    send_translation_request_at(trans);
    
    cout << "[RP Module] Transaction completed - at:" << (int)ext->at
                                  << ", pid_valid:" << (int)ext->pid_valid
                                  << ", exec_req:" << (int)ext->exec_req
                                  << ", priv_req:" << (int)ext->priv_req
                                  << ", no_write:" << (int)req->no_write
                                  << ", read_writeAMO:" << (int)req->tr.read_writeAMO
                                  << ", response: " << trans.get_response_string() << endl;
    
    // TODO : get resp from trans
    // 注意：不要 delete ext，因为 TLM 事务可能仍然持有它的引用
    // ext 的生命周期由 TLM 框架管理，或者应该使用智能指针
    // delete ext;  // 已注释，避免 double-free 或使用后释放
                            
    // 从TLM响应中提取翻译结果
    rsp_msg->status = (trans.get_response_status() == tlm::TLM_OK_RESPONSE) ? SUCCESS : UNSUPPORTED_REQUEST;
    rsp_msg->trsp.PPN = trans.get_address() / PAGESIZE;
    rsp_msg->trsp.pa = trans.get_address();
    rsp_msg->trsp.R = 1;
    rsp_msg->trsp.W = 1;
    
    printf("[DEBUG_IOVA] TLM response addr=0x%lx, status=%s, extracted PA=0x%lx\n",
           (uint64_t)trans.get_address(), trans.get_response_string().c_str(), rsp_msg->trsp.PPN * PAGESIZE);
    fflush(stdout);
    printf("[DEBUG_IOVA] Exiting iommu_translate_iova_rp\n");
    fflush(stdout);
}


int8_t RP_Module::check_faults_rp(
    iommu_top *iommu,
    uint16_t cause, uint8_t  exp_PV, uint32_t exp_PID, uint8_t  exp_PRIV,
    uint32_t exp_DID, uint64_t exp_iotval, uint8_t ttyp, uint64_t exp_iotval2) {
    fault_rec_t fault_rec;
    fqb_t fqb;
    fqh_t fqh;

    fqh.raw = read_register(&iommu->iommu_inst, FQH_OFFSET, 4);
    if ( (fqh.raw >= read_register(&iommu->iommu_inst, FQT_OFFSET, 4)) && (cause != 0) ) {
        printf("No faults logged\n");
        return -1;
    }
    if ( (fqh.raw < read_register(&iommu->iommu_inst, FQT_OFFSET, 4)) && (cause == 0) ) {
        printf("Unexpected fault logged\n");
        return -1;
    }

    fqb.raw = read_register(&iommu->iommu_inst, FQB_OFFSET, 8);
    read_memory_test_rp(((fqb.ppn * PAGESIZE) | (fqh.index * FQ_ENTRY_SZ)), FQ_ENTRY_SZ, (char *)&fault_rec);

    // pop the fault record
    fqh.index++;
    write_register(&iommu->iommu_inst, FQH_OFFSET, 4, fqh.raw);

    if ( fault_rec.CAUSE != cause || fault_rec.DID != exp_DID ||
         fault_rec.iotval != exp_iotval ||
         fault_rec.iotval2 != exp_iotval2 ||
         fault_rec.TTYP != ttyp ||
         fault_rec.reserved != 0 ) {
        printf("Bad fault record\n");
        return -1;
    }
    if ( (exp_PV != fault_rec.PV) ||
         (exp_PV && ((fault_rec.PID != exp_PID) ||
         (fault_rec.PRIV != exp_PRIV))) ) {
        printf("Bad fault record\n");
        return -1;
    }
    return 0;
}

int8_t RP_Module::check_rsp_and_faults_rp(
    iommu_top *iommu,
    hb_to_iommu_req_t *req,
    iommu_to_hb_rsp_t *rsp,
    status_t status,
    uint16_t cause,
    uint64_t exp_iotval2) {

    fqh_t fqh;
    uint8_t EXP_TTYP;

    EXP_TTYP = TTYPE_NONE;
    if ( req->tr.at == ADDR_TYPE_UNTRANSLATED && req->tr.read_writeAMO == READ ) {
        if ( req->exec_req )
            EXP_TTYP = UNTRANSLATED_READ_FOR_EXECUTE_TRANSACTION;
        else
            EXP_TTYP = UNTRANSLATED_READ_TRANSACTION;
    }
    if ( req->tr.at == ADDR_TYPE_UNTRANSLATED && req->tr.read_writeAMO == WRITE )
        EXP_TTYP = UNTRANSLATED_WRITE_AMO_TRANSACTION;
    if ( req->tr.at == ADDR_TYPE_TRANSLATED && req->tr.read_writeAMO == READ ) {
        if ( req->pid_valid && req->exec_req )
            EXP_TTYP = TRANSLATED_READ_FOR_EXECUTE_TRANSACTION;
        else
            EXP_TTYP = TRANSLATED_READ_TRANSACTION;
    }
    if ( req->tr.at == ADDR_TYPE_TRANSLATED && req->tr.read_writeAMO == WRITE )
        EXP_TTYP = TRANSLATED_WRITE_AMO_TRANSACTION;
    if ( req->tr.at == ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST )
        EXP_TTYP = PCIE_ATS_TRANSLATION_REQUEST;
    if ( rsp->status != status ) return -1;

    fqh.raw = read_register(&iommu->iommu_inst, FQH_OFFSET, 4);
    if ( (fqh.raw >= read_register(&iommu->iommu_inst, FQT_OFFSET, 4)) && (cause != 0) ) {
        printf("No faults logged\n");
        return -1;
    }
    if ( (fqh.raw < read_register(&iommu->iommu_inst, FQT_OFFSET, 4)) && (cause == 0) ) {
        printf("Unexpected fault logged\n");
        return -1;
    }

    if ( cause == 0 ) return 0;
    return check_faults_rp(iommu, cause, req->pid_valid, req->process_id, req->priv_req,
                 req->device_id, req->tr.iova, EXP_TTYP, exp_iotval2);
}

void RP_Module::send_translation_request_rp(iommu_top *iommu, uint32_t did, uint8_t pid_valid, uint32_t pid, uint8_t no_write,
    uint8_t exec_req, uint8_t priv_req, uint8_t is_cxl_dev, uint8_t at, uint64_t iova,
    uint32_t length, uint8_t read_writeAMO,
    hb_to_iommu_req_t *req, iommu_to_hb_rsp_t *rsp) {

    printf("[SEND_REQ] Entering send_translation_request_rp, IOVA=0x%lx\n", iova);
    fflush(stdout);
    std::cout << "[SEND_REQ_COUT] IOVA=0x" << std::hex << iova << std::dec << std::endl;

    req->device_id        = did;
    req->pid_valid        = pid_valid;
    req->process_id       = pid;
    req->no_write         = no_write;
    req->exec_req         = exec_req;
    req->priv_req         = priv_req;
    req->is_cxl_dev       = is_cxl_dev;
    req->tr.at            = static_cast<addr_type_t>(at);
    req->tr.iova          = iova;
    req->tr.length        = length;
    req->tr.read_writeAMO = read_writeAMO;

    iommu_translate_iova_rp(iommu, req, rsp);

    printf("[SEND_REQ] Exiting send_translation_request_rp, status=0x%x\n", rsp->status);
    fflush(stdout);
    std::cout << "[SEND_REQ_COUT] Status=0x" << std::hex << rsp->status << std::dec << std::endl;

    return;
}

uint64_t RP_Module::add_device(iommu_top *iommu, uint32_t device_id, uint32_t gscid, uint8_t en_ats, uint8_t en_pri, uint8_t t2gpa,
           uint8_t dtf, uint8_t prpr,
           uint8_t gade, uint8_t sade, uint8_t dpe, uint8_t sbe, uint8_t sxl,
           uint8_t iohgatp_mode, uint8_t iosatp_mode, uint8_t pdt_mode,
           uint8_t msiptp_mode, uint8_t msiptp_pages, uint64_t msi_addr_mask,
           uint64_t msi_addr_pattern) {
    device_context_t DC;
    char zero[16384];
    memset(zero, 0, 16384);
    memset(&DC, 0, sizeof(DC));
    
    printf("[ADD_DEVICE] Called with iohgatp_mode=%d, iosatp_mode=%d\n", iohgatp_mode, iosatp_mode);

    DC.tc.V      = 1;
    DC.tc.EN_ATS = en_ats;
    DC.tc.EN_PRI = en_pri;
    DC.tc.T2GPA  = t2gpa;
    DC.tc.DTF    = dtf;
    DC.tc.PRPR   = prpr;
    DC.tc.GADE   = gade;
    DC.tc.SADE   = sade;
    DC.tc.DPE    = dpe;
    DC.tc.SBE    = sbe;
    DC.tc.SXL    = sxl;
    if ( iohgatp_mode != IOHGATP_Bare ) {
        DC.iohgatp.GSCID = gscid;
        DC.iohgatp.PPN = get_free_ppn(4);
        printf("[ADD_DEVICE] Allocated iohgatp PPN=0x%lx for mode %d\n", DC.iohgatp.PPN, iohgatp_mode);
        write_memory_test_rp(zero, DC.iohgatp.PPN * PAGESIZE, 16384 );
    }
    DC.iohgatp.MODE = iohgatp_mode;
    printf("[ADD_DEVICE] Set DC.iohgatp.MODE=%d, PPN=0x%lx\n", DC.iohgatp.MODE, DC.iohgatp.PPN);
    if ( iosatp_mode != IOSATP_Bare ) {
        DC.tc.PDTV = 0;
        DC.fsc.iosatp.MODE = iosatp_mode;
        if ( DC.iohgatp.MODE != IOHGATP_Bare ) {
            gpte_t gpte;
            DC.fsc.iosatp.PPN = get_free_gppn(1, DC.iohgatp);
            gpte.raw = 0;
            gpte.V = 1;
            gpte.R = 1;
            gpte.W = 1;
            gpte.X = 0;
            gpte.U = 1;
            gpte.G = 0;
            gpte.A = 0;
            gpte.D = 0;
            gpte.PBMT = PMA;
            gpte.PPN = get_free_ppn(1);
            write_memory_test_rp(zero, gpte.PPN * PAGESIZE, 4096);
            add_g_stage_pte(iommu, DC.iohgatp, (PAGESIZE * DC.fsc.iosatp.PPN), gpte, 0);
        } else {
            DC.fsc.iosatp.PPN = get_free_ppn(1);
            write_memory_test_rp(zero, DC.fsc.iosatp.PPN * PAGESIZE, 4096);
        }
    }
    if ( pdt_mode != PDTP_Bare ) {
        DC.tc.PDTV = 1;
        DC.fsc.pdtp.MODE = pdt_mode;
        if ( DC.iohgatp.MODE != IOHGATP_Bare ) {
            gpte_t gpte;
            DC.fsc.pdtp.PPN = get_free_gppn(1, DC.iohgatp);
            gpte.raw = 0;
            gpte.V = 1;
            gpte.R = 1;
            gpte.W = 1;
            gpte.X = 0;
            gpte.U = 1;
            gpte.G = 0;
            gpte.A = 0;
            gpte.D = 0;
            gpte.PBMT = PMA;
            gpte.PPN = get_free_ppn(1);
            write_memory_test_rp( zero, gpte.PPN * PAGESIZE, 4096);
            add_g_stage_pte(iommu, DC.iohgatp, (PAGESIZE * DC.fsc.pdtp.PPN), gpte, 0);
        } else {
            DC.fsc.pdtp.PPN = get_free_ppn(1);
            write_memory_test_rp( zero, DC.fsc.pdtp.PPN * PAGESIZE, 4096);
        }
    }
    DC.msiptp.MODE = msiptp_mode;
    if ( msiptp_mode != MSIPTP_Off ) {
       DC.msiptp.PPN = get_free_ppn(msiptp_pages);
       write_memory_test_rp( zero, DC.msiptp.PPN * PAGESIZE, 4096);
       DC.msi_addr_mask.mask = msi_addr_mask;
       DC.msi_addr_pattern.pattern = msi_addr_pattern;
    }
    return add_dev_context(iommu, &DC, device_id);
}

uint64_t RP_Module::get_free_ppn(
    uint64_t num_ppn) {
    uint64_t free_ppn;
    if ( next_free_page & (num_ppn -1) ) {
        next_free_page = next_free_page + (num_ppn -1);
        next_free_page = next_free_page & ~(num_ppn -1);
    }
    free_ppn = next_free_page;
    next_free_page += num_ppn;
    memset(&ddr_ptr->memory[free_ppn * PAGESIZE], 0, num_ppn * PAGESIZE);
    return free_ppn;
}

uint64_t RP_Module::get_free_gppn(uint64_t num_gppn, iohgatp_t iohgatp) {
    uint64_t free_gppn = next_free_gpage[iohgatp.GSCID];

    if ( free_gppn & (num_gppn -1) ) {
        free_gppn = free_gppn + (num_gppn -1);
        free_gppn = free_gppn & ~(num_gppn -1);
    }
    next_free_gpage[iohgatp.GSCID] = free_gppn + num_gppn;
    return free_gppn;
}

uint64_t RP_Module::add_g_stage_pte (
    iommu_top *iommu,
    iohgatp_t iohgatp, uint64_t gpa, gpte_t gpte, uint8_t add_level) {

    uint16_t vpn[5];
    uint64_t a;
    uint8_t i, PTESIZE, LEVELS;
    gpte_t nl_gpte;

    PTESIZE = 8;
    if ( iohgatp.MODE == IOHGATP_Sv32x4 && iommu->iommu_inst.reg_file.fctl.gxl == 1 ) {
        vpn[0] = get_bits(21, 12, gpa);
        vpn[1] = get_bits(34, 22, gpa);
        LEVELS = 2;
        PTESIZE = 4;
    }
    if ( iohgatp.MODE == IOHGATP_Sv39x4 && iommu->iommu_inst.reg_file.fctl.gxl == 0) {
        vpn[0] = get_bits(20, 12, gpa);
        vpn[1] = get_bits(29, 21, gpa);
        vpn[2] = get_bits(40, 30, gpa);
        LEVELS = 3;
    }
    if ( iohgatp.MODE == IOHGATP_Sv48x4 ) {
        vpn[0] = get_bits(20, 12, gpa);
        vpn[1] = get_bits(29, 21, gpa);
        vpn[2] = get_bits(38, 30, gpa);
        vpn[3] = get_bits(49, 39, gpa);
        LEVELS = 4;
    }
    if ( iohgatp.MODE == IOHGATP_Sv57x4 ) {
        vpn[0] = get_bits(20, 12, gpa);
        vpn[1] = get_bits(29, 21, gpa);
        vpn[2] = get_bits(38, 30, gpa);
        vpn[3] = get_bits(47, 39, gpa);
        vpn[4] = get_bits(58, 48, gpa);
        LEVELS = 5;
    }
    
    printf("[ADD_G_STAGE_PTE] GPA=0x%lx, add_level=%d, LEVELS=%d\n", gpa, add_level, LEVELS);
    printf("[ADD_G_STAGE_PTE] VPN values: [0]=%d, [1]=%d, [2]=%d, [3]=%d\n", vpn[0], vpn[1], vpn[2], vpn[3]);
    
    i = LEVELS - 1;
    a = iohgatp.PPN * PAGESIZE;
    printf("[ADD_G_STAGE_PTE] Starting walk: i=%d, root_addr=0x%lx (PPN=0x%lx)\n", i, a, iohgatp.PPN);
    
    while ( i > add_level ) {
        nl_gpte.raw = 0;
        printf("[ADD_G_STAGE_PTE] Reading PTE at level %d, addr=0x%lx (vpn[%d]=%d)\n", i, (a | (vpn[i] * PTESIZE)), i, vpn[i]);
        if ( read_memory_test_rp( (a | (vpn[i] * PTESIZE)), PTESIZE, (char *)&nl_gpte.raw) ) {
            printf("[ADD_G_STAGE_PTE] ERROR: Failed to read PTE at level %d!\n", i);
            return -1;
        }
        if ( nl_gpte.V == 0 ) {
            printf("[ADD_G_STAGE_PTE] Level %d PTE invalid (V=0), allocating new page...\n", i);
            nl_gpte.V = 1;
            nl_gpte.PPN = get_free_ppn(1);
            printf("[ADD_G_STAGE_PTE] Allocated PPN=0x%lx for level %d\n", nl_gpte.PPN, i);
            if ( write_memory_test_rp( (char *)&nl_gpte.raw, (a | (vpn[i] * PTESIZE)), PTESIZE) ) {
                printf("[ADD_G_STAGE_PTE] ERROR: Failed to write PTE at level %d!\n", i);
                return -1;
            }
        } else {
            printf("[ADD_G_STAGE_PTE] Level %d PTE already valid: 0x%lx\n", i, nl_gpte.raw);
        }
        i = i - 1;
        if ( i < 0 ) {
            printf("[ADD_G_STAGE_PTE] ERROR: i became negative!\n");
            return -1;
        }
        a = nl_gpte.PPN * PAGESIZE;
        printf("[ADD_G_STAGE_PTE] Moving to level %d, next_addr=0x%lx\n", i, a);
    }
    if ( write_memory_test_rp( (char *)&gpte.raw, (a | (vpn[i] * PTESIZE)), PTESIZE) ) return -1;
    return (a | (vpn[i] * PTESIZE));
}


uint64_t RP_Module::add_dev_context(
    iommu_top *iommu,
    device_context_t *DC, uint32_t device_id) {
    uint64_t a;
    uint8_t i, LEVELS, DC_SIZE;
    ddte_t ddte;
    uint16_t DDI[3];
    // The DDT used to locate the DC may be configured to be a 1, 2, or 3 level
    // radix-table depending on the maximum width of the device_id supported.
    // The partitioning of the device_id to obtain the device directory indexes
    // (DDI) to traverse the DDT radix-tree table are as follows:
    if ( iommu->iommu_inst.reg_file.capabilities.msi_flat == 0 ) {
        DDI[0] = get_bits(6,   0, device_id);
        DDI[1] = get_bits(15,  7, device_id);
        DDI[2] = get_bits(23, 16, device_id);
        DC_SIZE = BASE_FORMAT_DC_SIZE;
    } else {
        DDI[0] = get_bits(5,   0, device_id);
        DDI[1] = get_bits(14,  6, device_id);
        DDI[2] = get_bits(23, 15, device_id);
        DC_SIZE = EXT_FORMAT_DC_SIZE;
    }
    a = iommu->iommu_inst.reg_file.ddtp.ppn * PAGESIZE;
    if ( iommu->iommu_inst.reg_file.ddtp.iommu_mode == DDT_3LVL ) LEVELS = 3;
    if ( iommu->iommu_inst.reg_file.ddtp.iommu_mode == DDT_2LVL ) LEVELS = 2;
    if ( iommu->iommu_inst.reg_file.ddtp.iommu_mode == DDT_1LVL ) LEVELS = 1;
    
    printf("[ADD_DEV_CONTEXT] Writing DC for device_id=0x%x, DDI[0]=%d, DDI[1]=%d, DDI[2]=%d\n", 
           device_id, DDI[0], DDI[1], DDI[2]);
    printf("[ADD_DEV_CONTEXT] DC.tc.V=%d, DC.tc.EN_ATS=%d, DC.tc.GADE=%d, DC.tc.SADE=%d\n",
           DC->tc.V, DC->tc.EN_ATS, DC->tc.GADE, DC->tc.SADE);
    printf("[ADD_DEV_CONTEXT] DC_SIZE=%d, DC address=0x%lx\n", DC_SIZE, (a + (DDI[0] * DC_SIZE)));
    
    i = LEVELS - 1;
    while ( i > 0 ) {
        ddte.raw = 0;
        if ( read_memory_test_rp( (a + (DDI[i] * 8)), 8, (char *)&ddte.raw) ) return -1;
        if ( ddte.V == 0 ) {
            ddte.V = 1;
            ddte.PPN = get_free_ppn(1);
            if ( write_memory_test_rp( (char *)&ddte.raw, (a + (DDI[i] * 8)), 8) ) return -1;
            printf("[ADD_DEV_CONTEXT] Created new DDTE at level %d, index %d, PPN=0x%lx\n", i, DDI[i], ddte.PPN);
        }
        i = i - 1;
        a = ddte.PPN * PAGESIZE;
    }
    
    // 写入 DC 之前再次确认
    printf("[ADD_DEV_CONTEXT] About to write DC (size=%d bytes) to address 0x%lx\n", DC_SIZE, (a + (DDI[0] * DC_SIZE)));
    printf("[ADD_DEV_CONTEXT] DC raw data (first 16 bytes): 0x%lx 0x%lx\n", ((uint64_t*)DC)[0], ((uint64_t*)DC)[1]);
    
    if ( write_memory_test_rp( (char *)DC, (a + (DDI[0] * DC_SIZE)), DC_SIZE) ) return -1;
    
    // 立即读回验证
    device_context_t DC_verify;
    memset(&DC_verify, 0, sizeof(DC_verify));
    if ( read_memory_test_rp( (a + (DDI[0] * DC_SIZE)), DC_SIZE, (char *)&DC_verify) ) {
        printf("[ADD_DEV_CONTEXT] ERROR: Failed to read back DC for verification!\n");
        return -1;
    }
    
    printf("[ADD_DEV_CONTEXT] Verification - Read back DC from 0x%lx\n", (a + (DDI[0] * DC_SIZE)));
    printf("[ADD_DEV_CONTEXT] Verification - DC_verify.tc.V=%d (should be 1)\n", DC_verify.tc.V);
    printf("[ADD_DEV_CONTEXT] Verification - DC_verify.tc.EN_ATS=%d\n", DC_verify.tc.EN_ATS);
    printf("[ADD_DEV_CONTEXT] Verification - DC raw data (first 16 bytes): 0x%lx 0x%lx\n", ((uint64_t*)&DC_verify)[0], ((uint64_t*)&DC_verify)[1]);
    
    if ( DC_verify.tc.V != 1 ) {
        printf("[ADD_DEV_CONTEXT] ERROR: DC V bit is not set after write! Write/Read memory test failed.\n");
    } else {
        printf("[ADD_DEV_CONTEXT] SUCCESS: DC written and verified successfully!\n");
    }
    
    return (a + (DDI[0] * DC_SIZE));
}

uint64_t RP_Module::translate_gpa (
    iommu_top *iommu, iohgatp_t iohgatp, uint64_t gpa, uint64_t *spa) {

    uint16_t vpn[5];
    uint64_t a;
    uint8_t i, PTESIZE, LEVELS;
    gpte_t nl_gpte;
    uint64_t gst_page_sz;
    uint8_t gxl = iommu->iommu_inst.reg_file.fctl.gxl;

    PTESIZE = 8;
    if ( iohgatp.MODE == IOHGATP_Bare ) {
        *spa = gpa;
        return -1;
    }
    if ( iohgatp.MODE == IOHGATP_Sv32x4 && gxl == 1) {
        vpn[0] = get_bits(21, 12, gpa);
        vpn[1] = get_bits(34, 22, gpa);
        LEVELS = 2;
        PTESIZE = 4;
        gst_page_sz = 4UL * 1024UL * 1024UL;
    }
    if ( iohgatp.MODE == IOHGATP_Sv39x4 && gxl == 0) {
        vpn[0] = get_bits(20, 12, gpa);
        vpn[1] = get_bits(29, 21, gpa);
        vpn[2] = get_bits(40, 30, gpa);
        gst_page_sz = 512UL * 512UL * PAGESIZE;
        LEVELS = 3;
    }
    if ( iohgatp.MODE == IOHGATP_Sv48x4 ) {
        vpn[0] = get_bits(20, 12, gpa);
        vpn[1] = get_bits(29, 21, gpa);
        vpn[2] = get_bits(38, 30, gpa);
        vpn[3] = get_bits(49, 39, gpa);
        gst_page_sz = 512UL * 512UL * 512UL * PAGESIZE;
        LEVELS = 4;
    }
    if ( iohgatp.MODE == IOHGATP_Sv57x4 ) {
        vpn[0] = get_bits(20, 12, gpa);
        vpn[1] = get_bits(29, 21, gpa);
        vpn[2] = get_bits(38, 30, gpa);
        vpn[3] = get_bits(47, 39, gpa);
        vpn[4] = get_bits(58, 48, gpa);
        gst_page_sz = 512UL * 512UL * 512UL * 512UL * PAGESIZE;
        LEVELS = 5;
    }
    i = LEVELS - 1;
    a = iohgatp.PPN * PAGESIZE;
    while ( 1 ) {
        nl_gpte.raw = 0;
        if ( read_memory_test_rp( (a | (vpn[i] * PTESIZE)), PTESIZE, (char *)&nl_gpte.raw) ) return -1;
        if ( nl_gpte.V == 0 ) return -1;
        if ( nl_gpte.R != 0 || nl_gpte.X != 0 ) {
            *spa = nl_gpte.PPN;
            *spa = *spa * PAGESIZE;
            *spa = *spa & ~(gst_page_sz - 1);
            *spa = *spa | (gpa & (gst_page_sz - 1));
            return (a | (vpn[i] * PTESIZE));
        }
        i = i - 1;
        if ( i < 0 ) return -1;
        gst_page_sz = ( i == 0 ) ? PAGESIZE : (gst_page_sz / 512);
        a = nl_gpte.PPN * PAGESIZE;
    }
    return -1;
}

uint64_t RP_Module::add_vs_stage_pte (
    iommu_top *iommu,
    iosatp_t satp, uint64_t va, spte_t pte, uint8_t add_level,
    iohgatp_t iohgatp, uint8_t SXL) {

    uint16_t vpn[5];
    uint64_t a;
    uint8_t i, PTESIZE, LEVELS;
    spte_t nl_pte;

    PTESIZE = 8;
    if ( satp.MODE == IOSATP_Sv32 && SXL == 1) {
        vpn[0] = get_bits(21, 12, va);
        vpn[1] = get_bits(31, 22, va);
        LEVELS = 2;
        PTESIZE = 4;
    }
    if ( satp.MODE == IOSATP_Sv39 && SXL == 0) {
        vpn[0] = get_bits(20, 12, va);
        vpn[1] = get_bits(29, 21, va);
        vpn[2] = get_bits(38, 30, va);
        LEVELS = 3;
    }
    if ( satp.MODE == IOSATP_Sv48 ) {
        vpn[0] = get_bits(20, 12, va);
        vpn[1] = get_bits(29, 21, va);
        vpn[2] = get_bits(38, 30, va);
        vpn[3] = get_bits(47, 39, va);
        LEVELS = 4;
    }
    if ( satp.MODE == IOSATP_Sv57 ) {
        vpn[0] = get_bits(20, 12, va);
        vpn[1] = get_bits(29, 21, va);
        vpn[2] = get_bits(38, 30, va);
        vpn[3] = get_bits(47, 39, va);
        vpn[4] = get_bits(56, 48, va);
        LEVELS = 5;
    }
    i = LEVELS - 1;
    a = satp.PPN * PAGESIZE;
    while ( i > add_level ) {
        if ( translate_gpa(iommu, iohgatp, a, &a) == -1) return -1;
        nl_pte.raw = 0;
        if ( read_memory_test_rp( (a | (vpn[i] * PTESIZE)), PTESIZE, (char *)&nl_pte.raw))  return -1;
        if ( nl_pte.V == 0 ) {
            gpte_t gpte;

            nl_pte.V = 1;
            nl_pte.PPN = get_free_gppn(1, iohgatp);

            gpte.raw = 0;
            gpte.V = 1;
            gpte.R = 1;
            gpte.W = 1;
            gpte.X = 0;
            gpte.U = 1;
            gpte.G = 0;
            gpte.A = 0;
            gpte.D = 0;
            gpte.PBMT = PMA;
            gpte.PPN = get_free_ppn(1);

            if ( add_g_stage_pte(iommu, iohgatp, (PAGESIZE * nl_pte.PPN), gpte, 0) == -1) return -1;

            if ( write_memory_test_rp( (char *)&nl_pte.raw, (a | (vpn[i] * PTESIZE)), PTESIZE) ) return -1;
        }
        i = i - 1;
        if ( i < 0 ) return 1;
        a = nl_pte.PPN * PAGESIZE;
    }
    if ( translate_gpa(iommu, iohgatp, a, &a) == -1) return -1;
    if ( write_memory_test_rp( (char *)&pte.raw, (a | (vpn[i] * PTESIZE)), PTESIZE) ) return -1;
    return (a | (vpn[i] * PTESIZE));
}

uint64_t RP_Module::add_s_stage_pte (
    iosatp_t satp, uint64_t va, spte_t pte, uint8_t add_level, uint8_t SXL) {

    uint16_t vpn[5];
    uint64_t a;
    uint8_t i, PTESIZE, LEVELS;
    spte_t nl_pte;

    PTESIZE = 8;
    if ( satp.MODE == IOSATP_Sv32 && SXL == 1) {
        vpn[0] = get_bits(21, 12, va);
        vpn[1] = get_bits(31, 22, va);
        LEVELS = 2;
        PTESIZE = 4;
    }
    if ( satp.MODE == IOSATP_Sv39 && SXL == 0) {
        vpn[0] = get_bits(20, 12, va);
        vpn[1] = get_bits(29, 21, va);
        vpn[2] = get_bits(38, 30, va);
        LEVELS = 3;
    }
    if ( satp.MODE == IOSATP_Sv48 ) {
        vpn[0] = get_bits(20, 12, va);
        vpn[1] = get_bits(29, 21, va);
        vpn[2] = get_bits(38, 30, va);
        vpn[3] = get_bits(47, 39, va);
        LEVELS = 4;
    }
    if ( satp.MODE == IOSATP_Sv57 ) {
        vpn[0] = get_bits(20, 12, va);
        vpn[1] = get_bits(29, 21, va);
        vpn[2] = get_bits(38, 30, va);
        vpn[3] = get_bits(47, 39, va);
        vpn[4] = get_bits(56, 48, va);
        LEVELS = 5;
    }
    
    // 调试日志：打印VPN信息
    printf("[ADD_S_STAGE_PTE] IOVA=0x%lx, VPN=[%d,%d,%d,%d,%d], LEVELS=%d, root_PPN=0x%lx\n",
           va, vpn[0], vpn[1], vpn[2], vpn[3], vpn[4], LEVELS, satp.PPN);
    
    i = LEVELS - 1;
    a = satp.PPN * PAGESIZE;
    while ( i > add_level ) {
        nl_pte.raw = 0;
        uint64_t pte_addr_level = a | (vpn[i] * PTESIZE);
        if ( read_memory_test_rp( pte_addr_level, PTESIZE, (char *)&nl_pte.raw)) return -1;
        printf("[ADD_S_STAGE_PTE] Level %d: read PTE at 0x%lx, value=0x%lx, V=%d\n",
               i, pte_addr_level, nl_pte.raw, nl_pte.V);
        if ( nl_pte.V == 0 ) {
            nl_pte.V = 1;
            nl_pte.PPN = get_free_ppn(1);
            nl_pte.R = 0; nl_pte.W = 0; nl_pte.X = 0;  // non-leaf PTE
            uint64_t write_addr = pte_addr_level;
            if ( write_memory_test_rp( (char *)&nl_pte.raw, write_addr, PTESIZE) ) return -1;
            printf("[ADD_S_STAGE_PTE] Level %d: WRITE new non-leaf PTE at 0x%lx, PPN=0x%lx, raw=0x%lx\n",
                   i, write_addr, nl_pte.PPN, nl_pte.raw);
        }
        i = i - 1;
        if ( i < 0 ) return -1;
        a = nl_pte.PPN * PAGESIZE;
    }
    
    // 写入最终Level的PTE（leaf PTE）
    uint64_t leaf_pte_addr = a | (vpn[i] * PTESIZE);
    printf("[ADD_S_STAGE_PTE] WRITE leaf PTE at 0x%lx (level %d), IOVA=0x%lx -> PA=0x%lx (PPN=0x%lx), raw=0x%lx, V=%d,R=%d,W=%d,X=%d\n",
           leaf_pte_addr, i, va, pte.PPN * PAGESIZE, pte.PPN, pte.raw, pte.V, pte.R, pte.W, pte.X);
    
    if ( write_memory_test_rp( (char *)&pte.raw, leaf_pte_addr, PTESIZE) ) return -1;
    
    // 验证写入：读回刚写入的PTE
    spte_t verify_pte;
    verify_pte.raw = 0;
    if ( read_memory_test_rp( leaf_pte_addr, PTESIZE, (char *)&verify_pte.raw) ) {
        printf("[ADD_S_STAGE_PTE] ERROR: Failed to verify PTE at 0x%lx\n", leaf_pte_addr);
        return -1;
    }
    printf("[ADD_S_STAGE_PTE] VERIFY PTE at 0x%lx: raw=0x%lx, PPN=0x%lx, V=%d,R=%d,W=%d,X=%d\n",
           leaf_pte_addr, verify_pte.raw, verify_pte.PPN, verify_pte.V, verify_pte.R, verify_pte.W, verify_pte.X);
    
    if (verify_pte.raw != pte.raw) {
        printf("[ADD_S_STAGE_PTE] WARNING: PTE mismatch! written=0x%lx, read=0x%lx\n", pte.raw, verify_pte.raw);
    }
    
    return leaf_pte_addr;
}

uint64_t RP_Module::add_process_context(
    iommu_top *iommu,
    device_context_t *DC, process_context_t *PC, uint32_t process_id) {
    uint64_t a;
    uint8_t i, LEVELS;
    pdte_t pdte;
    uint16_t PDI[3];

    PDI[0] = get_bits(7,   0, process_id);
    PDI[1] = get_bits(16,  8, process_id);
    PDI[2] = get_bits(19, 17, process_id);

    if ( DC->fsc.pdtp.MODE == PD20 ) LEVELS = 3;
    if ( DC->fsc.pdtp.MODE == PD17 ) LEVELS = 2;
    if ( DC->fsc.pdtp.MODE == PD8  ) LEVELS = 1;

    a = DC->fsc.pdtp.PPN * PAGESIZE;
    i = LEVELS - 1;
    while ( i > 0 ) {
        if ( translate_gpa(iommu, DC->iohgatp, a, &a) == -1 ) return -1;
        pdte.raw = 0;
        if ( read_memory_test_rp( (a + (PDI[i] * 8)), 8, (char *)&pdte.raw) ) return -1;
        if ( pdte.V == 0 ) {
            pdte.V = 1;
            pdte.reserved0 = pdte.reserved1 = 0;
            if (DC->iohgatp.MODE != IOHGATP_Bare) {
                gpte_t gpte;

                pdte.PPN = get_free_gppn(1, DC->iohgatp);

                gpte.raw = 0;
                gpte.V = 1;
                gpte.R = 1;
                gpte.W = 0;
                gpte.X = 0;
                gpte.U = 1;
                gpte.G = 0;
                gpte.A = 0;
                gpte.D = 0;
                gpte.PBMT = PMA;
                gpte.PPN = get_free_ppn(1);

                if ( add_g_stage_pte(iommu, DC->iohgatp, (PAGESIZE * pdte.PPN), gpte, 0) == -1 ) return -1;
            } else {
                pdte.PPN = get_free_ppn(1);
            }
            if ( write_memory_test_rp( (char *)&pdte.raw, (a + (PDI[i] * 8)), 8) ) return -1;
        }
        i = i - 1;
        a = pdte.PPN * PAGESIZE;
    }
    if ( translate_gpa(iommu, DC->iohgatp, a, &a) == -1 ) return -1;
    if ( write_memory_test_rp( (char *)PC, (a + (PDI[0] * 16)), 16) ) return -1;
    return (a + (PDI[0] * 16));
}


int8_t RP_Module::reset_system(
    uint8_t mem_gb, uint16_t num_vms) {
    uint32_t gscid;
    // Create memory
    //if ( (memory = reinterpret_cast<int8_t*>(malloc((mem_gb * 1024UL * 1024UL * 1024UL)))) == NULL )
    //    return -1;

    // Initialize free list of pages
    for ( gscid = 0; gscid < 65536; gscid++ ) next_free_gpage[gscid] = 0;
    next_free_page = 0;
    return 0;
}

uint32_t RP_Module::log2szm1(uint32_t n) {
#if defined(__GNUC__) || defined(__clang__)
    return 31 - __builtin_clz(n);
#else
    uint32_t log2sz = 0;
    while (n >>= 1) {
        log2sz++;
    }
    return log2sz > 0 ? log2sz-1 : 0;
#endif
}

int8_t RP_Module::enable_cq(
    iommu_top *iommu,
    uint32_t nppn) {
    cqb_t cqb;
    cqcsr_t cqcsr;

    cqb.raw = 0;
    cqb.ppn = get_free_ppn(nppn);
    cqb.log2szm1 = log2szm1((nppn * PAGESIZE)/CQ_ENTRY_SZ);
    write_register(&iommu->iommu_inst, CQB_OFFSET, 8, cqb.raw);
    do {
        cqcsr.raw = read_register(&iommu->iommu_inst, CQCSR_OFFSET, 4);
    } while ( cqcsr.busy == 1 );
    cqcsr.raw = 0;
    cqcsr.cie = 1;
    cqcsr.cqen = 1;
    cqcsr.cqmf = 1;
    cqcsr.cmd_to = 1;
    cqcsr.cmd_ill = 1;
    cqcsr.fence_w_ip = 1;
    write_register(&iommu->iommu_inst, CQCSR_OFFSET, 4, cqcsr.raw);
    do {
        cqcsr.raw = read_register(&iommu->iommu_inst, CQCSR_OFFSET, 4);
    } while ( cqcsr.busy == 1 );
    if ( cqcsr.cqon != 1 ) {
        printf("CQ enable failed\n");
        return -1;
    }
    return 0;
}

int8_t RP_Module::enable_fq(
    iommu_top *iommu,
    uint32_t nppn) {
    fqb_t fqb;
    fqcsr_t fqcsr;

    fqb.raw = 0;
    fqb.ppn = get_free_ppn(nppn);
    fqb.log2szm1 = log2szm1((nppn * PAGESIZE)/FQ_ENTRY_SZ);
    write_register(&iommu->iommu_inst, FQB_OFFSET, 8, fqb.raw);
    do {
        fqcsr.raw = read_register(&iommu->iommu_inst, FQCSR_OFFSET, 4);
    } while ( fqcsr.busy == 1 );
    fqcsr.raw = 0;
    fqcsr.fie = 1;
    fqcsr.fqen = 1;
    fqcsr.fqmf = 1;
    fqcsr.fqof = 1;
    write_register(&iommu->iommu_inst, FQCSR_OFFSET, 4, fqcsr.raw);
    do {
        fqcsr.raw = read_register(&iommu->iommu_inst, FQCSR_OFFSET, 4);
    } while ( fqcsr.busy == 1 );
    if ( fqcsr.fqon != 1 ) {
        printf("FQ enable failed\n");
        return -1;
    }
    return 0;
}

int8_t RP_Module::enable_disable_pq(
    iommu_top *iommu,
    uint32_t nppn, uint8_t enable_disable) {
    pqb_t pqb;
    pqcsr_t pqcsr;

    if ( enable_disable == 1 ) {
        pqb.raw = 0;
        pqb.ppn = get_free_ppn(nppn);
        pqb.log2szm1 = log2szm1((nppn * PAGESIZE)/PQ_ENTRY_SZ);
        write_register(&iommu->iommu_inst, PQB_OFFSET, 8, pqb.raw);
    }
    do {
        pqcsr.raw = read_register(&iommu->iommu_inst, PQCSR_OFFSET, 4);
    } while ( pqcsr.busy == 1 );
    pqcsr.raw = 0;
    pqcsr.pie = 1;
    pqcsr.pqen = enable_disable;
    pqcsr.pqmf = 1;
    pqcsr.pqof = 1;
    write_register(&iommu->iommu_inst, PQCSR_OFFSET, 4, pqcsr.raw);
    do {
        pqcsr.raw = read_register(&iommu->iommu_inst, PQCSR_OFFSET, 4);
    } while ( pqcsr.busy == 1 );
    if ( pqcsr.pqon != 1 && enable_disable == 1) {
        printf("PQ enable failed\n");
        return -1;
    }
    if ( pqcsr.pqon == 1 && enable_disable == 0 ) {
        printf("PQ disable failed\n");
        return -1;
    }
    return 0;
}

int8_t RP_Module::enable_iommu(
    iommu_top *iommu,
    uint8_t iommu_mode) {
    ddtp_t ddtp;
    uint32_t i;
    uint64_t zero = 0;

    // Allocate a page for DDT root page
    do {
        ddtp.raw = read_register(&iommu->iommu_inst, DDTP_OFFSET, 8);
    } while ( ddtp.busy == 1 );

    ddtp.raw = 0;
    ddtp.ppn = get_free_ppn(1);
    // Clear the page
    for ( i = 0; i < 512; i++ )
        write_memory_test_rp( (char *)&zero, (ddtp.ppn * PAGESIZE) | (i * 8), 8);

    ddtp.iommu_mode = iommu_mode;
    write_register(&iommu->iommu_inst, DDTP_OFFSET, 8, ddtp.raw);
    do {
        ddtp.raw = read_register(&iommu->iommu_inst, DDTP_OFFSET, 8);
    } while ( ddtp.busy == 1 );
    return (ddtp.iommu_mode == iommu_mode) ? 0 : -1;
}

int8_t RP_Module::check_exp_pq_rec(iommu_top *iommu, uint32_t DID, uint32_t PID, uint8_t PV, 
                 uint8_t PRIV, uint8_t EXEC,uint16_t reserved0, uint8_t reserved1, uint64_t PLOAD)
{
    page_rec_t page_rec;
    pqb_t pqb;
    pqh_t pqh;
    if ( read_register(&iommu->iommu_inst, PQH_OFFSET, 4) == read_register(&iommu->iommu_inst, PQT_OFFSET, 4) ) return -1;
    pqh.raw = read_register(&iommu->iommu_inst, PQH_OFFSET, 4);
    pqb.raw = read_register(&iommu->iommu_inst, PQB_OFFSET, 8);
    read_memory_test_rp(((pqb.ppn * PAGESIZE) | (pqh.index * PQ_ENTRY_SZ)), PQ_ENTRY_SZ, (char *)&page_rec);
    if ( page_rec.DID != DID ) return -1;
    if ( page_rec.PID != PID ) return -1;
    if ( page_rec.PV != PV ) return -1;
    if ( page_rec.PRIV != PRIV ) return -1;
    if ( page_rec.EXEC != EXEC ) return -1;
    if ( page_rec.reserved0 != reserved0 ) return -1;
    if ( page_rec.reserved1 != reserved1 ) return -1;
    if ( page_rec.PAYLOAD != PLOAD ) return -1;
    write_register(&iommu->iommu_inst, PQH_OFFSET, 4, pqh.raw + 1);
    return 0;
}
void RP_Module::iotinval(
    iommu_top *iommu,
    uint8_t f3, uint8_t GV, uint8_t AV, uint8_t PSCV, uint32_t GSCID, uint32_t PSCID, uint64_t address) {
    command_t cmd;
    cqb_t cqb;
    cqt_t cqt;
    uint64_t temp, temp1;
    temp = access_viol_addr;
    temp1 = data_corruption_addr;
    access_viol_addr = -1;
    data_corruption_addr = -1;
    cmd.low = cmd.high = 0;
    cmd.iotinval.opcode = IOTINVAL;
    cmd.iotinval.func3 = f3;
    cmd.iotinval.gv = GV;
    cmd.iotinval.av = AV;
    cmd.iotinval.pscv = PSCV;
    cmd.iotinval.gscid = GSCID;
    cmd.iotinval.pscid = PSCID;
    cmd.iotinval.addr_63_12 = address / PAGESIZE;
    cqb.raw = read_register(&iommu->iommu_inst, CQB_OFFSET, 8);
    cqt.raw = read_register(&iommu->iommu_inst, CQT_OFFSET, 4);
    write_memory_test_rp( (char *)&cmd, ((cqb.ppn * PAGESIZE) | (cqt.index * CQ_ENTRY_SZ)), CQ_ENTRY_SZ);
    access_viol_addr = temp;
    data_corruption_addr = temp1;
    cqt.index++;
    write_register(&iommu->iommu_inst, CQT_OFFSET, 4, cqt.raw);
    process_commands(&iommu->iommu_inst);
    return;
}
void RP_Module::ats_command(
    iommu_top *iommu,
    uint8_t f3, uint8_t DSV, uint8_t PV, uint32_t PID, uint8_t DSEG, uint16_t RID, uint64_t payload) {
    command_t cmd;
    cqb_t cqb;
    cqt_t cqt;
    uint64_t temp, temp1;
    temp = access_viol_addr;
    temp1 = data_corruption_addr;
    access_viol_addr = -1;
    data_corruption_addr = -1;
    cmd.low = cmd.high = 0;
    cmd.ats.opcode = ATS;
    cmd.ats.func3 = f3;
    cmd.ats.rid = RID;
    cmd.ats.pv = PV;
    cmd.ats.pid = PID;
    cmd.ats.dsv = DSV;
    cmd.ats.dseg = DSEG;
    cmd.ats.payload = payload;

    cqb.raw = read_register(&iommu->iommu_inst, CQB_OFFSET, 8);
    cqt.raw = read_register(&iommu->iommu_inst, CQT_OFFSET, 4);
    write_memory_test_rp( (char *)&cmd, ((cqb.ppn * PAGESIZE) | (cqt.index * CQ_ENTRY_SZ)), CQ_ENTRY_SZ);
    access_viol_addr = temp;
    data_corruption_addr = temp1;
    cqt.index++;
    write_register(&iommu->iommu_inst, CQT_OFFSET, 4, cqt.raw);
    process_commands(&iommu->iommu_inst);
    return;
}
void RP_Module::generic_any(
    iommu_top *iommu,
    command_t cmd) {
    cqb_t cqb;
    cqt_t cqt;
    uint64_t temp, temp1;
    temp = access_viol_addr;
    temp1 = data_corruption_addr;
    cqb.raw = read_register(&iommu->iommu_inst, CQB_OFFSET, 8);
    cqt.raw = read_register(&iommu->iommu_inst, CQT_OFFSET, 4);
    write_memory_test_rp( (char *)&cmd, ((cqb.ppn * PAGESIZE) | (cqt.index * CQ_ENTRY_SZ)), CQ_ENTRY_SZ);
    access_viol_addr = temp;
    data_corruption_addr = temp1;
    cqt.index++;
    write_register(&iommu->iommu_inst, CQT_OFFSET, 4, cqt.raw);
    process_commands(&iommu->iommu_inst);
    return;
}

void RP_Module::iodir(
    iommu_top *iommu,
    uint8_t f3, uint8_t DV, uint32_t DID, uint32_t PID) {
    command_t cmd;
    cqb_t cqb;
    cqt_t cqt;
    uint64_t temp, temp1;
    temp = access_viol_addr;
    temp1 = data_corruption_addr;
    access_viol_addr = -1;
    data_corruption_addr = -1;
    cmd.low = cmd.high = 0;
    cmd.iodir.opcode = IODIR;
    cmd.iodir.func3 = f3;
    cmd.iodir.dv = DV;
    cmd.iodir.did = DID;
    cmd.iodir.pid = PID;
    cqb.raw = read_register(&iommu->iommu_inst, CQB_OFFSET, 8);
    cqt.raw = read_register(&iommu->iommu_inst, CQT_OFFSET, 4);
    write_memory_test_rp( (char *)&cmd, ((cqb.ppn * PAGESIZE) | (cqt.index * CQ_ENTRY_SZ)), CQ_ENTRY_SZ);
    access_viol_addr = temp;
    data_corruption_addr = temp1;
    cqt.index++;
    write_register(&iommu->iommu_inst, CQT_OFFSET, 4, cqt.raw);
    process_commands(&iommu->iommu_inst);
    return;
}
void RP_Module::iofence(
    iommu_top *iommu,
    uint8_t f3, uint8_t PR, uint8_t PW, uint8_t AV, uint8_t WSI_bit, uint64_t addr, uint32_t data) {
    command_t cmd;
    cqb_t cqb;
    cqt_t cqt;
    uint64_t temp, temp1;
    temp = access_viol_addr;
    temp1 = data_corruption_addr;
    access_viol_addr = -1;
    data_corruption_addr = -1;
    cmd.low = cmd.high = 0;
    cmd.iofence.opcode = IOFENCE;
    cmd.iofence.func3 = f3;
    cmd.iofence.pr = PR;
    cmd.iofence.pw = PW;
    cmd.iofence.av = AV;
    cmd.iofence.wsi = WSI_bit;
    cmd.iofence.addr_63_2 = addr >> 2;
    cmd.iofence.data = data;
    cqb.raw = read_register(&iommu->iommu_inst, CQB_OFFSET, 8);
    cqt.raw = read_register(&iommu->iommu_inst, CQT_OFFSET, 4);
    write_memory_test_rp( (char *)&cmd, ((cqb.ppn * PAGESIZE) | (cqt.index * CQ_ENTRY_SZ)), CQ_ENTRY_SZ);
    access_viol_addr = temp;
    data_corruption_addr = temp1;
    cqt.index++;
    write_register(&iommu->iommu_inst, CQT_OFFSET, 4, cqt.raw);
    process_commands(&iommu->iommu_inst);
    return;
}