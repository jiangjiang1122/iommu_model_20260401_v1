# Makefile for RISC-V IOMMU SystemC Model
#
# Test scenario selection:
#   make TEST=rand4k_singlestage      (default) 4KB随机读 + 仅一级地址翻译, 4000 requests, 16MB范围
#   make TEST=msi_perf                MSI地址翻译(Flat+MRIF+故障+MSIPT Cache命中) 功能验证
#   make TEST=seq128k_singlestage     128KB顺序读 + 仅一级地址翻译, 2000 requests, 1MB范围
#   make TEST=sv48_bare               Sv48 + Bare, 1000 requests
#   make TEST=seq128k_twostage        128KB顺序读 + Sv48/Sv48x4两阶段地址翻译, 5000 requests
#   make TEST=rand4k_twostage         4KB随机读 + Sv48/Sv48x4两阶段地址翻译, 4000 requests
#   make test_dedup_unit              PT Cache去重+预取单元测试
#   make test_dedup_integration       PT Cache去重+预取集成测试
#
TEST ?= rand4k_singlestage

# Compiler settings
CXX = g++
CC = gcc

# SystemC settings - adjust these paths if SystemC is installed elsewhere
# For Linux, using system-installed SystemC
SYSTEMC_PREFIX = /usr
SYSTEMC_INCLUDE = /usr/include
SYSTEMC_LIB = /usr/lib/x86_64-linux-gnu

# Compiler flags
CXXFLAGS = -std=c++17 -w -I$(SYSTEMC_INCLUDE) -I. -I./iommu -I./iommu/include -I./iommu/iommu_fun_model -I./iommu/iommu_perf_model -I./iommu/cache_src -I./iommu/cache_src/cache -I./iommu/cache_src/common -I./iommu/cache_src/replacement -I./iommu/cache_src/subsystem -I./slink -DSC_INCLUDE_DYNAMIC_PROCESSES -DSC_DISABLE_API_VERSION_CHECK $(TEST_FLAGS) $(EXTRA_FLAGS)

# Debug/Release build
DEBUG ?= 1
ifeq ($(DEBUG), 1)
    CXXFLAGS += -g -O0 -DDEBUG -DDEBUG_TRANSLATION -DDEBUG_TWOSTAGE -DDEBUG_MSITRANS -DDEBUG_COMMANDS -DDEBUG_SECONDSTAGE -DDEBUG_ATC -DDEBUG_FAULTS -DDEBUG_INTERRUPT -DDEBUG_HPM -DDEBUG_UTILS
    CFLAGS += -g -O0 -DDEBUG
else
    CXXFLAGS += -O3 -DNDEBUG
    CFLAGS += -O3 -DNDEBUG
endif

# Libraries
LIBS = -lsystemc -Wl,--no-as-needed -lpthread -lm

# Test thread file selection and flags based on TEST scenario
# Each scenario has a dedicated source file with clear naming:
#   - test_rp_seq128k_single_stage_thread.cc   : 128KB顺序读 + 单级翻译
#   - test_rp_seq128k_two_stage_thread.cc      : 128KB顺序读 + 两阶段翻译
#   - test_rp_rand4k_single_stage_thread.cc    : 4KB随机读 + 单级翻译
#   - test_rp_sv48_bare_thread.cc              : Sv48+Bare基础测试
ifeq ($(TEST), sv48_bare)
    TEST_THREAD_SRC = rp/test_rp_sv48_bare_thread.cc
    TEST_FLAGS =
else ifeq ($(TEST), seq128k_singlestage)
    TEST_THREAD_SRC = rp/test_rp_seq128k_single_stage_thread.cc
    TEST_FLAGS =
else ifeq ($(TEST), seq128k_twostage)
    TEST_THREAD_SRC = rp/test_rp_seq128k_two_stage_thread.cc
    TEST_FLAGS = -DTEST_SEQ_128K -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=0
else ifeq ($(TEST), seq128k_twostage_s2on)
    TEST_THREAD_SRC = rp/test_rp_seq128k_two_stage_thread.cc
    TEST_FLAGS = -DTEST_SEQ_128K -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1
else ifeq ($(TEST), seq128k_twostage_s2on_128g)
    # 场景6: 128KB顺序读 + 两阶段 + S2开启 + 128GB/s入口/出口 + 全局并发512 + PTW并发5
    #   基准: 稳态IOPS=250M(100%效率), 纯10000包(跳过Phase1单包)
    TEST_THREAD_SRC = rp/test_rp_seq128k_two_stage_thread.cc
    TEST_FLAGS = -DTEST_SEQ_128K -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
                 -DTEST_CFG_AXI_PORT_WIDTH_BIT=1024 \
                 -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=512 \
                 -DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=5 \
                 -DTEST_CFG_SKIP_PHASE1=1
else ifeq ($(TEST), cache_inval)
    # [失效] 缓存失效功能测试: DC/PC/PT/Walker 失效 + CQ 命令通路
    #   复用场景5两阶段配置(D=3预取 + Walker Cache + S2), 小规模批次访问,
    #   每轮下发一条失效指令+IOFENCE, 用 PTW 任务增量判定失效是否生效
    TEST_THREAD_SRC = rp/test_rp_cache_inval_thread.cc
    TEST_FLAGS = -DTEST_SEQ_128K -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
                 -DTEST_CFG_SKIP_PHASE1=1
else ifeq ($(TEST), rand4k_twostage)
    TEST_THREAD_SRC = rp/test_rp_rand4k_two_stage_thread.cc
    TEST_FLAGS = -DTEST_RAND_4K -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1
else ifeq ($(TEST), seq512b_2mb_twostage_s2on)
    # 场景8: 512B步进顺序递增 + VS/G两级均为2MB大页 + 两阶段 + S2开启
    #   复用场景5配置: 64GB/s(512bit)入口/出口 + 全局并发256 + PTW并发4 + D=3预取
    #   大页语义: PT Cache只查不写(命中率0), Walker Cache缓存端到端2MB leaf,
    #   PTW大页不启动预取(仍返回D+1个结果, D个无效), 首包walk最多15次DDR
    SCENE8_PTW ?= 4
    TEST_THREAD_SRC = rp/test_rp_seq512b_2mb_two_stage_thread.cc
    TEST_FLAGS = -DTEST_SEQ_512B_2MB -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
                 -DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=$(SCENE8_PTW) \
                 -DTEST_CFG_SKIP_PHASE1=1
else ifeq ($(TEST), seq512b_2mb_twostage_s2on_128g)
    # 场景9: 场景8的高带宽版 - 512B步进顺序递增 + VS/G两级均2MB大页 + 两阶段 + S2开启
    #   128GB/s(1024bit)入口/出口 + 全局并发512 + Buffer512(绑定跟随) + D=3预取
    #   PTW并发可用 make SCENE9_PTW=N 调参(大页任务exec仅~37ns, 默认4理论足够)
    #   目标: 验证大页路径在翻倍带宽下能否翻倍到250M线速
    SCENE9_PTW ?= 4
    TEST_THREAD_SRC = rp/test_rp_seq512b_2mb_two_stage_thread.cc
    TEST_FLAGS = -DTEST_SEQ_512B_2MB -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
                 -DTEST_CFG_AXI_PORT_WIDTH_BIT=1024 \
                 -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=512 \
                 -DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=$(SCENE9_PTW) \
                 -DTEST_CFG_SKIP_PHASE1=1
else ifeq ($(TEST), rand4k_twostage_s2on_128g)
    # 场景7: 4KB随机读(16MB IOVA范围) + 两阶段 + S2开启 + 128GB/s入口/出口
    #   全局并发512 + Buffer512(可用 make SCENE7_BUFFER=N 覆盖) + PTW并发24(可用 make SCENE7_PTW=N 覆盖调参) + D=3预取(随机IOVA下预取失效) + 10000包(1250页x8)
    SCENE7_PTW ?= 24
    SCENE7_BUFFER ?= 512
    TEST_THREAD_SRC = rp/test_rp_rand4k_two_stage_thread.cc
    TEST_FLAGS = -DTEST_RAND_4K -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
                 -DTEST_CFG_AXI_PORT_WIDTH_BIT=1024 \
                 -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=512 \
                 -DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=$(SCENE7_PTW) \
                 -DTEST_CFG_PT_DEDUP_BUFFER_SIZE=$(SCENE7_BUFFER) \
                 -DTEST_CFG_NUM_PAGES=1250
else ifeq ($(TEST), rand4k_twostage_s2on_128g_inval)
    # 场景10: 场景7 + 运行期随机穿插 DC/PC/PT/Walker 缓存失效命令
    #   基础负载与场景7逐包等价(相同随机种子/相同IOVA序列), 便于直接对比性能;
    #   失效注入独立RNG, 不扰动IOVA序列。
    #   平均每 SCENE10_INVAL_PERIOD 个请求穿插一条失效指令(+IOFENCE),
    #   类型按权重随机(VMA-LAZY/GVMA-LAZY/SCAN_RANGE/NL=1/IODIR.DDT/IODIR.PDT)
    #   验证: 失效扰动下功能100%正确 + PT/Walker延迟失效(LIB/VN)是否生效 + 性能影响
    SCENE10_PTW ?= 4
    SCENE10_INVAL_PERIOD ?= 500
    TEST_THREAD_SRC = rp/test_rp_rand4k_two_stage_inval_thread.cc
    TEST_FLAGS = -DTEST_RAND_4K -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
                 -DTEST_CFG_AXI_PORT_WIDTH_BIT=1024 \
                 -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=512 \
                 -DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=$(SCENE10_PTW) \
                 -DTEST_CFG_NUM_PAGES=1250 \
                 -DTEST_CFG_INVAL_PERIOD_REQS=$(SCENE10_INVAL_PERIOD)
else ifeq ($(TEST), virt_lazy_twostage)
    # 场景11: 虚拟化两级Stage — Lazy 模式 Cache Invalidate (规范6.5.3 场景二)
    #   Guest OS 管理Stage1, unmap后GVA入Flush Queue, Drain时批量发
    #   IOTINVAL.VMA(GV=1,GSCID,PSCID,AV=0) 经vIOMMU->VMM拦截->物理CQ;
    #   VMM 管理Stage2, 累积后批量发 IOTINVAL.GVMA。负载与场景7同源可对比。
    VIRT_FQ_DEPTH ?= 32
    VIRT_TRAP_NS ?= 2000
    VIRT_FQ_TIMEOUT_NS ?= 10000
    TEST_THREAD_SRC = rp/test_rp_virt_lazy_thread.cc
    TEST_FLAGS = -DTEST_RAND_4K -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
                 -DTEST_CFG_AXI_PORT_WIDTH_BIT=1024 \
                 -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=512 \
                 -DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=4 \
                 -DTEST_CFG_NUM_PAGES=1250 \
                 -DTEST_CFG_VIRT_FQ_DEPTH=$(VIRT_FQ_DEPTH) \
                 -DTEST_CFG_VIRT_FQ_TIMEOUT_NS=$(VIRT_FQ_TIMEOUT_NS) \
                 -DTEST_CFG_VMM_TRAP_NS=$(VIRT_TRAP_NS)
else ifeq ($(TEST), virt_strict_twostage)
    # 场景12: 虚拟化两级Stage — Strict 模式 Cache Invalidate (规范6.5.4 场景二)
    #   Guest OS 每次unmap立即发 IOTINVAL.VMA(GV=1,GSCID,PSCID,AV=1,ADDR=GVA)
    #   并等IOFENCE.C完成后才回收GPA; VMM 每次Stage2 unmap后立即发 GVMA。
    #   负载与unmap序列与场景11完全一致, 仅失效策略不同 -> 可直接A/B对比。
    VIRT_TRAP_NS ?= 2000
    TEST_THREAD_SRC = rp/test_rp_virt_strict_thread.cc
    TEST_FLAGS = -DTEST_RAND_4K -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
                 -DTEST_CFG_AXI_PORT_WIDTH_BIT=1024 \
                 -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=512 \
                 -DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=4 \
                 -DTEST_CFG_NUM_PAGES=1250 \
                 -DTEST_CFG_VMM_TRAP_NS=$(VIRT_TRAP_NS)
else ifeq ($(TEST), rand4k_msi_mix_s2on_128g)
    # 场景13-v4(混合性能): 4KB随机读写 + SQ/CQ/MSI 混合负载, 两阶段 + S2开启
    #   128GB/s入口/出口 + 读写分离并发(写320+读320=640) + Buffer320
    #   + PT/Dedup双多RAM(4组) + PTW并发64(可 make SCENE13_PTW=N 覆盖) + D=3预取
    #   [v4] 系统级任务奇读偶写交替: 偶数组=8x512B全读, 奇数组=8x512B全写
    #   [v4] SQ改为32B读请求(原为写)
    #   [v4] SQ/CQ经预热后100%命中PT Cache, 不进dedup/PTW
    #   每组 = 8x512B Data + 1x32B SQ读 + 1x16B CQ写 + 1x4B MSI写 = 11包
    #   909组 x 11包 + 1 MSI = 10000包 (Data 7272 + SQ 909 + CQ 909 + MSI 910)
    #   IOPS只统计Data任务(is_ctrl=0), SQ/CQ/MSI(is_ctrl=1)不计入
    SCENE13_PTW ?= 64
    SCENE13_BUFFER ?= 320
    # [准入控制开关] 0=Buffer满bypass直转PTW(默认); 1=准入控制(满则跳过出队, request FIFO排队反压)
    DEDUP_ADM ?= 0
    TEST_THREAD_SRC = rp/test_rp_rand4k_msi_mix_thread.cc
    TEST_FLAGS = -DTEST_RAND_4K -DTEST_TWO_STAGE \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
                 -DTEST_CFG_AXI_PORT_WIDTH_BIT=1024 \
                 -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=640 \
                 -DTEST_CFG_IOMMU_WRITE_MAX_OUTSTANDING=320 \
                 -DTEST_CFG_IOMMU_READ_MAX_OUTSTANDING=320 \
                 -DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=$(SCENE13_PTW) \
                 -DTEST_CFG_PT_DEDUP_BUFFER_SIZE=$(SCENE13_BUFFER) \
                 -DTEST_CFG_DEDUP_ADMISSION_CTRL=$(DEDUP_ADM) \
                 -DTEST_CFG_NUM_PAGES=909
else ifeq ($(TEST), rand4k_msi_mix_s2on_128g_512mb)
    # 场景13-v4-512MB(混合性能): IOVA数据随机范围扩大到512MB, GPA 50个2MB大页(100MB)
    #   基于 rand4k_msi_mix_s2on_128g, 仅IOVA数据随机范围16MB->512MB、GPA大页20->50;
    #   MSI/SQ/CQ的IOVA与GPA上移避免与512MB数据区重叠; VS部分映射(数据页+预取页,多对一随机GPA);
    #   保证所有GPA->SPA有效(50大页G-stage全建), 主/预取任务PTE均有效。
    #   [v2回归] 读写分离并发(写265+读243=508) + Buffer266 + PTW并发27
    SCENE13_PTW ?= 27
    SCENE13_BUFFER ?= 266
    # [负载规模] 组数与末尾MSI数: 默认909组+1尾MSI=10000包; 1818组+2尾MSI=20000包
    SCENE13_PAGES ?= 909
    SCENE13_TAILS ?= 1
    # [准入控制开关] 0=Buffer满bypass直转PTW(默认); 1=准入控制(满则跳过出队, request FIFO排队反压)
    DEDUP_ADM ?= 0
    TEST_THREAD_SRC = rp/test_rp_rand4k_msi_mix_thread.cc
    TEST_FLAGS = -DTEST_RAND_4K -DTEST_TWO_STAGE \
                 -DTEST_CFG_S13_IOVA_512MB \
                 -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=3 \
                 -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 \
                 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
                 -DTEST_CFG_AXI_PORT_WIDTH_BIT=1024 \
                 -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=508 \
                 -DTEST_CFG_IOMMU_WRITE_MAX_OUTSTANDING=265 \
                 -DTEST_CFG_IOMMU_READ_MAX_OUTSTANDING=243 \
                 -DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=$(SCENE13_PTW) \
                 -DTEST_CFG_PT_DEDUP_BUFFER_SIZE=$(SCENE13_BUFFER) \
                 -DTEST_CFG_DEDUP_ADMISSION_CTRL=$(DEDUP_ADM) \
                 -DTEST_CFG_NUM_PAGES=$(SCENE13_PAGES) \
                 -DTEST_CFG_TAIL_MSI=$(SCENE13_TAILS)
else ifeq ($(TEST), rand4k_msi_mix_multidev)
    # 多设备V2：单端口、独立VM、逐包轮询，旧场景不启用新增代码。
    MULTIDEV_N ?= 2
    MULTIDEV_IOVA_MB ?= 16
    MULTIDEV_GROUPS ?= 909
    MULTIDEV_QUEUE_DEPTH ?= 32
    MULTIDEV_PATTERN ?= overlap
    MULTIDEV_LOAD_MODE ?= per_device
    MULTIDEV_RR_START ?= 0
    MULTIDEV_D ?= 3
    # [写保序队列] 0=全局单队列(默认); 1=每设备独立写保序队列(队列数=设备数)
    MULTIDEV_PERDEV_WO ?= 0
    DEDUP_ADM ?= 0
    ifeq ($(filter $(MULTIDEV_N),1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16),)
        $(error MULTIDEV_N must be an integer in 1..16)
    endif
    ifeq ($(filter $(MULTIDEV_IOVA_MB),16 512),)
        $(error MULTIDEV_IOVA_MB must be 16 or 512)
    endif
    ifeq ($(filter $(MULTIDEV_PATTERN),overlap independent),)
        $(error MULTIDEV_PATTERN must be overlap or independent)
    endif
    ifeq ($(filter $(MULTIDEV_LOAD_MODE),per_device fixed_total),)
        $(error MULTIDEV_LOAD_MODE must be per_device or fixed_total)
    endif
    ifeq ($(MULTIDEV_LOAD_MODE),fixed_total)
        ifeq ($(filter $(MULTIDEV_N),1 2 4 8 16),)
            $(error fixed_total requires MULTIDEV_N=1/2/4/8/16)
        endif
    endif
    ifeq ($(MULTIDEV_IOVA_MB),512)
        SCENE13_PTW ?= 27
        SCENE13_BUFFER ?= 266
        # [并发资源] 默认与单设备512MB基线一致; 可以命令行升级(如 PTW64/Buf320/640/320/320)
        SCENE13_GLOBAL ?= 508
        SCENE13_READ ?= 243
        SCENE13_WRITE ?= 265
        MULTIDEV_LAYOUT_FLAGS = -DTEST_CFG_S13_IOVA_512MB \
            -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=$(SCENE13_GLOBAL) \
            -DTEST_CFG_IOMMU_READ_MAX_OUTSTANDING=$(SCENE13_READ) \
            -DTEST_CFG_IOMMU_WRITE_MAX_OUTSTANDING=$(SCENE13_WRITE)
    else
        SCENE13_PTW ?= 64
        SCENE13_BUFFER ?= 320
        MULTIDEV_LAYOUT_FLAGS = -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=640 \
            -DTEST_CFG_IOMMU_READ_MAX_OUTSTANDING=320 \
            -DTEST_CFG_IOMMU_WRITE_MAX_OUTSTANDING=320
    endif
    TEST_THREAD_SRC = rp/test_rp_rand4k_msi_mix_multidev_thread.cc
    TEST_FLAGS = -DTEST_RAND_4K -DTEST_TWO_STAGE -DTEST_CFG_MULTI_DEVICE_SCENE=1 \
        -DTEST_CFG_NUM_DEVICES=$(MULTIDEV_N) \
        -DTEST_CFG_NUM_PAGES=$(MULTIDEV_GROUPS) \
        -DTEST_CFG_MD_QUEUE_DEPTH=$(MULTIDEV_QUEUE_DEPTH) \
        -DTEST_CFG_MD_INDEPENDENT=$(if $(filter independent,$(MULTIDEV_PATTERN)),1,0) \
        -DTEST_CFG_MD_FIXED_TOTAL=$(if $(filter fixed_total,$(MULTIDEV_LOAD_MODE)),1,0) \
        -DTEST_CFG_MD_RR_START=$(MULTIDEV_RR_START) \
        -DTEST_CFG_PERDEV_WRITE_ORDER=$(MULTIDEV_PERDEV_WO) \
        -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=$(MULTIDEV_D) \
        -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
        -DTEST_CFG_AXI_PORT_WIDTH_BIT=1024 \
        -DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=$(SCENE13_PTW) \
        -DTEST_CFG_PT_DEDUP_BUFFER_SIZE=$(SCENE13_BUFFER) \
        -DTEST_CFG_DEDUP_ADMISSION_CTRL=$(DEDUP_ADM) $(MULTIDEV_LAYOUT_FLAGS)
else ifeq ($(TEST), rand4k_msi_mix_multidev_mc)
    # 多设备 × 每设备多通道: N 设备, 每设备内部 C 通道, 共 N*C 条流, 2D 逐包轮询(流号 i=c*N+d)。
    #   轮询顺序: (d1,c1)..(dN,c1),(d1,c2)..(dN,c2),.. 通道外层、设备内层, 每次访问取1包 -> 各通道
    #   4KB 的 8x512B 高度交错注入。fixed_total: 每设备 GROUPS 组均分到 C 通道(chan_groups),
    #   每设备包数=GROUPS*11+1=10000(tail MSI 只给通道0), 总=N*10000(与 multidev/independent 逐包同量)。
    #   页策略: 全局唯一随机页, N*GROUPS 个 4KB 页两两不重复(跨设备跨通道), 需 N*GROUPS<=RANGE_PAGES-D。
    #   并发默认对齐上一 independent 基线(512MB: PTW64/Buf320/Global640/Read320/Write320/ADM1/PERDEV_WO1)。
    MULTIDEV_MC_N ?= 16
    MULTIDEV_MC_CHAN ?= 8
    MULTIDEV_IOVA_MB ?= 512
    MULTIDEV_GROUPS ?= 909
    MULTIDEV_QUEUE_DEPTH ?= 32
    MULTIDEV_RR_START ?= 0
    MULTIDEV_D ?= 3
    MULTIDEV_PERDEV_WO ?= 1
    DEDUP_ADM ?= 1
    # [读写端口分离] SPLIT 开关(1=读写双端口双发送进程, 0=旧单端口基线); 读/写发送队列深度(近乎无界)
    MULTIDEV_MC_SPLIT_RW ?= 1
    MULTIDEV_MC_RWQ_DEPTH ?= 65536
    # [DDR页表访问延时] SLINK单趟延时(ns): 一次DDR页表访问 = 前向SLINK + DDR内部100 + 回程SLINK。
    #   默认50(合计200ns), 与单设备读写分离基线一致。
    MULTIDEV_MC_SLINK_NS ?= 50
    ifeq ($(filter $(MULTIDEV_MC_N),1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16),)
        $(error MULTIDEV_MC_N must be an integer in 1..16)
    endif
    ifeq ($(filter $(MULTIDEV_MC_CHAN),1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16),)
        $(error MULTIDEV_MC_CHAN must be an integer in 1..16)
    endif
    ifeq ($(filter $(MULTIDEV_IOVA_MB),16 512),)
        $(error MULTIDEV_IOVA_MB must be 16 or 512)
    endif
    ifeq ($(MULTIDEV_IOVA_MB),512)
        SCENE13_PTW ?= 64
        SCENE13_BUFFER ?= 320
        SCENE13_GLOBAL ?= 640
        SCENE13_READ ?= 320
        SCENE13_WRITE ?= 320
        MULTIDEV_MC_LAYOUT_FLAGS = -DTEST_CFG_S13_IOVA_512MB \
            -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=$(SCENE13_GLOBAL) \
            -DTEST_CFG_IOMMU_READ_MAX_OUTSTANDING=$(SCENE13_READ) \
            -DTEST_CFG_IOMMU_WRITE_MAX_OUTSTANDING=$(SCENE13_WRITE)
    else
        SCENE13_PTW ?= 64
        SCENE13_BUFFER ?= 320
        MULTIDEV_MC_LAYOUT_FLAGS = -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=640 \
            -DTEST_CFG_IOMMU_READ_MAX_OUTSTANDING=320 \
            -DTEST_CFG_IOMMU_WRITE_MAX_OUTSTANDING=320
    endif
    TEST_THREAD_SRC = rp/test_rp_rand4k_msi_mix_multidev_mc_thread.cc
    TEST_FLAGS = -DTEST_RAND_4K -DTEST_TWO_STAGE \
        -DTEST_CFG_MULTIDEV_MC_SCENE=1 \
        -DTEST_CFG_MULTI_DEVICE_SCENE=1 \
        -DTEST_CFG_NUM_DEVICES=$(MULTIDEV_MC_N) \
        -DTEST_CFG_MD_CHAN_PER_DEV=$(MULTIDEV_MC_CHAN) \
        -DTEST_CFG_NUM_PAGES=$(MULTIDEV_GROUPS) \
        -DTEST_CFG_MD_QUEUE_DEPTH=$(MULTIDEV_QUEUE_DEPTH) \
        -DTEST_CFG_MD_RR_START=$(MULTIDEV_RR_START) \
        -DTEST_CFG_PERDEV_WRITE_ORDER=$(MULTIDEV_PERDEV_WO) \
        -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=$(MULTIDEV_D) \
        -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
        -DTEST_CFG_AXI_PORT_WIDTH_BIT=1024 \
        -DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=$(SCENE13_PTW) \
        -DTEST_CFG_PT_DEDUP_BUFFER_SIZE=$(SCENE13_BUFFER) \
        -DTEST_CFG_DEDUP_ADMISSION_CTRL=$(DEDUP_ADM) \
        -DTEST_CFG_SLINK_NOC_LATENCY_NS=$(MULTIDEV_MC_SLINK_NS) \
        -DTEST_CFG_SPLIT_RW_PORTS=$(MULTIDEV_MC_SPLIT_RW) \
        -DTEST_CFG_MC_RW_QUEUE_DEPTH=$(MULTIDEV_MC_RWQ_DEPTH) $(MULTIDEV_MC_LAYOUT_FLAGS)
else ifeq ($(TEST), rand4k_msi_mix_multichan)
    # 单设备多通道激励注入: 同一设备(device_id=0/GSCID=1)内 n 路并发通道,
    #   每通道各自在 IOVA 范围内随机选 4KB(组内 8x512B 连续) + 共享 SQ/CQ/MSI 控制包,
    #   经"逐包轮询仲裁 + 统一输出口"注入(n producer 有界队列 + 1 RR scheduler + 1 watchdog)。
    #   复用多设备追踪: MULTI_DEVICE_SCENE=1 + NUM_DEVICES=1 -> md_trace 按设备(did=0)聚合,
    #   Jain 恒=1(退化, 预期); 入口 4ns(数据)/2ns(控制) 节拍与单流基线逐包一致。
    #   默认对齐九逸单设备基线(512MB/PTW64/Buf320/Global640/Read320/Write320/ADM1), 可命令行覆盖。
    MULTICHAN_N ?= 8
    MULTICHAN_IOVA_MB ?= 512
    MULTICHAN_GROUPS ?= 909
    MULTICHAN_QUEUE_DEPTH ?= 32
    MULTICHAN_D ?= 3
    MULTICHAN_RR_START ?= 0
    # [读写端口分离] 读/写发送队列深度(近乎无界); SPLIT 开关(1=读写双端口双发送进程, 0=旧单端口基线)
    MULTICHAN_RWQ_DEPTH ?= 65536
    MULTICHAN_SPLIT_RW ?= 1
    # [DDR页表访问延时] SLINK单趟延时(ns): 一次DDR页表访问 = 前向SLINK + DDR内部100 + 回程SLINK。
    #   默认50(合计200ns); 传100则合计300ns(100+100+100), 观测页表访问延时上升对性能的影响。
    MULTICHAN_SLINK_NS ?= 50
    # [DDR内部访问延时] DDR内部固定延时(ns): 与SLINK独立叠加, 一次DDR页表访问 = SLINK + DDR内部 + SLINK。
    #   默认100(合计200ns); 传200则合计300ns(50+200+50), 观测DDR颗粒延时上升对性能的影响。
    MULTICHAN_DDR_NS ?= 100
    # [负载口径] fixed_total(默认)=全部通道合计 MULTICHAN_GROUPS(909) 组 + 1 tail MSI = 10000 包
    #            (与单流基线逐包同量), 用于对比"多通道交织"对性能的影响; 工作集小(建表~3.5k页),
    #            可规避 per_channel 大工作集触发的 IOMMU 既有"两阶段预取+S2 Walker Cache大页"缺陷;
    #            per_channel=每通道 MULTICHAN_GROUPS 组(总 n x 10000 包), 注意: n>=8 时大共享工作集
    #            会触发上述预取无效页缺陷(agg_flush require g_pte.V!=0 失败 abort), 暂不推荐。
    MULTICHAN_LOAD_MODE ?= fixed_total
    # [准入控制开关] 默认1(对齐九逸基线ADM1); 0=Buffer满bypass直转PTW
    DEDUP_ADM ?= 1
    ifeq ($(filter $(MULTICHAN_N),1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16),)
        $(error MULTICHAN_N must be an integer in 1..16)
    endif
    ifeq ($(filter $(MULTICHAN_IOVA_MB),16 512),)
        $(error MULTICHAN_IOVA_MB must be 16 or 512)
    endif
    ifeq ($(filter $(MULTICHAN_LOAD_MODE),per_channel fixed_total),)
        $(error MULTICHAN_LOAD_MODE must be per_channel or fixed_total)
    endif
    ifeq ($(MULTICHAN_LOAD_MODE),fixed_total)
        ifeq ($(filter $(MULTICHAN_N),1 2 4 8 16),)
            $(error fixed_total requires MULTICHAN_N=1/2/4/8/16)
        endif
    endif
    # [并发资源] 默认对齐九逸单设备基线; 可命令行升级/降级
    SCENE13_PTW ?= 64
    SCENE13_BUFFER ?= 320
    SCENE13_GLOBAL ?= 640
    SCENE13_READ ?= 320
    SCENE13_WRITE ?= 320
    ifeq ($(MULTICHAN_IOVA_MB),512)
        MULTICHAN_LAYOUT_FLAGS = -DTEST_CFG_S13_IOVA_512MB \
            -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=$(SCENE13_GLOBAL) \
            -DTEST_CFG_IOMMU_READ_MAX_OUTSTANDING=$(SCENE13_READ) \
            -DTEST_CFG_IOMMU_WRITE_MAX_OUTSTANDING=$(SCENE13_WRITE)
    else
        MULTICHAN_LAYOUT_FLAGS = -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=$(SCENE13_GLOBAL) \
            -DTEST_CFG_IOMMU_READ_MAX_OUTSTANDING=$(SCENE13_READ) \
            -DTEST_CFG_IOMMU_WRITE_MAX_OUTSTANDING=$(SCENE13_WRITE)
    endif
    TEST_THREAD_SRC = rp/test_rp_rand4k_msi_mix_multichan_thread.cc
    TEST_FLAGS = -DTEST_RAND_4K -DTEST_TWO_STAGE \
        -DTEST_CFG_MULTI_CHANNEL_SCENE=1 \
        -DTEST_CFG_NUM_CHANNELS=$(MULTICHAN_N) \
        -DTEST_CFG_MC_QUEUE_DEPTH=$(MULTICHAN_QUEUE_DEPTH) \
        -DTEST_CFG_MC_RR_START=$(MULTICHAN_RR_START) \
        -DTEST_CFG_MC_LOAD_MODE=$(if $(filter fixed_total,$(MULTICHAN_LOAD_MODE)),1,0) \
        -DTEST_CFG_MULTI_DEVICE_SCENE=1 \
        -DTEST_CFG_NUM_DEVICES=1 \
        -DTEST_CFG_NUM_PAGES=$(MULTICHAN_GROUPS) \
        -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=$(MULTICHAN_D) \
        -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
        -DTEST_CFG_AXI_PORT_WIDTH_BIT=1024 \
        -DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=$(SCENE13_PTW) \
        -DTEST_CFG_PT_DEDUP_BUFFER_SIZE=$(SCENE13_BUFFER) \
        -DTEST_CFG_DEDUP_ADMISSION_CTRL=$(DEDUP_ADM) \
        -DTEST_CFG_SLINK_NOC_LATENCY_NS=$(MULTICHAN_SLINK_NS) \
        -DTEST_CFG_SPLIT_RW_PORTS=$(MULTICHAN_SPLIT_RW) \
        -DTEST_CFG_MC_RW_QUEUE_DEPTH=$(MULTICHAN_RWQ_DEPTH) \
        -DTEST_CFG_DDR_INTERNAL_LATENCY_NS=$(MULTICHAN_DDR_NS) $(MULTICHAN_LAYOUT_FLAGS)
else ifeq ($(TEST), rand4k_msi_mix_multichan_mps128)
    # [MPS/MRRS=128B 小包场景] 在 rand4k_msi_mix_multichan 基线(单设备/8通道/512MB/DDR页表200ns/
    #   PTW64/Buf320/Global640/Read320/Write320/ADM1/D3, 稳态ALL=250.43 Mreq/s)之上,
    #   只改 PCIe MPS/MRRS 切割粒度: 1个4KB 由 8x512B 改为 32x128B,
    #   组内包数 11 -> 35, 正式包数 10000 -> 31816(=909x35+1), 多通道机制完全不变。
    # [A/B 不变量] ① 入口端口占用逐组相同(8x4ns+3x2ns = 32x1ns+3x2ns = 38ns);
    #   ② 数据总字节相同(7272x512B = 29088x128B = 3723264B), SQ/CQ/MSI 包数与字节不变;
    #   ③ 工作集相同(909个4KB页, 建表逻辑复用); ④ 并发资源相同。
    # [注入节拍] 128B 数据包经入口带宽公式 1000*128*8/1024000 = 1.0ns/包, 即"每1ns注入1包"
    #   由 IOMMU 既有代码自然成立, 无需新增节拍逻辑; 控制包仍为固定2ns。
    # [统计口径] IOPS 以每个小请求包为单位(每个128B数据包 + 每个SQ/CQ/MSI 各计1个任务)。
    #   期望: DATA IOPS 4x(182.63->730.5), ALL IOPS 3.18x(250.43->~797, 控制包数不随MPS变),
    #         带宽与基线一致(67.0 GB/s)。低于此即为 IOMMU 小包处理惩罚。
    # [报告前缀] multichan_mps128_n8_512mb_*.csv, 与基线 multichan_n8_512mb_*.csv 完全隔离。
    MULTICHAN_N ?= 8
    MULTICHAN_IOVA_MB ?= 512
    MULTICHAN_GROUPS ?= 909
    MULTICHAN_QUEUE_DEPTH ?= 32
    MULTICHAN_D ?= 3
    MULTICHAN_RR_START ?= 0
    # [读写端口分离] 读/写发送队列深度(近乎无界); SPLIT 开关(1=读写双端口双发送进程, 0=旧单端口基线)
    MULTICHAN_RWQ_DEPTH ?= 65536
    MULTICHAN_SPLIT_RW ?= 1
    MULTICHAN_SLINK_NS ?= 50
    MULTICHAN_LOAD_MODE ?= fixed_total
    # [MPS/MRRS 粒度(字节)] 默认128; 传512 可退化为基线切割, 用于自检参数化中性。
    MULTICHAN_MPS ?= 128
    DEDUP_ADM ?= 1
    ifeq ($(filter $(MULTICHAN_N),1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16),)
        $(error MULTICHAN_N must be an integer in 1..16)
    endif
    ifeq ($(filter $(MULTICHAN_IOVA_MB),16 512),)
        $(error MULTICHAN_IOVA_MB must be 16 or 512)
    endif
    ifeq ($(filter $(MULTICHAN_LOAD_MODE),per_channel fixed_total),)
        $(error MULTICHAN_LOAD_MODE must be per_channel or fixed_total)
    endif
    ifeq ($(filter $(MULTICHAN_MPS),16 32 64 128 256 512 1024 2048 4096),)
        $(error MULTICHAN_MPS must be a power-of-2 divisor of 4096: 16/32/64/128/256/512/1024/2048/4096)
    endif
    ifeq ($(MULTICHAN_LOAD_MODE),fixed_total)
        ifeq ($(filter $(MULTICHAN_N),1 2 4 8 16),)
            $(error fixed_total requires MULTICHAN_N=1/2/4/8/16)
        endif
    endif
    # [并发资源] 默认对齐九逸单设备基线; 可命令行升级/降级
    SCENE13_PTW ?= 64
    SCENE13_BUFFER ?= 320
    SCENE13_GLOBAL ?= 640
    SCENE13_READ ?= 320
    SCENE13_WRITE ?= 320
    ifeq ($(MULTICHAN_IOVA_MB),512)
        MULTICHAN_MPS_LAYOUT_FLAGS = -DTEST_CFG_S13_IOVA_512MB \
            -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=$(SCENE13_GLOBAL) \
            -DTEST_CFG_IOMMU_READ_MAX_OUTSTANDING=$(SCENE13_READ) \
            -DTEST_CFG_IOMMU_WRITE_MAX_OUTSTANDING=$(SCENE13_WRITE)
    else
        MULTICHAN_MPS_LAYOUT_FLAGS = -DTEST_CFG_IOMMU_GLOBAL_MAX_OUTSTANDING=$(SCENE13_GLOBAL) \
            -DTEST_CFG_IOMMU_READ_MAX_OUTSTANDING=$(SCENE13_READ) \
            -DTEST_CFG_IOMMU_WRITE_MAX_OUTSTANDING=$(SCENE13_WRITE)
    endif
    TEST_THREAD_SRC = rp/test_rp_rand4k_msi_mix_multichan_mps128_thread.cc
    TEST_FLAGS = -DTEST_RAND_4K -DTEST_TWO_STAGE \
        -DTEST_CFG_MC_MPS_SCENE=1 \
        -DTEST_CFG_MPS_BYTES=$(MULTICHAN_MPS) \
        -DTEST_CFG_IO_PACKET_BYTES=$(MULTICHAN_MPS) \
        -DTEST_CFG_NUM_CHANNELS=$(MULTICHAN_N) \
        -DTEST_CFG_MC_QUEUE_DEPTH=$(MULTICHAN_QUEUE_DEPTH) \
        -DTEST_CFG_MC_RR_START=$(MULTICHAN_RR_START) \
        -DTEST_CFG_MC_LOAD_MODE=$(if $(filter fixed_total,$(MULTICHAN_LOAD_MODE)),1,0) \
        -DTEST_CFG_MULTI_DEVICE_SCENE=1 \
        -DTEST_CFG_NUM_DEVICES=1 \
        -DTEST_CFG_NUM_PAGES=$(MULTICHAN_GROUPS) \
        -DTEST_CFG_PT_DEDUP_PREFETCH_DEPTH=$(MULTICHAN_D) \
        -DTEST_CFG_PTW_WALKER_CACHE_ENABLED=1 -DTEST_CFG_WALKER_CACHE_S2_ENABLED=1 \
        -DTEST_CFG_AXI_PORT_WIDTH_BIT=1024 \
        -DTEST_CFG_PTW_MAX_OUTSTANDING_TASKS=$(SCENE13_PTW) \
        -DTEST_CFG_PT_DEDUP_BUFFER_SIZE=$(SCENE13_BUFFER) \
        -DTEST_CFG_DEDUP_ADMISSION_CTRL=$(DEDUP_ADM) \
        -DTEST_CFG_SLINK_NOC_LATENCY_NS=$(MULTICHAN_SLINK_NS) \
        -DTEST_CFG_SPLIT_RW_PORTS=$(MULTICHAN_SPLIT_RW) \
        -DTEST_CFG_MC_RW_QUEUE_DEPTH=$(MULTICHAN_RWQ_DEPTH) $(MULTICHAN_MPS_LAYOUT_FLAGS)
else ifeq ($(TEST), msi_perf)
    # MSI地址翻译性能模型功能验证 (方案修订v2)
    #   三设备覆盖: 场景A(S1=Bare直达MSIPT) / 场景B(两级PTW S1后识别+is_msi回填)
    #   / 场景C(单级S1-only)。请求数 make TEST=msi_perf MSI_N=1/10/100/1000 递增验证。
    MSI_N ?= 10
    TEST_THREAD_SRC = rp/test_rp_msi_perf_thread.cc
    TEST_FLAGS = -DTEST_CFG_MSI_REQS=$(MSI_N)
else
    # rand4k_singlestage (default)
    TEST_THREAD_SRC = rp/test_rp_rand4k_single_stage_thread.cc
    TEST_FLAGS =
endif

# Source files
CXX_SOURCES = \
    iommu/iommu_fun_model/iommu_utils.cc \
    iommu/iommu_perf_model/iommu_reg.cc \
    iommu/iommu_fun_model/iommu_atc.cc \
    iommu/iommu_fun_model/iommu_ats.cc \
    iommu/iommu_perf_model/iommu_command_queue.cc \
    iommu/iommu_perf_model/iommu_device_context.cc \
    iommu/iommu_fun_model/iommu_faults.cc \
    iommu/iommu_perf_model/iommu_hpm.cc \
    iommu/iommu_fun_model/iommu_interrupt.cc \
    iommu/iommu_fun_model/iommu_msi_trans.cc \
    iommu/iommu_fun_model/iommu_process_context.cc \
    iommu/iommu_perf_model/iommu_ref_api.cc \
    iommu/iommu_fun_model/iommu_second_stage_trans.cc \
    iommu/iommu_fun_model/iommu_two_stage_trans.cc \
    iommu/iommu_fun_model/iommu_translate.cc \
    iommu/iommu_top.cc \
    iommu/iommu_perf_model/iommu_perf_parser.cc \
    iommu/iommu_perf_model/iommu_perf_collector.cc \
    iommu/iommu_perf_model/iommu_perf_pt_cache_response.cc \
    iommu/iommu_perf_model/iommu_perf_xdtw.cc \
    iommu/iommu_perf_model/iommu_perf_ptw.cc \
    iommu/iommu_perf_model/iommu_perf_pt_dedup_flush.cc \
    iommu/iommu_perf_model/iommu_perf_msipt_cache.cc \
    iommu/iommu_perf_model/iommu_perf_forwarder_fault_cq.cc \
    iommu/iommu_perf_model/iommu_perf_reorder.cc \
    iommu/iommu_perf_model/iommu_task_cache_convert.cc \
    iommu/cache_src/common/json_config.cpp \
    iommu/cache_src/common/stats_collector.cpp \
    iommu/cache_src/cache/cache_base.cpp \
    iommu/cache_src/cache/dc_cache.cpp \
    iommu/cache_src/cache/pc_cache.cpp \
    iommu/cache_src/cache/pt_cache.cpp \
    iommu/cache_src/cache/dedup_cache.cpp \
    iommu/cache_src/cache/walker_cache.cpp \
    iommu/cache_src/cache/msipt_cache.cpp \
    iommu/cache_src/replacement/plru_policy.cpp \
    iommu/cache_src/replacement/srrip_policy.cpp \
    iommu/cache_src/subsystem/cache_subsystem.cpp \
    rp/test_rp_func.cc \
    $(TEST_THREAD_SRC) \
    pcienoc/test_pcienoc.cc \
    slink/test_slink.cc \
    ddr/test_ddr.cc 

# CXX_SOURCES = \
#     iommu_top.cc  # Exclude due to missing header files

MAIN_SOURCE = main.cpp

# Object files - place them in the build directory
CXX_OBJECTS = $(patsubst %.cc,build/%.o,$(patsubst %.cpp,build/%.o,$(CXX_SOURCES)))
MAIN_OBJECT = build/$(MAIN_SOURCE:.cpp=.o)
ALL_OBJECTS = $(CXX_OBJECTS) $(MAIN_OBJECT)

# Target executable
TARGET = iommu_model

# Default target
all: $(TARGET)

# Link the executable
$(TARGET): $(ALL_OBJECTS)
	$(CXX) $(CXXFLAGS) -o $@ $^ -L$(SYSTEMC_LIB) $(LIBS) -Wl,--allow-multiple-definition

# Compile C++ sources into build directory
build/%.o: %.cc
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Compile C++ sources (.cpp) into build directory
build/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@



# Clean build artifacts
clean:
	rm -rf build/*.o $(TARGET) 2>/dev/null || true
	rm -rf build 2>/dev/null || true

# Rebuild
rebuild: clean all

# Phony targets
.PHONY: all clean rebuild test_dedup_unit test_dedup_integration

# ===================== Unit Test Target =====================
test_dedup_unit:
	@echo "=========================================="
	@echo "编译 PT Cache去重+预取 单元测试"
	@echo "=========================================="
	g++ -std=c++17 -Wall -Wextra -g \
		-o test_dedup_unit test_dedup_prefetch_unit.cpp
	@echo "✅ 单元测试编译成功"
	@echo ""
	@echo "=========================================="
	@echo "运行单元测试"
	@echo "=========================================="
	./test_dedup_unit

# ===================== Integration Test Target =====================
test_dedup_integration: iommu_model
	@echo "=========================================="
	@echo "运行 PT Cache去重+预取 集成测试"
	@echo "=========================================="
	chmod +x test_integration_dedup_prefetch.sh
	./test_integration_dedup_prefetch.sh

# Dependencies
iommu_command_queue.o: iommu_struct.hh iommu_registers.hh iommu_data_structures.hh
iommu_translate.o: iommu_struct.hh iommu_registers.hh iommu_data_structures.hh iommu_req_rsp.hh
iommu_top.o: iommu_top.hh iommu_struct.hh param_trans_def.hh