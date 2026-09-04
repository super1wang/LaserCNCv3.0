# ST1C6b6 观察状态与聚合准入

## 范围与状态

基线 `895ae67`。承接 [逐项审计](ST1C6b-公共契约逐项审计.md) 中 LocalTraceService、LocalMetricsService、DiagnosticsService 的四类负例；不扩展上层模块、不修改公共头/持久格式，不把本节点签为完整 Observability 或 C6 冻结。四项旧实现红灯 0/4，修复后的观察专项 13 项各十次通过；扩大验证与证据见 [交付记录](../阶段交付/2026-09-05-ST1C6b6-观察状态与聚合准入.md)。

## Trace 终态

- end 的合法终态仅为 Succeeded/Failed/Cancelled/Stale。Running 虽是合法记录枚举，但不能用于结束；所有未定义 uint8_t 值也不能成为完成记录。
- 保持 void noexcept end 及一次完成所有权：首次非法结束被归一为 Failed，并带 Validation/Trace.InvalidTerminalStatus、requestedStatus 数值与 spanId；原先传入的 Error 保留为 cause。不是静默忽略后留在 active，也不是声称业务操作失败；该失败描述的是观察契约使用错误。
- records 和 exporter 接收同一合法终态，后续合法/非法 end 与析构不再重复发布；合法终态的可选 Error 原样保留。两个线程竞争 end 时仍由同一 atomic 选出一次完成，不承诺哪个调用获胜。
- 仅证明通常资源条件下的状态准入；noexcept 对分配失败仍采用既有兜底，不等于记录或 exporter 一定落地。Span 分配失败的活动残留、活跃总量与淘汰后身份复用继续在 C6b/c/d 审计。

## Metrics 聚合

- 原有非有限输入拒绝、Counter 负增量拒绝、身份 kind 冲突及 series 容量保留。合法有限输入还必须使累计 sum 和 uint64_t count 可表示；否则返回 Validation/Metrics.AggregateOverflow。
- 在同一服务锁内对候选副本检查；只有通过算术检查并准备 exporter 快照后才插入/替换聚合。拒绝不修改 count/value/sum/minimum/maximum，不输出该 observation，旧序列仍可接受后续合法数据。
- Counter 的 value/sum 为累计值；Gauge/Histogram 的 value 是最后值，sum 是累计值。Gauge 的最后值虽可表示，也不能在 sum 已溢出时部分接受。正负极值抵消、零和负零保持原有语义；不静默饱和、重置或降精度来伪装有界。
- [私有生产算术](../../src/runtime/observability/metric_accumulation.hpp) 被真实服务调用，同时由测试直接构造 count=max-1/max 验证不回绕且失败无字段变化。这不是公开的状态注入 API，不声称实际运行了 2^64 次公共调用。双精度溢出则由三个公开记录入口实测。

## Diagnostics 准入和资源

- Check 返回的 Healthy/Degraded/Unhealthy/Unknown 均保留。未定义 status 转成当前 id 的 Unhealthy 报告，details 包含 errorCode=Diagnostics.InvalidStatus 和 reportedStatus；原非法报告不进入 latest 或 exporter。run 的 Result 成功仍表示取得诊断报告，不意味着健康。
- 现有 id 错配检查优先级保持。注册表插入保留传入 shared_ptr 参数所有权，临时 Entry 即使重复拒绝或插入抛异常，也不会在锁内销毁最后一个 Check；参数离开函数时锁已经释放。合法重复/冻结拒绝错误码和原注册项不变。
- 重入测试通过最后一个 Check 所有者的真实析构读取 latest/frozen；用同进程 10 秒看护超时标记并退出 86，避免死锁永久占据测试进程。用户析构仍须不抛异常，不从本条推出服务并发析构或无限递归调用安全。

## 覆盖、兼容和剩余工作

新增九项：非法 Trace 终态全域（Running 加 251 个未定义值，共 252）、三个 Metrics 入口的五种正负溢出场景、Diagnostic 未定义状态 252 个值、重复注册析构重入，以及合法 Trace/Error、计数算术边界、有限极值抵消、合法诊断/冻结拒绝、真实双线程 end 五项。内部循环不当作独立 CTest 数量；竞态测试允许任一合法赢家，不是所有交错穷举。

这是行为准入收紧：以前被记录/传播的非法状态和溢出聚合不再被当成正常数据；新增两个 Error code 及一个诊断 details.errorCode。公共头、接口形状、持久版本不变。

直接 LogObservabilityExporter 的 DTO/异常/极端时间差和 SpdlogLogService 的未知 LogLevel 仍为下一优先项；不能因 Local 服务已有校验就认为独立出口安全。时间顺序、身份淘汰、资源失败、输入总预算、终态保留与 C7 容量仍保留。新增私有头尚不在现有 71 公共头/141 生产源扫描中，C8 必须补私有头扫描；本轮只做实际编译、测试和人工依赖复核，不伪称自动边界已覆盖该头。
