#include "iommu_struct.hh"
#include "iommu_top.hh"
#include <cstring>  // for memset

// Global variables for page allocation
int8_t *memory = nullptr;
uint64_t access_viol_addr = -1;
uint64_t data_corruption_addr = -1;
uint64_t next_free_page = 0;
uint64_t next_free_gpage[65536] = {0};  // Initialize all entries to 0
uint8_t pr_go_requested = 0;
uint8_t pw_go_requested = 0;
int test_endian = LITTLE_ENDIAN;  // 默认小端序

// 定义缺失的常量
#define ACCESS_FAULT 1
#define DATA_CORRUPTION 2
#define PMA 0  // 物理内存属性，通常为0
#define BIG_ENDIAN 1
#define LITTLE_ENDIAN 0
#define RVI_IOMMU_READ 0
#define RVI_IOMMU_WRITE 1
#define RVI_IOMMU_ATOMIC 2

uint8_t read_memory(iommu_t *iommu, uint64_t addr, uint8_t size, char *data,
                    uint32_t rcid, uint32_t mcid, uint32_t pma, int endian)
{
    read_memory_test(iommu, addr, size, data);
    return 0;
}

uint8_t read_memory_for_AMO(iommu_t *iommu, uint64_t address, uint8_t size, char *data,
                            uint32_t rcid, uint32_t mcid, uint32_t pma,
                            int endian)
{
    read_memory_test(iommu, address, size, data);
    return 0;
}

uint8_t write_memory(iommu_t *iommu, char *data, uint64_t address, uint32_t size,
                     uint32_t rcid, uint32_t mcid, uint32_t pma,
                     int endian)
{
    write_memory_test(iommu, data, address, size);
    return 0;
}

uint8_t read_memory_test(iommu_t *iommu, uint64_t addr, uint8_t size, char *data)
{
    printf("[IOMMU_REF_API] read_memory_test: addr = 0x%lx, size = %d\n",addr,size);

    // v2: Route through ctrl_path_req_ddr_fifo for non-blocking DDR access
    ctrl_path_ddr_req_t req;
    req.addr = addr;
    req.size = size;
    req.is_write = false;
    memset(req.write_data, 0, sizeof(req.write_data));

    iommu->top->ctrl_path_req_ddr_fifo.write(req);
    wait(iommu->top->ctrl_path_rsp_event);

    // Copy response data from ctrl_path_rsp_buf
    memcpy(data, iommu->top->ctrl_path_rsp_buf, size);

    printf("[IOMMU_REF_API] After ctrl_path READ - data[0-7]: 0x%lx\n", *(uint64_t*)data);
    return 0;
}

uint8_t write_memory_test(iommu_t *iommu, char *data, uint64_t address, uint32_t size)
{
    printf("IOMMU:write_memory_test: addr = 0x%lx, size = %d\n",(unsigned long)address,size);

    // v2: Route through ctrl_path_req_ddr_fifo for non-blocking DDR access
    ctrl_path_ddr_req_t req;
    req.addr = address;
    req.size = size;
    req.is_write = true;
    memcpy(req.write_data, data, (size <= 64) ? size : 64);

    iommu->top->ctrl_path_req_ddr_fifo.write(req);
    wait(iommu->top->ctrl_path_rsp_event);

    return 0;
}

// 为了向后兼容，提供不带iommu参数的版本
uint8_t read_memory_test(uint64_t addr, uint8_t size, char *data)
{
    // 使用默认的iommu实例或者返回模拟数据
    // 这里我们只是简单地将内存区域复制到数据缓冲区
    if (addr == access_viol_addr) return ACCESS_FAULT;
    if (addr == data_corruption_addr) return DATA_CORRUPTION;
    if(memory) {
        memcpy(data, &memory[addr], size);
    } else {
        // 如果内存未初始化，填充零
        memset(data, 0, size);
    }
    return 0;
}

uint8_t write_memory_test(char *data, uint64_t addr, uint32_t size)
{
    // 将数据写入模拟内存
    if (addr == access_viol_addr) return ACCESS_FAULT;
    if (addr == data_corruption_addr) return DATA_CORRUPTION;
    if(memory) {
        memcpy(&memory[addr], data, size);
    }
    return 0;
}

void iommu_to_hb_do_global_observability_sync(uint8_t PR, uint8_t PW)
{

}

void send_msg_iommu_to_hb(iommu_t *iommu,ats_msg_t *prgr)
{
    uint64_t address;
    uint32_t size = 8; // 64 Byte

    // get bus id from request id
    // Bus ID (Bus Number)：8 位
    // Device ID (Device Number)：5 位
    // Function ID (Function Number)：3 位

    uint32_t bus_id = ((prgr->RID) >> 8) & 0xFF;
    address = iommu->internal_reg_file.rc_bus_range[bus_id];    

    // 创建 TLM 通用负载
    tlm::tlm_generic_payload trans;
    PayloadExtention* new_ext = new PayloadExtention();
    sc_time delay = SC_ZERO_TIME;

    // 设置负载属性
    trans.set_command(tlm::TLM_WRITE_COMMAND);
    trans.set_address(address);
    trans.set_data_ptr(reinterpret_cast<unsigned char*>(&prgr->PAYLOAD));
    trans.set_data_length(size);
    trans.set_streaming_width(size);
    trans.set_byte_enable_ptr(nullptr);
    trans.set_dmi_allowed(false);
    trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);

    new_ext->msg_code = prgr->MSGCODE;
    new_ext->tag = prgr->TAG;

    new_ext->requester_id = prgr->RID;
    new_ext->pid_valid = prgr->PV;
    new_ext->process_id = prgr->PID;
    new_ext->priv_req = prgr->PRIV;

    new_ext->exec_req = prgr->EXEC_REQ;    
    new_ext->tag = 0;
    new_ext->tag = prgr->DSEG;

    trans.set_extension(new_ext);

    // 调用 b_transport
    iommu->top->axi_master_2_to_pcie_noc_socket->b_transport(trans, delay);

    // 检查响应状态
    if (trans.get_response_status() != tlm::TLM_OK_RESPONSE) {
        // 处理错误
        return; // 函数返回void，不能返回值
    }
    return;
}

void get_attribs_from_req(hb_to_iommu_req_t *req, uint8_t *read,
                          uint8_t *write, uint8_t *exec, uint8_t *priv)
{
*read = (req->tr.read_writeAMO == READ && req->exec_req && req->tr.at == ADDR_TYPE_UNTRANSLATED) ?
            0 : ( req->tr.read_writeAMO == READ ) ? 1 : 0;

    *write = ( req->tr.read_writeAMO == WRITE ) ?  1 : 0;

    // The No Write flag, when Set, indicates that the Function is requesting read-only
    // access for this translation.
    // The TA (IOMMU) may ignore the No Write Flag, however, if the TA responds with a
    // translation marked as read-only then the Function must not issue Memory Write
    // transactions using that translation. In this case, the Function may issue another
    // translation request with the No Write flag Clear, which may result in a new
    // translation completion with or without the W (Write) bit Set.
    // Upon receiving a Translation Request with the NW flag Clear, TAs are permitted to
    // mark the associated pages dirty. Functions MUST not issue such Requests
    // unless they have been given explicit write permission.
    // Note ATS Translation requests are read - so read_writeAMO is READ for these requests
    *write = ( (req->tr.at == ADDR_TYPE_PCIE_ATS_TRANSLATION_REQUEST) &&
                 (req->no_write == 0) ) ? 1 : *write;

    // If a Translation Request has a PASID, the Untranslated Address Field is an address
    // within the process address space indicated by the PASID field.
    // If a Translation Request has a PASID with either the Privileged Mode Requested
    // or Execute Requested bit Set, these may be used in constructing the Translation
    // Completion Data Entry.  The PASID Extended Capability indicates whether a Function
    // supports and is enabled to send and receive TLPs with the PASID.
    *exec = ( req->tr.read_writeAMO == READ && (req->exec_req &&
                (req->tr.at == ADDR_TYPE_UNTRANSLATED || req->pid_valid)) ) ? 1 : 0;
    *priv = ( req->pid_valid && req->priv_req ) ? S_MODE : U_MODE;
    return;
}

/*
该函数用于处理虚拟中断文件（MRIF）的地址重叠问题，主要功能包括：

1. MRIF 地址检测 ：检查给定的 GPA 是否落在 MRIF 地址范围内
2. 页大小调整 ：当检测到 MRIF 地址时，将页大小调整为 4KB
3. 地址对齐检查 ：验证 MRIF 地址是否正确对齐
*/
void handle_virtual_interrupt_file_overlap(device_context_t *DC, uint64_t gpa,
                                          uint64_t *gst_page_sz)
{
    uint64_t m = DC->msi_addr_mask.mask << 12;
    uint64_t p = DC->msi_addr_pattern.pattern << 12;
    // If MSI page table mode is Off or if the initial page sizes is base page
    // size, there is nothing to do.
    if (DC->msiptp.MODE == MSIPTP_Off || *gst_page_sz == PAGESIZE)
        return;
    for (uint64_t sz = PAGESIZE << 36; sz >= (PAGESIZE << 9); sz >>= 9) {
        uint64_t mask = m & ~(sz - 1);
        *gst_page_sz = (*gst_page_sz >= sz && ((gpa & mask) == (p & mask))) ?
                       (sz >> 9) : *gst_page_sz;
    }
}

uint64_t get_free_ppn(uint64_t num_ppn) {
    uint64_t free_ppn;
    if (next_free_page & (num_ppn - 1)) {
        next_free_page = next_free_page + (num_ppn - 1);
        next_free_page = next_free_page & ~(num_ppn - 1);
    }
    free_ppn = next_free_page;
    next_free_page += num_ppn;
    if (memory) {
        memset(&memory[free_ppn * PAGESIZE], 0, num_ppn * PAGESIZE);
    }
    return free_ppn;
}

uint64_t get_free_gppn(uint64_t num_gppn, iohgatp_t iohgatp) {
    uint64_t free_gppn = next_free_gpage[iohgatp.GSCID];

    if (free_gppn & (num_gppn - 1)) {
        free_gppn = free_gppn + (num_gppn - 1);
        free_gppn = free_gppn & ~(num_gppn - 1);
    }
    next_free_gpage[iohgatp.GSCID] = free_gppn + num_gppn;
    return free_gppn;
}