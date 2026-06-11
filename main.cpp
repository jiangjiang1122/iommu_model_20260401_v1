#include <systemc.h>
#include <tlm.h>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <cstdio>

#include "param_trans_def.hh"
#include "iommu_struct.hh"
#include "iommu_top.hh"
#include "rp/test_rp.hh"
#include "pcienoc/test_pcienoc.hh"
#include "ddr/test_ddr.hh"
#include "slink/test_slink.hh"

// Define debug macros if DEBUG is defined
#ifdef DEBUG
#ifndef DEBUG_MSITRANS
#define DEBUG_MSITRANS
#endif
#ifndef DEBUG_TRANSLATION
#define DEBUG_TRANSLATION
#endif
#ifndef DEBUG_TWOSTAGE
#define DEBUG_TWOSTAGE
#endif
#ifndef DEBUG_SECONDSTAGE
#define DEBUG_SECONDSTAGE
#endif
#ifndef DEBUG_COMMANDS
#define DEBUG_COMMANDS
#endif
#endif

using namespace std;
using namespace tlm;
using namespace tlm_utils;
using namespace sc_core;

int sc_main(int argc, char *argv[]) {
    // Display SystemC version information
    printf("%s\n", sc_release());
    printf("Copyright (c) 1996-2018 by all Contributors,\n");
    printf("ALL RIGHTS RESERVED\n");
    fflush(stdout);  // Force flush output buffer
    
    printf("Starting RISC-V IOMMU SystemC Model\n");
    fflush(stdout);  // Force flush output buffer

    // 创建IOMMU顶层模块实例
    iommu_top* iommu = new iommu_top("iommu");

    // 创建DDR模块实例
    DDR_Module* ddr = new DDR_Module("ddr");

    // 创建RP模块实例，传入IOMMU和DDR模块指针
    RP_Module* rp = new RP_Module("rp", iommu, ddr);

    // 创建PCIENOC模块实例
    PCIENOC_Module* pcienoc = new PCIENOC_Module("pcienoc");

    // 创建SLINK模块实例 (IOMMU到DDR页表访问的NoC路径模型)
    SLINK_Module* slink = new SLINK_Module("slink");

    // 绑定IOMMU的initiator sockets到DDR模块的target sockets
    iommu->axi_stream_socket.bind(ddr->axi_slave_from_cmn_rnd_socket);
    
    iommu->axi_master_0_to_pcie_noc_to_cmn_rni_socket.bind(ddr->axi_slave_from_pcie_noc_0_socket);
    
    // IOMMU -> SLINK -> DDR (页表/目录表访问路径，中间经过NoC)
    iommu->axi_master_1_to_cmn_rnd_socket.bind(slink->targ_socket);
    slink->init_socket.bind(ddr->axi_slave_from_cmn_rnd_1_socket);
    
    iommu->axi_master_2_ats_msg_to_pcie_noc_socket.bind(rp->axi_slave_to_pcie_noc_0_socket);

    // 绑定RP模块的initiator socket到IOMMU的target socket
    rp->axi_master_to_pcie_noc_0_socket.bind(iommu->axi_slave_from_pcie_noc_0_socket);
    
    // 绑定PCIENOC模块的initiator socket到IOMMU的target socket
    pcienoc->ahb_master_to_pcie_noc_1_socket.bind(iommu->ahb_slave_from_pcie_noc_1_socket);

    // 运行仿真，设置仿真时间为1000纳秒
    printf("IOMMU simulation start\n");
    sc_core::sc_start(200000000, sc_core::SC_NS);  // 200ms for Buffer 512 test

    // 清理内存
    delete pcienoc;
    delete rp;
    delete slink;
    delete ddr;
    delete iommu;

    printf("IOMMU simulation completed\n");
    return 0;
}