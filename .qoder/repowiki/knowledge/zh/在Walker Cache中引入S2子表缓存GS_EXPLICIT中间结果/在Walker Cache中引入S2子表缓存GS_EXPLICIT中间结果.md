---
kind: design
name: 在Walker Cache中引入S2子表缓存GS_EXPLICIT中间结果
source: session
category: adr
---

# 在Walker Cache中引入S2子表缓存GS_EXPLICIT中间结果

_来源：5d524ab → 1a95ce6 提交周期内记录的编码计划——内容为规划时意图，实现可能滞后或有出入。_

**状态：** accepted

## 背景
两阶段地址翻译中，VS-stage walk完成后获得GPA，进入GS_EXPLICIT阶段需要4次DDR read（Sv48x4: level 3->2->1->0）。为减少重复的G-stage页表遍历开销，需要在Walker Cache中增加对显式第二阶段(GS_EXPLICIT)中间walk结果的缓存能力。

## 决策驱动
- 减少GS_EXPLICIT阶段的DDR访问次数
- 复用现有Walker Cache三级结构(C1/C2/C3)的物理存储
- 保持向后兼容，不影响单阶段翻译场景

## 备选方案
- **独立S2子表方案** — 优点：与常规Walker Cache完全隔离，互不影响；通过is_s2标志位区分条目；以GPA为key而非IOVA；缺点：需要修改WalkerTag和walker_reserved_t结构；增加额外的统计计数器
- **共享Walker Cache条目** _（已否决）_ — 优点：无需新增数据结构；缺点：S2和V-stage条目混用会导致tag冲突；无法区分GPA和IOVA语义；实现复杂度高

## 决策
在现有WalkerCache类中新增独立的S2子表(ptw_s2_c1/c2/c3)，通过WalkerTag中的is_s2标志位区分S2和常规entry。S2子表以GPA为key（va_pa_flag=false），仅影响en_1S=1 && en_2S=1场景中的GS_EXPLICIT阶段。查询时机在init_gstage_walk之前，更新时机在walk_complete时按命中级别增量填充未命中层级。

## 影响
成功时可在顺序访问场景(seq128k_twostage)中获得高命中率，显著减少DDR reads；随机访问(rand4k_twostage)也有一定收益。代价是WalkerTag增加1bit、walker_reserved_t压缩reserved字段、WalkerCache增加3个子表和相应统计。默认关闭(PTW_WALKER_S2_CACHE_ENABLED=0)保证向后兼容。