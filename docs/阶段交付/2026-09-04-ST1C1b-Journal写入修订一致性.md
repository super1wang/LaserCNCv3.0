# ST1C1b Journal 写入修订一致性

## 范围与状态

基于 C5b `b02eb9c`，继续内核收口 C1b：过期持久修订不能写入 Journal，拒绝不能发布应用对象、修订、History、事件或成功回执。首次 Journal 之前的 Catalog/Snapshot 归属也纳入检查。没有新增第三方库、上层模块或第二套持久修订真值。

最终专项、完整 Debug、重复与纯生产门禁均已通过，形成 C1b 本地检查点；不是整体冻结签核。完整机制与边界见 [C1b 契约](../内核契约/ST1C1b-Journal写入修订准入.md)，总体待办见 [ST1C 计划](../内核契约/ST1C-补充审计与剩余执行计划.md)。

## 改动与验证含义

- 新记录在同一写事务内认证项目/文档持久头并核对所有前置修订；首次扩展既有数据库前认证整个 Journal 修订链。连接只保留检查证明标记，不缓存修订值，依赖 C5 独占和不可变只追加契约。
- 所有作用域只能保持稳定或单次递增；对象变化必须同时推进 Project/Document。写入、读取 Journal 与恢复共享同一变化规则；既有版本号不变，不自动修复非法历史材料。
- 旧 TransactionId 内容一致的已验证重放返回原记录，不被新头误拒绝；同 ID 不同内容仍冲突。
- 无 Journal 的既有文档先认证 Catalog/Snapshot 归属，复用已有摘要/载荷验证；私有辅助方法不成为公共绕过入口。已有 Journal 的文档继续以 Journal 为修订依据。
- Kernel 负向回归在 Handler 中实际暂存一个 Domain Event，持有有效订阅，确认写入拒绝后交付数为零；重启并成功重试后交付数为一。不是没有事件源或已析构订阅的空断言。
- 原有 Create/Undo/Redo 故障矩阵增加项目头和文档头查询错误/异常，共新增 12 种场景；首次链、Catalog、Snapshot 读取的错误/异常与 rollback 失败组合另覆盖 12 种场景，隔离后仍持有 C5 所有权。
- 测试材料注入从合法模板编码后明确改写修订和重算摘要，再用原始 SQL 注入；没有为正常写入增加 skip/testMode。此破坏性夹具仅用于隔离测试数据库。

## 红灯与阶段证据

1. `build/st1c1b-red2-tests.log`：生产修改前 3 项全失败，退出 8，1.82 秒，证明过期修订、非法变化、已有损坏链均可被接受。最初测试误用并不存在的诊断接口导致的编译失败日志保留，不算产品缺陷证据。
2. `build/st1c1b-first-tests.log`：原 3 项红转绿，3/3，3.79 秒。
3. `build/st1c1b-first-broad-tests.log`：早期实现非长压力回归 291/291，120.97 秒；该结果不覆盖后续首笔归属补充，也不是最终全集。
4. `build/st1c1b-owner-red-tests.log`：初次头检查实现仍遗漏无 Journal 的既有文档，Catalog-only 与 Snapshot-only 两分支失败，退出 8，0/1，0.86 秒。随后复用完整读取校验修复，保留红灯数据库。

## 最终门禁

Windows x64、MSVC `19.44.35216.0`、CMake `3.29.3`，警告即错误；构建、测试、压力与基准不重叠。

| 门禁 | 结果 | 日志 |
| --- | --- | --- |
| 最终 Debug 构建 | 退出 0 | `build/st1c1b-final-build.log` |
| 最终专项 | 14/14，9.74 秒；新增 9 项、C5 Host 回归 4 项、扩展故障矩阵 1 项 | `build/st1c1b-final-focused-tests.log` |
| 完整 Debug | 308/308，退出 0，550.55 秒 | `build/st1c1b-final-full-tests.log` |
| 新增回归与故障矩阵重复 | 10 个唯一用例各 10 次，共 100 次通过，58.47 秒；逐行核对编号 161–169、190 各有 10 条 Passed | `build/st1c1b-final-repeat10-tests.log` |
| 纯生产 Release | 退出 0；31 个工程，测试/契约探针/Benchmark/ASan 工程 0，CTestTestfile 0，ASan 编译工程 0 | `build/st1c1b-production.log` |
| 架构边界 | 70 个公共头、136 个生产源检查通过，AppKernel 旁路与第三方实现隔离检查通过 | 独立运行 `VerifyKernelBoundaries.cmake`，且完整 Debug 含对应门禁 |
| 文档 | 本次 6 份中文 Markdown 的 30 个本地链接均可解析；`git diff --check` 通过 | 本地核验输出 |

复跑命令（在已配置的 VS 2022 x64 环境中顺序执行，不与其他重负载并行）：

```powershell
cmake --build --preset vs2022-debug --parallel 1 -- /nodeReuse:false
ctest --preset vs2022-debug -j 4 --output-on-failure
ctest --preset vs2022-debug -R '^Journal write admission|Persistence stage failures' --repeat until-fail:10 -j 4 -V
cmake --build build/production-only --config Release --parallel 1 -- /nodeReuse:false
cmake -DLCNC_SOURCE_ROOT=J:/Code/LaserCNCv3.0 -P cmake/VerifyKernelBoundaries.cmake
```

上述纯生产 Release 不是完整 Release 测试配置。新增 9 项已纳入最终 308 项，扩展故障矩阵仍为原有唯一用例，不重复增加注册计数；重复次数不冒充唯一用例数。

已归档日志 SHA-256：

- 原始 3 项红灯：`4CBF9391AF6889A5EAA7513AA736F6A70E020BE37AEBFCA97C49C95A3D86270B`。
- 首笔归属红灯：`8F0B0697DA5C2074B871297677159B86422B252197B2900626F4AB9AC8FEE3B5`。
- 最终专项：`8F3DAB544303606F24CFDBBF5430B942EB81B6CCB6A194B16B7330EF474252E8`。
- 完整 Debug：`8E7334DC78BB77DF77B962777620951CF4541E6D5CE4BB195B19BB102683AF44`。
- 10 轮重复：`BB4A5A85D41CF91EB3C8CDEAA62B0E91E968F6DF71D77BC705CAA53D82D3CE55`。
- 纯生产 Release：`0A30630628612CF9394B0E6B23945C89119340B25E95864F95F2B4971092D634`。

## 后续与边界

C1b 不替代 C2 的全局准入、Project-only 活动和 drain/析构，也不代替 C3 存储键、C4 attach、C6 API/输入预算、C7 容量/性能和 C8 门禁治理。下一步仍依完整收口计划推进，不能宣布 Frozen。

首次链验证和首笔 Snapshot 认证增加首次写入成本；后续两个头查询及摘要认证也有成本。正式容量与性能基线由 C7/ST1D 验收，Benchmark 冒烟不证明无回退。当前证明的是可信单 Host 下的软件一致性，不提供任意原始 SQL 恶意篡改防护、GUI/设备安全或物理断电认证。

本检查点随本文件提交本地，提交标识以 Git 历史为准；本节点未推送远端，较大正确性与最终认证节点推送远端。所有失败日志和隔离证据数据库保留。
