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
            gpte.A = 1;
            gpte.D = 1;
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
            gpte.A = 1;
            gpte.D = 1;
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
            gpte.A = 1;
            gpte.D = 1;
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
    uint8_t f3, uint8_t GV, uint8_t AV, uint8_t PSCV, uint32_t GSCID, uint32_t PSCID, uint64_t address, uint8_t NL) {
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
    cmd.iotinval.nl = NL;   // [失效] 非叶PTE失效扩展位(需 capabilities.NL=1)
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

// ============================================================
// [CPU侧] RISC-V hart 侧 Cache/TLB 失效指令行为建模
//
// 关键规范语义(本组方法的存在意义):
//   SFENCE.VMA / SINVAL.VMA 只失效执行该指令的 hart 的地址转换缓存,
//   **不失效 IOMMU 的 IOATC**。因此 OS 修改被 DMA 使用的页表后, 除了
//   CPU 侧维护序列, 还必须经 IOMMU 命令队列下发 IOTINVAL.VMA/GVMA。
//   测试用例通过"只做 CPU 侧序列 -> IOMMU 仍命中"来验证这一隔离性。
//
// 建模口径: 本平台无 CPU core, 且页表由 write_memory_test_rp 直写 DDR,
//   故这些指令的数据搬移语义为空操作; 保留 计数 + 仿真时间推进 + 日志,
//   以在时间轴与统计上体现 OS 维护开销。
// ============================================================

void RP_Module::cpu_fence_i() {
    cpu_stats.fence_i++;
    printf("[t=%llu ns][CPU_INSTR] FENCE.I : flush local I-cache + refetch pipeline "
           "(no effect on IOMMU IOATC)\n",
           (unsigned long long)sc_core::sc_time_stamp().value() / 1000);
    fflush(stdout);
    wait(cpu_instr_latency_ns, SC_NS);
}

void RP_Module::cpu_sfence_vma(uint64_t addr, uint32_t asid) {
    cpu_stats.sfence_vma++;
    if (addr == 0 && asid == 0) {
        printf("[t=%llu ns][CPU_INSTR] SFENCE.VMA x0,x0 : invalidate ALL local TLB entries "
               "(IOMMU IOATC NOT affected)\n",
               (unsigned long long)sc_core::sc_time_stamp().value() / 1000);
    } else if (addr != 0 && asid == 0) {
        printf("[t=%llu ns][CPU_INSTR] SFENCE.VMA addr=0x%lx : invalidate local TLB for VA "
               "(IOMMU IOATC NOT affected)\n",
               (unsigned long long)sc_core::sc_time_stamp().value() / 1000,
               (unsigned long)addr);
    } else if (addr == 0 && asid != 0) {
        printf("[t=%llu ns][CPU_INSTR] SFENCE.VMA asid=%u : invalidate local TLB for ASID "
               "(IOMMU IOATC NOT affected)\n",
               (unsigned long long)sc_core::sc_time_stamp().value() / 1000, asid);
    } else {
        printf("[t=%llu ns][CPU_INSTR] SFENCE.VMA addr=0x%lx asid=%u : invalidate local TLB "
               "(IOMMU IOATC NOT affected)\n",
               (unsigned long long)sc_core::sc_time_stamp().value() / 1000,
               (unsigned long)addr, asid);
    }
    fflush(stdout);
    wait(cpu_instr_latency_ns, SC_NS);
}

void RP_Module::cpu_sfence_w_inval() {
    cpu_stats.sfence_w_inval++;
    printf("[t=%llu ns][CPU_INSTR] SFENCE.W.INVAL : order prior writes before SINVAL sequence\n",
           (unsigned long long)sc_core::sc_time_stamp().value() / 1000);
    fflush(stdout);
    wait(cpu_instr_latency_ns, SC_NS);
}

void RP_Module::cpu_sinval_vma(uint64_t addr, uint32_t asid) {
    cpu_stats.sinval_vma++;
    printf("[t=%llu ns][CPU_INSTR] SINVAL.VMA addr=0x%lx asid=%u : invalidate local TLB "
           "(NO ordering guarantee; must be fenced; IOMMU IOATC NOT affected)\n",
           (unsigned long long)sc_core::sc_time_stamp().value() / 1000,
           (unsigned long)addr, asid);
    fflush(stdout);
    wait(cpu_instr_latency_ns, SC_NS);
}

void RP_Module::cpu_sfence_inval_ir() {
    cpu_stats.sfence_inval_ir++;
    printf("[t=%llu ns][CPU_INSTR] SFENCE.INVAL.IR : order SINVAL sequence before "
           "subsequent implicit reads\n",
           (unsigned long long)sc_core::sc_time_stamp().value() / 1000);
    fflush(stdout);
    wait(cpu_instr_latency_ns, SC_NS);
}

void RP_Module::cpu_cbo_clean(uint64_t addr) {
    const uint64_t blk = addr & ~(CPU_CACHE_BLOCK_SIZE - 1);
    cpu_stats.cbo_clean++;
    cpu_stats.blocks_processed++;
    printf("[t=%llu ns][CPU_INSTR] CBO.CLEAN blk=0x%lx (size=%lu) : writeback dirty data, "
           "block stays valid\n",
           (unsigned long long)sc_core::sc_time_stamp().value() / 1000,
           (unsigned long)blk, (unsigned long)CPU_CACHE_BLOCK_SIZE);
    fflush(stdout);
    wait(cpu_instr_latency_ns, SC_NS);
}

void RP_Module::cpu_cbo_flush(uint64_t addr) {
    const uint64_t blk = addr & ~(CPU_CACHE_BLOCK_SIZE - 1);
    cpu_stats.cbo_flush++;
    cpu_stats.blocks_processed++;
    printf("[t=%llu ns][CPU_INSTR] CBO.FLUSH blk=0x%lx (size=%lu) : atomic Clean+Inval "
           "(data now visible to IOMMU)\n",
           (unsigned long long)sc_core::sc_time_stamp().value() / 1000,
           (unsigned long)blk, (unsigned long)CPU_CACHE_BLOCK_SIZE);
    fflush(stdout);
    wait(cpu_instr_latency_ns, SC_NS);
}

void RP_Module::cpu_cbo_inval(uint64_t addr) {
    const uint64_t blk = addr & ~(CPU_CACHE_BLOCK_SIZE - 1);
    cpu_stats.cbo_inval++;
    cpu_stats.blocks_processed++;
    printf("[t=%llu ns][CPU_INSTR] CBO.INVAL blk=0x%lx (size=%lu) : drop all copies "
           "in coherence domain (no writeback)\n",
           (unsigned long long)sc_core::sc_time_stamp().value() / 1000,
           (unsigned long)blk, (unsigned long)CPU_CACHE_BLOCK_SIZE);
    fflush(stdout);
    wait(cpu_instr_latency_ns, SC_NS);
}

void RP_Module::cpu_cbo_flush_range(uint64_t addr, uint64_t len) {
    if (len == 0) return;
    const uint64_t first = addr & ~(CPU_CACHE_BLOCK_SIZE - 1);
    const uint64_t last  = (addr + len - 1) & ~(CPU_CACHE_BLOCK_SIZE - 1);
    const uint64_t nblk  = (last - first) / CPU_CACHE_BLOCK_SIZE + 1;
    printf("[t=%llu ns][CPU_INSTR] CBO.FLUSH range [0x%lx, 0x%lx) -> %lu blocks\n",
           (unsigned long long)sc_core::sc_time_stamp().value() / 1000,
           (unsigned long)addr, (unsigned long)(addr + len), (unsigned long)nblk);
    fflush(stdout);
    for (uint64_t b = first; b <= last; b += CPU_CACHE_BLOCK_SIZE) {
        cpu_stats.cbo_flush++;
        cpu_stats.blocks_processed++;
        wait(cpu_instr_latency_ns, SC_NS);
    }
}

void RP_Module::cpu_pagetable_update_sequence(uint64_t pt_addr, uint64_t pt_len,
                                              uint32_t asid, bool with_fence_i) {
    printf("\n[t=%llu ns][CPU_INSTR] ===== OS page-table update sequence (CPU side) =====\n",
           (unsigned long long)sc_core::sc_time_stamp().value() / 1000);
    fflush(stdout);
    // 1) 把页表所在 Cache 块回写并失效, 确保 IOMMU 的隐式读能看到新页表
    cpu_cbo_flush_range(pt_addr, pt_len);
    // 2) 失效本 hart TLB(按 ASID; asid=0 表示全部)
    cpu_sfence_vma(0, asid);
    // 3) 若页表变更影响可执行页, 还需 FENCE.I
    if (with_fence_i) cpu_fence_i();
    printf("[t=%llu ns][CPU_INSTR] ===== CPU side done. NOTE: IOMMU IOATC still holds "
           "stale entries until IOTINVAL is issued =====\n",
           (unsigned long long)sc_core::sc_time_stamp().value() / 1000);
    fflush(stdout);
}

void RP_Module::print_cpu_instr_stats() {
    printf("\n========== CPU-side Instruction Stats (modeled) ==========\n");
    printf("  Cache block size:   %lu B (software-discovered, CBO.* granularity)\n",
           (unsigned long)CPU_CACHE_BLOCK_SIZE);
    printf("  Instr latency:      %.1f ns each (modeled)\n", cpu_instr_latency_ns);
    printf("  ---\n");
    printf("  FENCE.I:            %lu\n", (unsigned long)cpu_stats.fence_i);
    printf("  SFENCE.VMA:         %lu\n", (unsigned long)cpu_stats.sfence_vma);
    printf("  SFENCE.W.INVAL:     %lu\n", (unsigned long)cpu_stats.sfence_w_inval);
    printf("  SINVAL.VMA:         %lu\n", (unsigned long)cpu_stats.sinval_vma);
    printf("  SFENCE.INVAL.IR:    %lu\n", (unsigned long)cpu_stats.sfence_inval_ir);
    printf("  CBO.CLEAN:          %lu\n", (unsigned long)cpu_stats.cbo_clean);
    printf("  CBO.FLUSH:          %lu\n", (unsigned long)cpu_stats.cbo_flush);
    printf("  CBO.INVAL:          %lu\n", (unsigned long)cpu_stats.cbo_inval);
    printf("  Blocks processed:   %lu\n", (unsigned long)cpu_stats.blocks_processed);
    printf("==========================================================\n");
    fflush(stdout);
}

// ============================================================
// [虚拟化] 两级Stage 场景 Guest OS / vIOMMU / VMM 的 Cache Invalidate 建模
// 对应规范 6.5.3(Lazy) / 6.5.4(Strict) 场景二
// ============================================================

// ---- Stage2 unmap: 清除叶级 gpte(V=0); 非叶级只读, 缺失即失败(不分配) ----
uint64_t RP_Module::unmap_g_stage_pte(iommu_top* iommu, iohgatp_t iohgatp,
                                      uint64_t gpa) {
    uint16_t vpn[5];
    uint64_t a;
    uint8_t i, PTESIZE = 8, LEVELS = 0;
    gpte_t nl_gpte;

    if (iohgatp.MODE == IOHGATP_Sv32x4 && iommu->iommu_inst.reg_file.fctl.gxl == 1) {
        vpn[0] = get_bits(21, 12, gpa); vpn[1] = get_bits(34, 22, gpa);
        LEVELS = 2; PTESIZE = 4;
    } else if (iohgatp.MODE == IOHGATP_Sv39x4 && iommu->iommu_inst.reg_file.fctl.gxl == 0) {
        vpn[0] = get_bits(20, 12, gpa); vpn[1] = get_bits(29, 21, gpa);
        vpn[2] = get_bits(40, 30, gpa); LEVELS = 3;
    } else if (iohgatp.MODE == IOHGATP_Sv48x4) {
        vpn[0] = get_bits(20, 12, gpa); vpn[1] = get_bits(29, 21, gpa);
        vpn[2] = get_bits(38, 30, gpa); vpn[3] = get_bits(49, 39, gpa); LEVELS = 4;
    } else if (iohgatp.MODE == IOHGATP_Sv57x4) {
        vpn[0] = get_bits(20, 12, gpa); vpn[1] = get_bits(29, 21, gpa);
        vpn[2] = get_bits(38, 30, gpa); vpn[3] = get_bits(47, 39, gpa);
        vpn[4] = get_bits(58, 48, gpa); LEVELS = 5;
    } else {
        return (uint64_t)-1;
    }

    i = LEVELS - 1;
    a = iohgatp.PPN * PAGESIZE;
    while (i > 0) {
        nl_gpte.raw = 0;
        if (read_memory_test_rp((a | (vpn[i] * PTESIZE)), PTESIZE, (char*)&nl_gpte.raw))
            return (uint64_t)-1;
        if (nl_gpte.V == 0) return (uint64_t)-1;   // 非叶缺失: unmap 不分配页表
        i = i - 1;
        a = nl_gpte.PPN * PAGESIZE;
    }
    gpte_t zero_gpte;
    zero_gpte.raw = 0;                             // V=0 -> unmap
    if (write_memory_test_rp((char*)&zero_gpte.raw, (a | (vpn[i] * PTESIZE)), PTESIZE))
        return (uint64_t)-1;
    return (a | (vpn[i] * PTESIZE));
}

// ---- Stage1 unmap: 清除叶级 spte(V=0); VS页表页位于GPA空间, 需 translate_gpa ----
uint64_t RP_Module::unmap_vs_stage_pte(iommu_top* iommu, iosatp_t satp, uint64_t va,
                                       iohgatp_t iohgatp, uint8_t SXL) {
    uint16_t vpn[5];
    uint64_t a;
    uint8_t i, PTESIZE = 8, LEVELS = 0;
    spte_t nl_pte;

    if (satp.MODE == IOSATP_Sv32 && SXL == 1) {
        vpn[0] = get_bits(21, 12, va); vpn[1] = get_bits(31, 22, va);
        LEVELS = 2; PTESIZE = 4;
    } else if (satp.MODE == IOSATP_Sv39 && SXL == 0) {
        vpn[0] = get_bits(20, 12, va); vpn[1] = get_bits(29, 21, va);
        vpn[2] = get_bits(38, 30, va); LEVELS = 3;
    } else if (satp.MODE == IOSATP_Sv48) {
        vpn[0] = get_bits(20, 12, va); vpn[1] = get_bits(29, 21, va);
        vpn[2] = get_bits(38, 30, va); vpn[3] = get_bits(47, 39, va); LEVELS = 4;
    } else if (satp.MODE == IOSATP_Sv57) {
        vpn[0] = get_bits(20, 12, va); vpn[1] = get_bits(29, 21, va);
        vpn[2] = get_bits(38, 30, va); vpn[3] = get_bits(47, 39, va);
        vpn[4] = get_bits(56, 48, va); LEVELS = 5;
    } else {
        return (uint64_t)-1;
    }

    i = LEVELS - 1;
    a = satp.PPN * PAGESIZE;
    while (i > 0) {
        if (translate_gpa(iommu, iohgatp, a, &a) == -1) return (uint64_t)-1;
        nl_pte.raw = 0;
        if (read_memory_test_rp((a | (vpn[i] * PTESIZE)), PTESIZE, (char*)&nl_pte.raw))
            return (uint64_t)-1;
        if (nl_pte.V == 0) return (uint64_t)-1;    // 非叶缺失: unmap 不分配页表
        i = i - 1;
        a = nl_pte.PPN * PAGESIZE;
    }
    if (translate_gpa(iommu, iohgatp, a, &a) == -1) return (uint64_t)-1;
    spte_t zero_pte;
    zero_pte.raw = 0;                              // V=0 -> unmap
    if (write_memory_test_rp((char*)&zero_pte.raw, (a | (vpn[i] * PTESIZE)), PTESIZE))
        return (uint64_t)-1;
    return (a | (vpn[i] * PTESIZE));
}

// ---- 步骤2-3: Guest 把命令写入 vIOMMU 的虚拟CQ内存区 ----
void RP_Module::guest_write_vcq(const VirtCQEntry& e) {
    vcq_ring.push_back(e);
    vcq_tail++;
    virt_stats.vcq_writes++;
}

// ---- 步骤4: Guest 写虚拟CQ tail 寄存器(doorbell) ----
void RP_Module::guest_ring_vcq_doorbell() {
    virt_stats.vcq_doorbells++;
    printf("[t=%llu ns][GUEST] ring vCQ doorbell (tail=%u, %zu cmds pending)\n",
           (unsigned long long)sc_core::sc_time_stamp().value() / 1000,
           vcq_tail, vcq_ring.size());
    fflush(stdout);
}

// ---- 步骤5: VMM 拦截 Guest 对虚拟CQ tail 的写操作 ----
void RP_Module::vmm_intercept() {
    virt_stats.vmm_intercepts++;
    const bool trap = (vmm_intercept_mode == VmmInterceptMode::TRAP_AND_EMULATE);
    printf("[t=%llu ns][VMM] intercept via %s (modeled %.0f ns)\n",
           (unsigned long long)sc_core::sc_time_stamp().value() / 1000,
           trap ? "trap-and-emulate (VM exit)" : "para-virt hypercall",
           vmm_trap_latency_ns);
    fflush(stdout);
    virt_stats.vmm_trap_ns += vmm_trap_latency_ns;
    wait(vmm_trap_latency_ns, SC_NS);
}

// ---- 步骤6-9: VMM 解析Guest命令 -> 转换为物理IOMMU命令 -> 写物理CQ ----
void RP_Module::vmm_translate_and_forward(iommu_top* iommu) {
    virt_stats.vmm_xlat_ns += vmm_translate_latency_ns;
    wait(vmm_translate_latency_ns, SC_NS);

    const double t0 = sc_core::sc_time_stamp().to_seconds() * 1e9;
    for (const auto& e : vcq_ring) {
        if (e.opcode == 0) {
            // Guest 的 IOTINVAL.VMA -> 物理命令: GV=1 + GSCID标识该VM + PSCID + ADDR
            printf("[t=%llu ns][VMM] translate IOTINVAL.VMA -> phys CQ "
                   "(GV=1, GSCID=%u, PSCID=%u, AV=%u, ADDR=0x%lx)\n",
                   (unsigned long long)sc_core::sc_time_stamp().value() / 1000,
                   e.gscid, e.pscid, e.av, (unsigned long)e.addr);
            fflush(stdout);
            iotinval(iommu, VMA, e.gv, e.av, e.pscv, e.gscid, e.pscid, e.addr, e.nl);
            virt_stats.guest_inval_cmds++;
        } else {
            // IOFENCE.C AV=1: 物理IOMMU经AXI写完成标志到内存, 供Guest轮询
            iofence(iommu, IOFENCE_C, 0, 0, 1 /*AV*/, 0, iofence_flag_addr, 0x1);
        }
    }
    virt_stats.iofence_wait_total_ns += sc_core::sc_time_stamp().to_seconds() * 1e9 - t0;
    vcq_ring.clear();
}

// ---- 步骤10: VMM 更新虚拟CQ head 反馈 Guest ----
void RP_Module::vmm_update_vcq_head() {
    vcq_head = vcq_tail;
}

// ---- 步骤11: Guest 轮询确认 invalidation 完成 ----
void RP_Module::guest_poll_completion() {
    virt_stats.guest_poll_ns += guest_poll_latency_ns;
    wait(guest_poll_latency_ns, SC_NS);
    printf("[t=%llu ns][GUEST] poll completion OK (vCQ head=%u) -> safe to reclaim GPA\n",
           (unsigned long long)sc_core::sc_time_stamp().value() / 1000, vcq_head);
    fflush(stdout);
}

// ---- 步骤2~11 完整封装: Guest 经 vIOMMU/VMM 发起一条 IOTINVAL.VMA ----
void RP_Module::guest_issue_iotinval_vma(iommu_top* iommu, uint32_t gscid,
                                        uint32_t pscid, uint8_t av,
                                        uint64_t gva, uint8_t nl) {
    VirtCQEntry inv{};
    inv.opcode = 0;
    inv.gv = 1;                 // 虚拟化场景: GV=1 标识该VM
    inv.av = av;                // Lazy: 0(批量, 只GSCID/PSCID) / Strict: 1(带ADDR)
    inv.pscv = 1;               // Stage1 由 Guest 管理 -> 指定 PSCID
    inv.nl = nl;
    inv.gscid = gscid;
    inv.pscid = pscid;
    inv.addr = (av ? gva : 0);
    guest_write_vcq(inv);                       // 步骤2-3

    VirtCQEntry fence{};
    fence.opcode = 1;                           // IOFENCE.C (AV=1)
    guest_write_vcq(fence);                     // 步骤3

    guest_ring_vcq_doorbell();                  // 步骤4
    vmm_intercept();                            // 步骤5
    vmm_translate_and_forward(iommu);           // 步骤6-9
    vmm_update_vcq_head();                      // 步骤10
    guest_poll_completion();                    // 步骤11
}

// ---- VMM 发起 Stage2 失效: 直接写物理CQ, 无需拦截 ----
void RP_Module::vmm_issue_iotinval_gvma(iommu_top* iommu, uint32_t gscid) {
    printf("[t=%llu ns][VMM] issue IOTINVAL.GVMA (GV=1, GSCID=%u) for Stage2 unmap\n",
           (unsigned long long)sc_core::sc_time_stamp().value() / 1000, gscid);
    fflush(stdout);
    iotinval(iommu, GVMA, 1 /*GV*/, 0 /*AV*/, 0 /*PSCV*/, gscid, 0, 0, 0);
    iofence(iommu, IOFENCE_C, 0, 0, 1 /*AV*/, 0, iofence_flag_addr, 0x1);
    virt_stats.vmm_inval_cmds++;
}

// ---- Guest Flush Queue: 是否该 drain ----
bool RP_Module::guest_fq_should_drain() const {
    if (guest_fq.empty()) return false;
    if (guest_fq.size() >= guest_fq_depth) return true;
    const double now = sc_core::sc_time_stamp().to_seconds() * 1e9;
    return (now - guest_fq_last_drain_ns) >= guest_fq_timeout_ns;
}

// ---- Guest Flush Queue Drain: 一条批量 IOTINVAL.VMA(AV=0) 覆盖全部累积GVA ----
void RP_Module::guest_fq_drain(iommu_top* iommu, uint32_t gscid, uint32_t pscid) {
    if (guest_fq.empty()) return;
    const size_t n = guest_fq.size();
    const bool by_depth = (n >= guest_fq_depth);
    if (by_depth) virt_stats.fq_drain_by_depth++;
    else          virt_stats.fq_drain_by_timeout++;

    printf("[t=%llu ns][GUEST] Flush Queue DRAIN: %zu GVAs batched into ONE "
           "IOTINVAL.VMA(AV=0, GSCID=%u, PSCID=%u) [trigger=%s]\n",
           (unsigned long long)sc_core::sc_time_stamp().value() / 1000,
           n, gscid, pscid, by_depth ? "depth" : "timeout");
    fflush(stdout);

    // Lazy 关键: 无论累积多少 GVA, 只发一条不带 ADDR 的失效命令
    // -> 物理IOMMU侧走 LAZY(记录LIB + 全局VN++), PT/Walker 延迟失效
    guest_issue_iotinval_vma(iommu, gscid, pscid, 0 /*AV=0*/, 0);

    virt_stats.fq_drains++;
    virt_stats.fq_batched_gvas += n;
    guest_fq.clear();
    guest_fq_last_drain_ns = sc_core::sc_time_stamp().to_seconds() * 1e9;
}

// ---- 虚拟化失效统计报告 ----
void RP_Module::print_virt_inval_stats(const char* mode_name) {
    const auto& s = virt_stats;
    printf("\n========== Virtualized Cache Invalidation Report [%s] ==========\n",
           mode_name);
    printf("  [模型假设] 以下延时为建模值, 非实测(可经 Makefile 宏调整):\n");
    printf("    VMM intercept(%s): %.0f ns/次\n",
           vmm_intercept_mode == VmmInterceptMode::TRAP_AND_EMULATE
               ? "trap-and-emulate" : "para-virt hypercall",
           vmm_trap_latency_ns);
    printf("    VMM translate: %.0f ns   Guest poll: %.0f ns   fence: %.0f ns\n",
           vmm_translate_latency_ns, guest_poll_latency_ns, guest_fence_latency_ns);
    printf("  ---- unmap 与失效命令 ----\n");
    printf("    Guest unmaps (Stage1 GVA->GPA):     %lu\n", (unsigned long)s.guest_unmaps);
    printf("    VMM   unmaps (Stage2 GPA->SPA):     %lu\n", (unsigned long)s.vmm_unmaps);
    printf("    Guest IOTINVAL.VMA  cmds issued:    %lu\n", (unsigned long)s.guest_inval_cmds);
    printf("    VMM   IOTINVAL.GVMA cmds issued:    %lu\n", (unsigned long)s.vmm_inval_cmds);
    if (s.guest_unmaps > 0) {
        printf("    每次 unmap 平均失效命令数:          %.4f  (Strict=1.0, Lazy=1/FQ深度)\n",
               (double)s.guest_inval_cmds / s.guest_unmaps);
    }
    printf("  ---- Flush Queue (Lazy 批量合并) ----\n");
    printf("    Drains: %lu  (depth触发=%lu, timeout触发=%lu, 收尾强制=%lu)\n",
           (unsigned long)s.fq_drains, (unsigned long)s.fq_drain_by_depth,
           (unsigned long)s.fq_drain_by_timeout, (unsigned long)s.fq_drain_forced);
    printf("    合并 GVA 总数: %lu", (unsigned long)s.fq_batched_gvas);
    if (s.fq_drains > 0) {
        printf("   平均合并率: %.1f GVA/drain\n",
               (double)s.fq_batched_gvas / s.fq_drains);
    } else {
        printf("   (Strict 模式无 Flush Queue)\n");
    }
    printf("  ---- 虚拟化开销分解 ----\n");
    const double virt_total = s.vmm_trap_ns + s.vmm_xlat_ns + s.guest_poll_ns + s.fence_ns;
    printf("    VMM intercept(trap/hypercall): %10.1f ns\n", s.vmm_trap_ns);
    printf("    VMM translate+forward:         %10.1f ns\n", s.vmm_xlat_ns);
    printf("    Guest poll completion:         %10.1f ns\n", s.guest_poll_ns);
    printf("    Guest fence:                   %10.1f ns\n", s.fence_ns);
    printf("    合计虚拟化开销:                %10.1f ns\n", virt_total);
    printf("    其中 IOFENCE 同步等待:         %10.1f ns\n", s.iofence_wait_total_ns);
    printf("  ---- unmap 关键路径延迟 (unmap -> 可回收GPA) ----\n");
    if (s.unmap_path_samples > 0) {
        printf("    样本数: %lu   平均: %.1f ns   最大: %.1f ns\n",
               (unsigned long)s.unmap_path_samples,
               s.unmap_path_total_ns / s.unmap_path_samples, s.unmap_path_max_ns);
    } else {
        printf("    (无样本)\n");
    }
    printf("  ---- vIOMMU 交互计数 ----\n");
    printf("    vCQ writes: %lu   doorbells: %lu   VMM intercepts: %lu\n",
           (unsigned long)s.vcq_writes, (unsigned long)s.vcq_doorbells,
           (unsigned long)s.vmm_intercepts);
    printf("================================================================\n");
    fflush(stdout);
}