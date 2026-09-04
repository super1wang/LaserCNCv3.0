# ST1C6b18 Task 错误 cause 版本化持久化

## 契约

`TaskSnapshot::error` 是 Task 失败证据的一部分，持久化不得只保存顶层 `Error` 而静默丢弃 `cause`。从本节点开始，新的 `lasercnc.task-terminal` 载荷写为 v2：每层 Error 保存 `code/category/severity/message/details/cause`，无下一层时 `cause` 为 null。

Error 链包含根错误在内最多 32 层。`recordTaskTerminal()` 必须在序列化、摘要计算和数据库事务之前遍历整条链：任意层未知 `ErrorCategory` 或 `Severity` 返回 `Persistence.InvalidTaskError`；指针环返回 `Persistence.TaskErrorCauseCycle`；第 33 层返回 `Persistence.TaskErrorCauseTooDeep`。拒绝后原 acceptance 保持 Pending，不得写入部分 terminal 材料。

读取 v2 时同样限制 32 层，并要求每个非空 Error 对象具有完整字段和显式 `cause`。摘要正确但 cause 类型错误、字段缺失或链超深的载荷仍失败关闭；`taskHistory()` 对终态解码失败沿用外层 `Persistence.TaskTerminalMismatch`，不得返回截断的错误链。

## 兼容与幂等

- 读取继续接受历史 `task-terminal` v1；v1 只恢复顶层 Error，不虚构 cause。
- 新写入只产生 v2，不后台改写既有 v1 行，也不修改 SQLite schema。
- 已终态 v1 在升级后被同一个、且不带 cause 的 `TaskSnapshot` 重放时，按精确 v1 载荷和摘要比较后保持幂等成功。
- 若重放材料带 cause 而旧 v1 记录没有，不能把缺失证据视为等价，仍返回 `Persistence.TaskOutcomeConflict`。
- v2 是向前写入格式；旧二进制不承诺读取 v2。回滚必须使用升级前完整数据库备份，不能只回退可执行文件。

## 边界

本节点不改变 71 个公共头、Error 枚举数字、Task API、Task acceptance v1 或数据库表结构。ErrorCode、message、details 及整条 cause 的字节/深度以外总预算仍归 C6c；终态记录数量与保留策略归 C6d/C7。

本节点只触及 Application Kernel 的 Task 持久化实现、内核测试和中文文档，不增加 CAD、CAM、Machine、Process、GUI、产品 CLI/RPC/AI 或其他上层模块。
