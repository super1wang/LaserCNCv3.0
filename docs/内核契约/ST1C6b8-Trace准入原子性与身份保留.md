# ST1C6b8 Trace 准入原子性与身份保留

## 问题与契约

`LocalTraceService::startSpan` 原先先把记录写入活动表，再分配 completion 和 `ITraceSpan` 句柄。后续分配抛出时，调用方既拿不到句柄，也无法结束已经发布的活动记录；该 SpanId 随之被占用，重试返回冲突。这会把一次未成功的准入误记为长期活动事实。

现在先构造 completion 和未激活句柄，再在锁内核对活动表与有界完成窗口、插入活动记录并激活句柄，最后以无额外分配的移动操作交给 `Result`。未激活句柄析构不产生 `Trace.SpanAbandoned`。因此，在活动身份发布前的任一分配失败向调用方传播原异常，同时保持活动表、完成窗口和 exporter 不变，SpanId 可立即重试。

本节点没有将整个 `startSpan` 改为 `Result` 吸收所有资源异常，也不承诺 OOM 时构造诊断。活动表插入本身失败仍传播异常，但标准容器插入失败不改表。成功发布后，`end noexcept` 的记录、错误或 exporter 快照分配失败仍属于后续资源失败节点。

## 身份与寿命

SpanId 在活动期间及完成记录仍留在有界窗口期间不得复用；记录被 FIFO 淘汰后允许复用。这是窗口内防重，不是进程级永久墓碑。句柄通过共享 Core 保证拥有完成路径，即使 `LocalTraceService` 外层所有者先析构，句柄仍可结束且只通知一次 exporter。

## 验证边界

独立 `lasercnc_trace_allocation_probe` 只链接内核静态库，生产目标不包含分配替换。Release 与 ASan 分别穷举一次成功 `startSpan` 路径的每个分配点；每次注入都核对异常、活动数量、旧记录、exporter 次数、失败列表和同 ID 重试。MSVC Debug 替换分配器直接抛出会重入 Debug 堆锁，因此 Debug 仅做探针链接/启动 smoke，并由两项常规单元测试覆盖窗口复用与 Core 寿命，不能把 Debug smoke 写成分配故障签核。

本节点只覆盖 Trace 准入及已定义的身份窗口，不覆盖 Metrics/Diagnostics 资源耗尽、Trace 完成阶段分配失败、全局活动容量、文本/Value 预算或 Diagnostics latest 并发顺序。
