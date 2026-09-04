# ST1C6a：文件路径精确性与 Unicode 日志

## 当前范围

基线 `c7b3d87`。承接 [C6 契约](../内核契约/ST1C6-公共契约与输入预算.md)，本次处理五个文件路径选项的真实截断缺陷和 Windows 日志 Unicode 路径乱码；建立 71 个公共头的起始差异清单，不宣称逐字段契约/统一输入预算/终态保留已完成。全量 Debug 412/412、定向 ASan 54 项各 3 次、纯生产与架构通过，形成本地检查点；仅修改内核基础设施、依赖配置、测试和中文文档。C6 全部子节点闭合后再推送远端大节点。

## 红灯与实现

第一轮新测试 0/3、0.61 秒、退出 8；真实材料在 `build/st1c6a-path-red-tests.log` 和对应 `stress-contract-runs/file-path-admission/`。五类选项在 NUL 结尾/中间两种形式下均被接受并创建截断后的文件或目录；合法 Unicode 数据库/快照/资产成功，但两类日志实际创建成乱码文件名，期望路径不可读。多日志 sink 的坏路径也能让另一输出先创建。

扩展既有文件/目录保护以及 Unicode 轮转后，最终四项测试在原生产实现得到 0/4、0.64 秒、退出 8，见 `build/st1c6a-path-matrix-red-tests.log`。不是缺头、工具链或夹具订阅错误造成的红灯；构建均已成功，负例保留实际前缀文件和乱码日志。

新增私有 `file_path_validation.hpp`，在 std::filesystem::path 的原生码元上查 NUL。SQLite 和 Snapshot 在编码/绝对化/文件创建之前拒绝；Asset 通过 Snapshot 层复用检查并保持 cause 包装；日志先检查两个路径再进入 Impl。不会把含 NUL 的路径截短后继续执行，也不引入新的公开测试后门。

原日志把 UTF-8 字符串交给 Windows 窄字符文件 API，实际文件名取决于系统 ANSI 代码页。本次使用锁定 spdlog 已提供的 SPDLOG_WCHAR_FILENAMES，传递 path.native()；CMake 同时向库和适配器传播该选项，避免 filename_t 配置不一致。记录内容仍是 UTF-8 文本/JSONL，文件名改为 Windows 原生宽字符，并未更改日志数据格式或公共 DTO。

## 回归范围

[file_path_admission_tests.cpp](../../tests/unit/infrastructure/file_path_admission_tests.cpp) 新增四个 CTest：新目标 NUL 拒绝（5 选项 × 2 形式）、既有目标保持（5 × 2）、两类日志 sink 混合预检（2），以及合法 Unicode 全适配器往返/日志轮转（1），合计 23 个内部场景。不是 23 个独立 CTest。

旧数据保留测试在唯一夹具目录写入已知内容；本轮不会删除原红灯文件或用户数据。Unicode 正例在期望原生路径实际查询数据库、读取快照、发布/验证资产、读取日志及 `.1` 轮转文件；不会仅凭创建接口返回成功就通过。

修复后四项新用例与两项原 Spdlog 回归共 6 项各 3 次，18/18、3.97 秒、退出 0，见 `build/st1c6a-path-green-tests.log`。统一门禁如下，整个 C6/Kernel 尚未 Frozen。

## 统一门禁与复现

| 门禁 | 本轮结果 | 材料 |
| --- | --- | --- |
| 全目标 Debug 构建与全集 | 构建退出 0；412/412、710.12 秒、退出 0 | `build/st1c6a-final-debug-build.log`、`build/st1c6a-full-debug-tests.log`、`build/st1c6a-debug-details.log` |
| 路径/日志定向重复 | 6 项各 3 次，18/18、3.97 秒、退出 0 | `build/st1c6a-path-green-tests.log` |
| 全目标 ASan 重构建 | 退出 0，依赖、适配器和进程探针一并重建 | `build/st1c6a-asan-build.log` |
| 相关 ASan 重复 | 54 项各 3 次，162/162、156.91 秒、退出 0；逐项次数与 JSON 名单完全一致，无 sanitizer 错误 | `build/st1c6a-asan-tests.log`、`build/st1c6a-asan-details.log`、`build/st1c6a-asan-manifest.json` |
| 纯生产 Release | 构建退出 0；31 工程、0 测试/探针/Benchmark/ASan 工程、0 CTest 文件和插桩，testing/ASan 均 OFF | `build/st1c6a-production-build.log` |
| 架构与真实 Host 编译 | 71 公共头/139 生产源通过；全集包含真实 Host 正例及四个恢复写入口编译拒绝，原有负向架构用例通过 | `build/st1c6a-boundary.log`、全量独立输出及下述探针 result.log |

日志完整性补查：`st1c6a-debug-details.log` 是 LastTest.log 的原样副本，含 260480 个 NUL、只余 190 条逐用例记录，不能当作全集完整明细；保留原文件与摘要，不清洗后冒充原始日志。全量结论依据无 NUL 的独立捕获 `st1c6a-full-debug-tests.log`（412 条 Passed、终结 412/412、原进程退出 0）。真实 Host 编译以 `build/vs2022/tests/architecture-probes/host-document-compile-7791e07bc2945273/result.log` 为独立证据：exit=0，healthy、attach、attachRecovered、adoptRecovered、restoreDocuments 五个实际标记齐全。ASan 详细日志无 NUL，162 条记录齐全。C8 须检查同构建树 CTest 日志覆盖及完整性；最终签核前应串行获取清单和测试，避免运行中另启 CTest（包括 show-only），并在每次运行后立即核验/归档。

Debug、纯生产 Release、ASan 三个构建树的 CMakeCache 均为 SPDLOG_WCHAR_FILENAMES=ON；逐份检查生成的 spdlog 与 Infrastructure 工程，普通两树的四个配置、ASan 树唯一配置全部携带同名定义。ASan Infrastructure 实际携带 /fsanitize=address，严格 preset 保持 alloc_dealloc_mismatch=1:abort_on_error=1，未开启拦截失败继续选项。

本机工具链仍为 MSVC 19.44.35216.0 / CMake 3.29.3 / Windows x64。重型阶段串行构建和运行；构建使用 --parallel 1 /nodeReuse:false，ASan 使用 -j 2。可按以下命令重跑选集；输出需另存新日志，不覆盖本轮红灯证据。

```powershell
cmake --build --preset vs2022-debug --parallel 1 -- /nodeReuse:false
ctest --preset vs2022-debug --output-on-failure
cmake --build build/production-only --config Release --parallel 1 -- /nodeReuse:false
cmake --build --preset vs2022-asan --parallel 1 -- /nodeReuse:false
$pathFilter = 'Filesystem|Snapshot storage|Snapshot publication|Asset |AssetStore|File adapters|File path rejection|Logging path|SpdlogLogService|SqlitePersistenceBackend|SQLite Host session|Offline storage|integration.kernel_crash_(snapshot|asset)|integration.close_snapshot_identity_restart'
ctest --preset vs2022-asan --show-only=json-v1 -R $pathFilter
ctest --preset vs2022-asan -j 2 --repeat until-fail:3 -R $pathFilter --output-on-failure
```

ASan 选集覆盖文件路径、快照/资产读写发布、SQLite 会话、离线恢复以及快照/资产崩溃、自动 close 快照跨进程身份等相关用例，不是整个 ASan 全集。本轮普通 Release 全集、ASan 全集和 ST1D 最终固定版本压力/基线尚未执行；不得用纯生产 Release 构建代签 Release 测试。

提交前核对本轮 9 份中文文档的 176 个本地链接、公共头清单全部 71 项摘要及本交付 13 项材料摘要，均一致；git diff --check 通过。生成的日志、测试数据库及失败现场仅作为本机证据保留，不提交为源代码。

## 兼容与剩余项

公共头和持久格式未变；原先依赖 NUL 截断的错误调用将返回结构化拒绝。Unicode 日志会使用正确文件名，不自动重命名或拼接旧乱码日志；不得混用此前窄字符配置的 spdlog 静态库/对象文件。当前支持 Windows x64，不据此宣称其他系统或任意 UTF-16/路径长度行为。

私有头只使用标准库；已加入目标源清单，但既有自动边界扫描仍未包含 src 私有 h/hpp，继续登记于 C8，不能把 71/139 绿灯扩大解释。C6b/c/d 仍须逐入口/字段/格式兼容、统一预算、同步/Task 及终态保留；本节点不替代这些要求。

## SHA-256

| 材料 | 摘要 |
| --- | --- |
| st1c6a-path-matrix-red-tests.log | `815580C07878C446653ECCE2ED39F205A31A19752767A036732AD95701724B9F` |
| st1c6a-path-green-tests.log | `69798B60602F0D7E1B2DF0F54EBCFD4C7891D5AE4FB616755A09FDAE298D1A20` |
| st1c6a-final-debug-build.log | `6199928FE40429253B1DDF33F70001B0947D81F050FCDC42FD8D009B8EB09D07` |
| st1c6a-full-debug-tests.log | `5418372A91594999D9E27F67E7F06A807122D069EE5D06272D251870082A2CA3` |
| st1c6a-debug-details.log（不完整原样副本） | `AC925B4B56E3FE8F994720317AF60AA19CBD10BC37CEB64D36CACD4B5D7924DE` |
| st1c6a-production-build.log | `7027B630FED2D61AC9F8B605E95354424174C30861BFA2C9C50CB9AFCC144CEB` |
| st1c6a-asan-build.log | `1A24CB872BBABF31D14D0B943FF2B1291373DD8A637349EF5EDFBE373BA8C4FB` |
| st1c6a-asan-tests.log | `855CE84C9E4F3E3FB3D0C5B06D002B3EF1587E01BE8BD3ABF472FDAD98C854C9` |
| st1c6a-asan-details.log | `3F45D6C6F238695D9FCA7457327F143AB08DE4E3EF3E3A4CAEAEB435F3E696BA` |
| st1c6a-asan-manifest.json | `056771D84398CF339670E320080EE6C31DEF4DE7BF9EB1887832AE1CDE006760` |
| st1c6a-boundary.log | `D692FE10A9F7A6341F04FA5A91534FB7FD95081E8B026AB925EDEA0CE1A7AA68` |
| file_path_admission_tests.cpp | `9A44A5D647B9EF4C6130CBC008C74603D05A390B7EA43D9AC4CCE93B107D50DF` |
| host-document-compile-7791e07bc2945273/result.log | `694F71EC6084D7EB91286055144EA809649C4DC65D4076957CA98CCAFBC43AD1` |
