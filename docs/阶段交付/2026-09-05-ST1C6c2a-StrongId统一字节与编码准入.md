# ST1C6c2a StrongId 统一字节与编码准入交付

## 交付结论

Application Kernel 的全部 `StrongId<Tag>` 现统一限制为最多 4096 字节、严格合法 UTF-8、非空且不含既有 ASCII/C locale 空白与控制字符。35 类内核身份别名共享唯一构造门禁，超限或畸形输入不会进入注册、请求、日志、hash 或持久链。完整规则见 [C6c2a 契约](../内核契约/ST1C6c2a-StrongId统一字节与编码准入.md)。

本节点只修改 Foundation 公共模板、两个内核测试文件和中文文档，不增加第三方依赖，不扩展上层模块。C6c2a 不是 C6c 或 C6 汇总；下一步是 C6c2b 注册定义累计预算。

## 实现与兼容变化

- `kernelStrongIdMaximumBytes` 固定为 4096；4096 精确边界允许，4097 返回 `Foundation.StrongIdBudgetExceeded`，details 给出维度、实际值、上限和材料名称但不回显身份。
- 严格 UTF-8 检查拒绝非法 leading/continuation、过长编码、代理区和超过 U+10FFFF 的序列，返回 `Foundation.InvalidStrongIdEncoding`。
- 原 `Foundation.InvalidStrongId` 的空值、空白和控制字符语义保留。
- 合法 UTF-8 保持精确字节，不做 Unicode 规范化、大小写折叠或路径解释；身份仍不是权限令牌。
- SnapshotId 4097 字节现更早在构造层拒绝并保持目录为空；Store 对历史信封的 4096 防线继续保留，4096 字节真实往返仍通过。
- `StrongId` 仍只保存 `std::string`，对象布局和比较/hash 语义未变；`strong_id.hpp` 是本节点唯一有意公共头摘要变化。

## 红灯与回归处置

在生产修复前运行新增 Release 用例，旧实现对 4097 字节身份返回成功，拒绝断言失败。修复后 4096/4097 及四类畸形 UTF-8 边界转绿。

第一次全量 499 项中有 1 项测试失败：旧快照测试辅助函数把 4097 字节身份构造成功作为前置。其余 498 项通过，失败不来自生产副作用。测试改为断言新的更早准入层级、专用错误码和目录零副作用，未放宽 StrongId 或 Store 防线；最终全量重新运行并全部通过。

## 最终门禁

- Release 全目标构建通过；完整回归 499/499，耗时 1032.65 秒。
- Debug 全目标构建通过；身份/快照预算焦点 2/2。
- ASan 全目标构建通过；焦点 2 项连续三轮，共 6 次执行，无 sanitizer 报告。
- production-only Release 构建通过。
- 边界脚本通过 71 个公共头、141 个生产源、模块治理、ExecutionGateway 旁路和第三方实现目录隔离。
- 全仓 139 份 Markdown 的 636 个本地链接均可解析；2 个新增错误码均已进入中文契约；差异格式检查通过。
- 没有新增或升级第三方依赖；构建树、失败日志和测试临时目录不纳入提交。

## 可复现命令

```powershell
cmake --build --preset vs2022-release --parallel 16
ctest --preset vs2022-release --output-on-failure
cmake --build --preset vs2022-debug --parallel 16
ctest --test-dir build/vs2022 -C Debug -R "(StrongId rejects over-budget and malformed UTF-8 identities|Snapshot storage keys enforce identity and payload envelope budgets)" --output-on-failure
cmake --build --preset vs2022-asan --parallel 16
ctest --test-dir build/vs2022-asan -C RelWithDebInfo -R "(StrongId rejects over-budget and malformed UTF-8 identities|Snapshot storage keys enforce identity and payload envelope budgets)" --output-on-failure
cmake --build build/production-only --config Release --parallel 16
cmake -DLCNC_SOURCE_ROOT=J:/Code/LaserCNCv3.0 -P cmake/VerifyKernelBoundaries.cmake
```

## 提交与后续

本节点只做本地 Git 提交，不推送远端。后续严格按 C6c2b 注册描述/定义预算 → C6c2c 运行期请求/消息/观察预算 → C6c2d 持久与恢复 DTO 终检推进，然后才进入 C6d。C6 汇总才是候选远端大节点。

本交付不代签旧畸形身份自动迁移、Unicode 规范化、注册表总容量、同步 handler 抢占、终态保留、跨工具链 ABI、设备或物理断电；Kernel 1.0 仍未 Frozen。
