---
kind: design
name: Walker Cache 查询前置到 configure_and_route，PTW 仅消费任务携带结果
source: session
category: adr
---

# Walker Cache 查询前置到 configure_and_route，PTW 仅消费任务携带结果

_来源：6d4d639 → 69c282a 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
传统 PTW 在 walk 过程中自行发起 Walker Cache 查询，导致请求进入 PTW 后才开始查找，路径较长（dedup 路径 7+ 拍），且 VS-stage 的 walker_request/response FIFO 往返增加延迟。需要让所有输入请求在进入 PTW 之前就能完成 Walker Cache 查找，缩短关键路径。

## 决策驱动
- 减少 PTW 入口延迟
- 消除 VS-stage FIFO 往返
- 与 S2 Cache 查询物理隔离避免响应错配

## 备选方案
- **在 configure_and_route 中前置 Walker Cache 查询** — 优点：请求一到达就并行查找，PTW 可直接使用结果；S2 查询保留在 PTW 内走独立 FIFO，天然隔离；严格前置确保语义正确
- **保持 PTW 内部查询不变** _（已否决）_ — 优点：改动最小；缺点：无法缩短关键路径；VS-stage FIFO 往返仍存在；与后续多 RAM 重构耦合度低

## 决策
在 iommu_top::configure_and_route 写 pt_request_fifo 的同时注册 walker_front_pending 并写入 walker_front_request_fifo；新增 walker_front_response_thread 按 task_id 回填 walker_ctx 字段；PTW 侧通过 walker_result_ready 事件等待前置结果，命中后直接复用 apply_walker_front_result 解释逻辑。S2 Cache 查询（GS_EXPLICIT）保留在 PTW 内走原有 FIFO 通道。

## 影响
Walker Cache 命中率统计口径从「仅 PTW 任务」变为「全部输入请求」；需新增编译开关 TEST_CFG_WALKER_FRONT_ENABLED 保护旧路径以支持基线回归；PTW 输入侧增加 walker_front_ready_event 等待作为兜底（正常时序下 Walker 3~4 拍 << dedup 7+ 拍，等待为罕见情况）。