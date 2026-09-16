---
kind: design
name: 虚拟化场景 Lazy vs Strict 双模式测试框架
source: session
category: adr
---

# 虚拟化场景 Lazy vs Strict 双模式测试框架

_来源：1c79446 → 1a81109 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
需要对比规范 6.5.3/6.5.4 场景二下 Guest OS 经 vIOMMU 的 Lazy 模式（累积批量失效）与 VMM 直接下发的 Strict 模式（每次 unmap 立即失效）的性能差异，同时保持与现有场景10压力测试的独立性。

## 决策驱动
- A/B 对比需相同 unmap 序列
- 不破坏现有场景10基线数据
- 建模真实虚拟化栈时序
- 支持 FQ_DEPTH 与 VMM_TRAP_NS 敏感性分析

## 备选方案
- **新增场景11(Lazy)+场景12(Strict)，共享负载生成逻辑** — 优点：与场景10互补、可复用 test_rp.hh/unmap 接口、A/B 干净、基线回归不影响
- **改造现有场景10为Lazy/Strict切换** _（已否决）_ — 优点：代码量更少；缺点：废弃已有4档频率基线数据、混合随机负载不适合协议语义对比

## 决策
新建 rp/test_rp_virt_lazy_thread.cc 与 rp/test_rp_virt_strict_thread.cc 两个独立测试线程，共享 test_rp.hh 中的 unmap_vs_stage_pte/unmap_g_stage_pte 接口与 VirtCQ/VMM 拦截建模；Lazy 模式使用 GuestFlushQueue 累积 GVA，Drain 时 AV=0 下发 IOTINVAL.VMA；Strict 模式每次 unmap 立即 AV=1 带 ADDR 下发；四参数 VMM_TRAP_LATENCY_NS/VMM_TRANSLATE_LATENCY_NS/GUEST_POLL_LATENCY_NS/GUEST_FENCE_LATENCY_NS 作为建模假设显式标注。

## 影响
两场景输出 VirtInvalStats 统计结构便于 A/B 对比脚本分析；Makefile 目标复用场景7配置仅替换 TEST_THREAD_SRC；结论对 VMM_TRAP_NS 高度敏感，需在报告中明确标注建模假设值。