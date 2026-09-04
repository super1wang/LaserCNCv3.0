# ST1C6b10 Diagnostics 并发与 latest 顺序交付

## 范围与结果

本检查点只修改 Application Kernel 的 Diagnostics 内存服务及单元测试，不增加公共 API。契约见 [C6b10](../内核契约/ST1C6b10-Diagnostics并发与latest顺序.md)。

- 每个注册项由独立串行包装器持有；执行租约覆盖 check、报告转换、本地 latest 写入和 exporter 快照复制。
- 不同 DiagnosticId 不共享执行锁，exporter 调用仍在锁外。
- 同线程递归同 ID 转换为 Unhealthy + `Diagnostics.CheckReentered`，不会自死锁。
- latest 明确为最后完成本地发布，而不是调用开始、API 返回或 exporter 完成顺序。

## 红灯与门禁

真实 Debug 红灯保留于 `build/st1c6b10-red-tests2.log` 和 `build/st1c6b10-red-details2.log`：同 ID 第二次提前进入、最大并发为 2、最终 latest 被较早调用覆盖为 `run.1`，三项断言失败。修复后的焦点测试含 27 项断言，覆盖同 ID 串行、不同 ID 并行、observedAt/latest 顺序和同 ID 递归拒绝。

最终注册清单为 471 项。沿用 Foundation/Kernel/Infrastructure/架构/Headless 精确选集并加入本节点一项：Debug 216/216、Release 216/216；ASan 216 个名字各连续三次，即 648 次执行全部通过，普通输出无 AddressSanitizer 报告，新增并发测试在明细中执行三次。三套配置均在最终测试前完整重建。

纯生产 Release 构建通过，共 31 个 vcxproj，不含 test/probe/benchmark/ASan/Catch 目标及 CTestTestfile；边界检查覆盖 71 个公共头与 141 个生产源文件。公共头未修改。build 证据仅留本机、不入库。

复现入口：

```powershell
cmake --build --preset vs2022-debug --parallel 1 -- /nodeReuse:false
ctest --preset vs2022-debug --tests-from-file J:/Code/LaserCNCv3.0/build/st1c6b10-selected-tests.txt
cmake --build --preset vs2022-release --parallel 1 -- /nodeReuse:false
ctest --preset vs2022-release --tests-from-file J:/Code/LaserCNCv3.0/build/st1c6b10-selected-tests.txt
cmake --build --preset vs2022-asan --parallel 1 -- /nodeReuse:false
ctest --preset vs2022-asan --repeat until-fail:3 --tests-from-file J:/Code/LaserCNCv3.0/build/st1c6b10-selected-tests.txt
cmake --build build/production-only --config Release --parallel 1 -- /nodeReuse:false
cmake -DLCNC_SOURCE_ROOT=J:/Code/LaserCNCv3.0 -P cmake/VerifyKernelBoundaries.cmake
```

本节点未运行普通 471 项全集、完整故障/真实进程/压力矩阵或新容量基线；216 项选集不得代签这些范围。

## 未完成范围

Metrics/Diagnostics exporter 失败记录的资源边界、检查数量/details 预算、其他公共 DTO/格式、C6c/d、C7/C8 与 ST1D 保持未完成。C6 大节点未闭合，本检查点不推远端。
