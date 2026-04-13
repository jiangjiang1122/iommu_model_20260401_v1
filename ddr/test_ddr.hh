#ifndef TEST_DDR_HH
#define TEST_DDR_HH

#include "systemc.h"
#include "tlm.h"
#include "tlm_utils/simple_target_socket.h"
#include "param_trans_def.hh"  // 包含 PayloadExtention 定义
#include <cstring>

using namespace std;
using namespace sc_core;
using namespace tlm;
using namespace tlm_utils;

class DDR_Module : public sc_module {
public:
    // 模拟 DDR 存储器的数组 - 1MB 存储空间
    unsigned char memory[1024 * 1024]; // 1MB of simulated memory

public:
    // 与 IOMMU 模块的 initiator socket 对应的 target socket (slave)
    simple_target_socket<DDR_Module, 64> axi_slave_from_cmn_rnd_socket;
    simple_target_socket<DDR_Module, 64> axi_slave_from_pcie_noc_0_socket;
    simple_target_socket<DDR_Module, 64> axi_slave_from_cmn_rnd_1_socket;

    SC_HAS_PROCESS(DDR_Module);

    DDR_Module(sc_module_name name) : sc_module(name) {
        // 初始化模拟内存
        memset(memory, 0, sizeof(memory));
        
        // 只注册阻塞传输处理函数
        axi_slave_from_cmn_rnd_socket.register_b_transport(this, &DDR_Module::b_transport);
        axi_slave_from_pcie_noc_0_socket.register_b_transport(this, &DDR_Module::b_transport);
        axi_slave_from_cmn_rnd_1_socket.register_b_transport(this, &DDR_Module::b_transport);
    }
    
    // 处理 DDR 访问
    void process_ddr_access(tlm_generic_payload& trans) {
        tlm_command cmd = trans.get_command();
        sc_dt::uint64 addr = trans.get_address();
        unsigned char* data = trans.get_data_ptr();
        unsigned int len = trans.get_data_length();
        
        // 检查地址是否超出模拟内存范围
        if (addr + len > sizeof(memory)) {
            printf("[DDR] ERROR: Address 0x%lx + len %d exceeds memory size %zu\n", addr, len, sizeof(memory));
            trans.set_response_status(TLM_ADDRESS_ERROR_RESPONSE);
            return;
        }
        
        printf("[DDR] %s transaction - addr=0x%lx, len=%d\n", 
               (cmd == TLM_READ_COMMAND) ? "READ" : "WRITE", addr, len);
        fflush(stdout);
        
        if (cmd == TLM_READ_COMMAND) {
            // 执行读操作：从模拟内存中复制数据到传输的数据指针
            memcpy(data, &memory[addr], len);
            
            // 打印读取的数据（前 16 字节）
            printf("[DDR] READ data from 0x%lx: ", addr);
            for (int i = 0; i < min(len, 16U); i++) {
                printf("%02x", memory[addr + i]);
                if ((i + 1) % 8 == 0) printf(" ");
            }
            printf("\n");
            fflush(stdout);
            
            // 如果是读取 DC（64 字节），打印详细结构
            if (len == 64) {
                uint64_t* ptr = (uint64_t*)&memory[addr];
                printf("[DDR] DC data at 0x%lx: [0]=0x%lx, [1]=0x%lx\n", addr, ptr[0], ptr[1]);
            }
        } else if (cmd == TLM_WRITE_COMMAND) {
            // 执行写操作：从传输的数据指针复制数据到模拟内存
            memcpy(&memory[addr], data, len);
            
            // 打印写入的数据（前 16 字节）
            printf("[DDR] WRITE data to 0x%lx: ", addr);
            for (int i = 0; i < min(len, 16U); i++) {
                printf("%02x", data[i]);
                if ((i + 1) % 8 == 0) printf(" ");
            }
            printf("\n");
            fflush(stdout);
            
            // 如果是写入 DC（64 字节），打印详细结构
            if (len == 64) {
                uint64_t* ptr = (uint64_t*)data;
                printf("[DDR] DC data at 0x%lx: [0]=0x%lx, [1]=0x%lx\n", addr, ptr[0], ptr[1]);
            }
        }
        
        trans.set_response_status(TLM_OK_RESPONSE);
    }
    
    void b_transport(tlm_generic_payload& trans, sc_time& delay) {
        // 阻塞模式处理
        process_ddr_access(trans);
        delay = sc_time(10, SC_NS);  // 10ns 延迟
    }
};
#endif
