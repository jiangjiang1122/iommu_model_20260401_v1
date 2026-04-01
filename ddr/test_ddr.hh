#ifndef TEST_DDR_HH
#define TEST_DDR_HH

#include "systemc.h"
#include "tlm.h"
#include "tlm_utils/simple_target_socket.h"
#include "tlm_utils/simple_initiator_socket.h"
#include <cstring>  // for memcpy and memset

using namespace std;
using namespace sc_core;
using namespace tlm;
using namespace tlm_utils;

class DDR_Module : public sc_module {
public:
    // 模拟DDR存储器的数组 - 1MB存储空间
    unsigned char memory[1024 * 1024]; // 1MB of simulated memory

public:
    // 与IOMMU模块的initiator socket对应的target socket (slave)
    simple_target_socket<DDR_Module, 64> axi_slave_from_cmn_rnd_socket;
    simple_target_socket<DDR_Module, 64> axi_slave_from_pcie_noc_0_socket;
    simple_target_socket<DDR_Module, 64> axi_slave_from_cmn_rnd_1_socket;

    SC_HAS_PROCESS(DDR_Module);

    DDR_Module(sc_module_name name) : sc_module(name) {
        // 初始化模拟内存
        memset(memory, 0, sizeof(memory));
        
        // 注册传输处理函数
        axi_slave_from_cmn_rnd_socket.register_b_transport(this, &DDR_Module::b_transport);
        axi_slave_from_pcie_noc_0_socket.register_b_transport(this, &DDR_Module::b_transport);
        axi_slave_from_cmn_rnd_1_socket.register_b_transport(this, &DDR_Module::b_transport);
    }
    
    void b_transport(tlm_generic_payload& trans, sc_time& delay) {
        tlm_command cmd = trans.get_command();
        sc_dt::uint64 addr = trans.get_address();
        unsigned char* data = trans.get_data_ptr();
        unsigned int len = trans.get_data_length();
        
        // 检查地址是否超出模拟内存范围
        if (addr + len > sizeof(memory)) {
            trans.set_response_status(TLM_ADDRESS_ERROR_RESPONSE);
            return;
        }
        
        if (cmd == TLM_READ_COMMAND) {
            // 执行读操作：从模拟内存中复制数据到传输的数据指针
            memcpy(data, &memory[addr], len);
        } else if (cmd == TLM_WRITE_COMMAND) {
            // 执行写操作：从传输的数据指针复制数据到模拟内存
            memcpy(&memory[addr], data, len);
        }
        
        trans.set_response_status(TLM_OK_RESPONSE);
    }
};
#endif