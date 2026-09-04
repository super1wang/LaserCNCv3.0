# ST1C6b7 日志出口与等级准入

## 范围与状态

基线 `b60be7f`。只修复独立 LogObservabilityExporter 与 SpdlogLogService 的准入、时间计算和异常边界，不改公共头、持久格式、三方依赖或上层模块。六项真实负例在旧实现 0/6，修复后专项 12 项各十次通过；扩大验证见 [交付记录](../阶段交付/2026-09-05-ST1C6b7-日志出口与等级准入.md)。C6–C8/ST1D 仍未完成。

## 独立观察出口

1. exportObservation 自行检查 MetricKind 仅 Counter/Gauge/Histogram；未知值为 Validation/Observability.InvalidMetricKind。值须有限，Counter 还须非负，否则 InvalidMetricValue。合法 Gauge/Histogram 负值仍允许。不能依赖调用者一定经过 LocalMetricsService。
2. exportSpan 只接受 Succeeded/Failed/Cancelled/Stale，Running/未知值为 Validation/Observability.InvalidSpanStatus；空 name 为 InvalidSpanName；finishedAt < startedAt 为 InvalidSpanTimeRange。直接出口拒绝非法 DTO，不替调用方生成 Failed Span；LocalTraceService 的非法 end 归一规则属于另一入口。
3. 时间顺序检查后，在当前受支持的有符号整数 system_clock tick 域用无符号减法求非负差，再转换成毫秒 double。不得先做可能溢出的有符号 time_point 相减，也不得先把两个极端端点转 double 再相减而丢掉小间隔。全范围差值可有浮点舍入，但 max 附近一 tick、min 附近五 tick、跨纪元小区间和零间隔保留正确尺度。
4. 上述校验均先于 ILogService.write，不新增重试、flush 或业务状态副作用。exportSpan/exportObservation 的数据组装和后端标准/未知异常转换为 Internal/Observability.LogExportFailed，reason 记录异常原因；后端正常返回的 Error 原样返回，包含 category/severity/details/cause。
5. 新捕获边界使 LogObservabilityExporter 自己的后端抛异常从 Local 服务观察到的 Trace/Metrics.ExporterThrew 改为收到 Observability.LogExportFailed；其他 exporter 的异常处理不变。业务结果仍不因日志观察失败而反转。错误对象构造仍可分配，不承诺全局内存耗尽下 noexcept 或一定留诊断。

## 日志 Adapter

- write 只接纳六种具名 LogLevel（Trace/Debug/Info/Warning/Error/Critical）；6..255 在时间/JSON/后端调用前返回 Validation/Logging.InvalidLevel，不按 off/unknown 静默接纳或写入。
- 日志时间戳支持域明确排除 Unix 纪元前时间，返回 Validation/Logging.InvalidTimestamp；检查在 floor 转换前，避免最小 time_point 向下取秒时发生中间表示溢出。epoch 本身合法。Windows 既有 UTC 后端不能转换的其他日期仍为 Logging.WriteFailed，不把本项称为完整日历范围或任意后端日期认证。
- 多种输入同时非法时先拒绝等级，后拒绝纪元前时间。有效日志仍使用既有 sink/路径/轮转和 UTF-8 内容。负例比较已有 human/JSONL 完整字节不变；测试读取必须成功，不能以读不到文件得到的两个空串冒充无副作用。
- elapsed 数值合法不代表最终日志时间戳可被具体后端格式化；例如极端日期可由 RecordingLogService 接收，而真实 Spdlog 后端仍可能返回日期转换错误。拒绝无输出的保证只针对本节准入，不扩大为后端 I/O 失败时多 sink 事务回滚。

## 证据与剩余项

新增九项：未知 MetricKind 253 个值、三个合法 kind 的 NaN/正负无穷与负 Counter；非法 Span 状态 252 个值、空名称/反向时间；极端差值及一 tick；两个出口各标准/未知后端异常；未知等级 250 个值及 human/JSONL 无变更；纪元前两个边界。以上六项取得真实红灯，另三项补合法映射/小时间差、后端 Error 保留无重试、六种合法等级与 epoch JSONL 回读。内部循环不计为独立 CTest。

新增文件测试保留系统临时目录下带 admission 的独立日志；旧测试清理自己正常夹具的行为不变，失败材料不删除。现有路径/Unicode/轮转回归和真实 Headless JSONL 路径也纳入本轮选集。

本轮不签核文本/Value/labels 深度和字节总预算、Trace 活动/完成发布的资源失败、身份保留、Diagnostics latest 时间顺序或任意并发析构。下一步继续观察生命周期与资源失败、时间顺序，再补 Host/执行/状态/持久族 DTO/格式审计；C6c/d、C7 容量、C8 私有头/脚本/日志/CI、ST1D 最终三配置全部保留。
