# ST1C6b18 Task 错误 cause 版本化持久化交付

## 范围与实现

本检查点只修改 Application Kernel 的 Task terminal 私有编解码、写前准入、既有测试和中文文档，不改公共 API 或 SQLite schema。契约见 [C6b18](../内核契约/ST1C6b18-Task错误cause版本化持久化.md)。

- 新 `lasercnc.task-terminal` 写入从 v1 升为 v2，递归保留 Error 的 category、code、details、message、severity 和 cause。
- Error 链含根最多 32 层；写前拒绝任意层未知枚举、指针环和第 33 层，拒绝不改变 Pending acceptance。
- v2 读取拒绝摘要有效但 cause 畸形或超深的材料；v1 历史载荷继续读取。
- 无 cause 的等价 v1 终态在升级后可继续幂等重放；带 cause 的新材料不会与缺少该证据的 v1 记录误判相同。

## 红灯与修复证据

首轮真实 Release 红灯保留于 `build/st1c6b18-red-tests.log`：2 个用例、27 条断言中 4 条目标断言失败，证明旧实现写 v1、丢失 cause，并接受嵌套非法枚举、环和 33 层链。兼容扩展红灯保留于 `build/st1c6b18-v1-idempotency-red.log`，证明旧 v1 记录在升级编码后会被字节比较误判为冲突。

修复后 4 个 C6b18 用例共 113 条断言，覆盖三层字段往返、32 层正边界、33 层拒绝、嵌套非法枚举、环、v1 读取与幂等重放，以及重算摘要后的畸形/超深 v2 读取拒绝。

## 最终门禁

- Release 全集：484/484，通过，1057.88 秒；包含 Task/Workflow、并发压力、崩溃注入、独立进程恢复、架构边界和 Headless 往返。
- Debug/Release 扩大选集：各 225/225；另各运行新增 4/4。
- ASan：225 项与新增 4 项各连续三轮，共 687 次执行通过，无 sanitizer 报告。
- Release Task persistence 相关：10/10、1019 条断言通过；C6b18 定向最终 4/4、113 条断言。
- 纯生产 Release 构建通过；边界门禁检查 71 个公共头、141 个生产源，并通过模块治理、执行网关旁路和第三方实现目录隔离检查。

本节点不是 C6 大节点，不推送远端。下一步继续 Host、State 和其他持久 DTO/格式，再进入 C6c 统一预算、C6d 同步/Task/终态保留。

## 可复现命令

```powershell
cmake --build --preset vs2022-debug --parallel 16
ctest --preset vs2022-debug --tests-from-file J:/Code/LaserCNCv3.0/build/st1c6b17-selected-tests.txt
ctest --preset vs2022-debug -R "Task terminal persistence"
cmake --build --preset vs2022-release --parallel 16
ctest --preset vs2022-release
cmake --build --preset vs2022-asan --parallel 16
ctest --preset vs2022-asan --repeat until-fail:3 --tests-from-file J:/Code/LaserCNCv3.0/build/st1c6b17-selected-tests.txt
ctest --preset vs2022-asan --repeat until-fail:3 -R "Task terminal persistence"
cmake --build build/production-only --config Release --parallel 1 -- /nodeReuse:false
cmake -DLCNC_SOURCE_ROOT=J:/Code/LaserCNCv3.0 -P cmake/VerifyKernelBoundaries.cmake
```

## 未完成范围

本节点不代签 Host/State 全部公开类型、Task Error 文本/Value 总预算、其他持久 wire、同步取消/deadline、有界终态保留、容量和存储环境支持。C6b/c/d、C7/C8、ST1D 与整体 Kernel Frozen 仍未完成。
