# K10F-F5A：ASan 隔离配置与真实探针

## 状态

F5 进行中。已建立 Windows MSVC x64 RelWithDebInfo 隔离配置，三个真实探针及完整插桩构建通过；ASan 全集正在执行，尚未签发 ASan 全集或 F5 验收。未修改生产内核 API/实现，不增加上层模块。

## 构建与边界

```powershell
cmake --preset vs2022-asan
cmake --build --preset vs2022-asan --parallel 1 -- /nodeReuse:false
ctest --preset vs2022-asan -R integration.kernel_asan_probe --output-on-failure
ctest --preset vs2022-asan -j 4 --output-on-failure
```

新增 `LCNC_ENABLE_ASAN` 默认 OFF。开启时要求 `LCNC_BUILD_TESTING=ON`、Windows MSVC x64 和独立 RelWithDebInfo 配置；不在常规 Debug/Release 或 Production-only 目录追加插桩。`cmake/AddressSanitizer.cmake` 在依赖和内核目标创建前增加 `/fsanitize=address /Zi`，禁用增量链接，使用 ProgramDatabase 调试信息。SQLite、spdlog、Catch2 以及自有编译目标均在独立构建树重新编译，头文件依赖随使用它们的目标插桩。

实际工具链为 MSVC `19.44.35216.0`，编译器位于 `E:/vs2022IDE/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe`。配置仅把同目录的 `clang_rt.asan_dynamic-x86_64.dll` 复制到隔离测试程序目录 `build/vs2022-asan/asan-bin/RelWithDebInfo`，使构建期 Catch 发现与运行期 CTest 都能加载它；不修改系统 PATH 或系统目录，不纳入普通生产交付。已检查探针生成工程的 `EnableAsan=true`、`LinkIncremental=false` 和 `MultiThreadedDLL`。

此次本机配置使用 `FETCHCONTENT_SOURCE_DIR_*` 指向已下载且核对版本的原始依赖源码，避免重复下载；编译输出仍完全隔离，不复用原 Debug/Release 静态库。原始默认 preset 仍按集中声明获取依赖。Catch2 声明的 `95d8a61...` 是不可变附注 tag 对象，其 `^{commit}` 解析为 `8b08d4d...`，与本机 HEAD 一致，不是版本漂移。

ASan 与 `/RTC`、增量链接、Edit-and-Continue 不兼容，运行库定位及内存读数也有特殊边界，参见 [微软限制说明](https://learn.microsoft.com/en-us/cpp/sanitizers/asan-known-issues?view=msvc-170)。本配置只声明上述实际验证组合，不能外推其他编译器、架构或构建类型。

已审计 13 个实际编译工程（不包含 CMake 在选项处理前运行的 CompilerId 自检工程）：均为 `EnableAsan=true`，无增量链接。已构建测试程序的 PE 导入表包含 ASan DLL，复制后 DLL 与实际工具链原件 SHA-256 一致：`2BABC2A4298D128C8ABC33EFC7F3B70611246010059EF343D72F096D8AE24FF3`。已核对五个 Git 依赖工作区干净，SQLite 压缩包 SHA3-256 与集中声明一致，使用中的 sqlite3.c/h 与该压缩包内容一致。

实际根项目的 Production-only + ASan、混合 Debug/Release + ASan 两种配置均失败并返回预期错误。另将四种防护逻辑（关闭测试、混合配置、非 MSVC、非 x64）纳入 `architecture.asan_configuration_guards`，已在普通 Debug CTest 中通过（2.37 秒）；该测试通过无语言 CMake 夹具验证拒绝逻辑，不冒充实际非 MSVC/x86 编译器测试。

## 真实探针

`tests/integration/kernel_asan_probe.cpp` 编译时要求 ASan 宏，并使用 shadow-memory 接口确认合法位置可访问、堆分配末尾处于 poisoned 状态。通过 noinline 读取函数保留优化构建中的实际访问。

| 模式 | 预期行为 | 本次结果 |
| --- | --- | --- |
| healthy | 读取合法元素，输出成功标记，退出 0，无 ASan 错误 | 通过 |
| heap-buffer-overflow | 在数组末尾外读取一个 int，产生对应 ASan 错误 | 诊断匹配，退出 3 |
| heap-use-after-free | 释放数组后读取旧指针，产生对应 ASan 错误 | 诊断匹配，退出 3 |

CTest 驱动 `tests/cmake/verify_asan_probe.cmake` 为每次执行保留独立 stdout、stderr 和退出码；子进程 30 秒、测试 45 秒上限。负例必须同时满足非零数字退出码与准确诊断类型；普通错误、超时、DLL 缺失或仅开了编译选项都不算通过。探针诊断已显示 `readValue` 和源码位置。

驱动和 ASan test preset 显式设置 `alloc_dealloc_mismatch=1:abort_on_error=1`，清除允许忽略 interception failure 的继承选项；不通过继续运行或抑制错误让测试变绿。当前探针 3/3（0.53 秒），日志为 `build/k10f-f5-probe-tests.log`，详细证据保留在 `build/vs2022-asan/tests/asan-probes/`。

## 未完成门禁

普通 Debug/Release 构建及 Production-only Release 构建已经通过。完整 ASan 测试、最终代码 Debug/Release 全集、架构与配置负例汇总、连续重复及 F1/F2/F3 专项仍须完成。ASan 只提供地址错误检测，不是数据竞争、断电或零泄漏证明；插桩下的内存不能作为 F4 无插桩性能基线。
