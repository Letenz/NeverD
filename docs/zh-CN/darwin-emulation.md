# macOS / iOS 来宾进程环境

`os/darwin/` 提供共享的 Mach-O 启动、Darwin 系统调用和内存模型；
`macos/`、`ios/` 分别定义平台契约。它们属于来宾 OS 层，HVF 属于宿主 CPU
后端，二者独立选择。

| Profile | 平台 | 架构 |
| --- | --- | --- |
| `macos-macho64-v1` | macOS | x64、ARM64 |
| `ios-macho64-v1` | iOS 设备 | ARM64 |
| `ios-simulator-macho64-v1` | iOS Simulator | x64、ARM64 |

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

本轮范围对齐现有 Linux/Windows 的有限进程模型：原始 Mach-O 字节、数据和 BSS、
段权限、`LC_MAIN` 的参数及返回、规范 `LC_UNIXTHREAD` 的启动栈、显式 argv/envp/apple
向量、退出、标准输出与错误输出、固定身份、匿名内存映射、保护和释放。
ARM64 使用 16 KiB OS 页，x64 使用 4 KiB；CPU 页表仍以 4 KiB 为基础。
`__PAGEZERO` 只保留地址，不消耗数 GiB 内存。
Mach-O 文件尾页保留同页原始字节，后续完整虚拟页清零。初始数据、栈和匿名页都按
OS 页独立持有物理内存，因此部分解除映射能释放预算，重新分配的页面保持清零。

系统调用遵循 Darwin ABI。ARM64 从 X16 读取服务号，x64 使用 BSD 类别前缀；
错误返回正 errno 并置 carry。JSON 中的 `error` 字段区分错误和值相同的成功结果。
不会复用 Linux 的负 errno。内存保护跨空洞或最大权限边界失败时，整段原权限保持不变。
部分 `write` 复制的字节会保留，后续访存错误仍返回 EFAULT。

设备与模拟器的 Mach-O 平台标记必须分别匹配。此版本接受不依赖动态库、重定位、
初始化函数或 TLS 的独立可执行文件。需要 dyld 链接、Mach IPC、线程、文件系统、
Objective-C/Swift 运行时、Foundation/UIKit，或 arm64e/PAC 的输入会明确拒绝；
不会用空实现假装执行成功。它不等同于完整 macOS/iOS 系统，也不是 Apple 的 Simulator。

测试使用自行编写、由 Clang/LLD 生成的五种 Mach-O 平台/架构用例，另有独立构造的
线程入口和畸形文件测试、4 KiB/16 KiB 内存测试，以及 C API/CLI 报告一致性测试。
Python SDK 也通过真实共享库执行五种平台/架构组合。
HVF 必需门禁也包含这些用例。完整支持边界、来源和命令见[英文说明](../darwin-emulation.md)。

新增的完整原生工作负载门禁要求本机架构的每个 Darwin 进程用例都实际通过：
ARM64 三个平台共 33 项，x64 的 macOS 和 Simulator 共 22 项。
`scripts/run_native_cpu_ci.py --require-darwin-backend hvf` 可以在本机验收；
Linux 使用 `kvm`，Windows 使用 `whp`，同时传入 `--build` 和 `--evidence` 路径。
缺少用例注册、跳过必需用例或缺少 `ld64.lld` 均不能通过。
[原生 Darwin 专项工作流](../../.github/workflows/darwin-native.yml)提供关闭 Unicorn
后的 x64 KVM/WHP 构建和验收；它需要 runner 实际提供虚拟化能力，保留完整测试证据。

2026-10-03 在 Apple M4 Max / macOS 15.6.1 的 Release 构建上验证：Darwin
114 项通过、0 失败，133 项因后端或架构不适用而跳过；共享会话和镜像映射 26 项通过；
Linux/Windows 进程及公开 API/CLI 回归 168 项通过。关闭 Unicorn 的原生 HVF 门禁
扩大到 20 个测试目标，811 项通过、0 失败，其中包含 57 项 Darwin 检查。
五种 Mach-O 组合的 Python SDK 集成、能力清单、SDK 审计、文档和格式检查也通过。
这些统计存在重叠，不能相加。Intel HVF、Linux KVM、Windows WHP 的本次硬件运行验证
仍需对应宿主；软件跨架构结果不替代这些验证。

最终桌面包已重新纳入本轮引擎，并通过 186 个 Mach-O 的依赖、签名和 Cocoa 启动检查。
包内引擎与关闭测试、关闭 Unicorn 的 CLI，在三种 ARM64 平台的 18 个场景中报告完全一致，
且全部实际使用 HVF；两种 x64 Mach-O 的显式 HVF 请求在 ARM64 宿主上明确拒绝。
硬件门禁现按宿主要求每种 Darwin 平台实际执行，ARM64 共 12 个必需用例均通过；
删除注册、跳过本机用例或无法识别宿主架构都会失败。相关脚本回归 96 项通过。

同时关闭 HVF 和 Unicorn 后，Darwin、HVF 和配置测试目标也构建成功：38 项通过、
0 失败，231 项后端用例按预期跳过；产物没有 Hypervisor.framework 链接依赖。
这项验证证明构建开关与诊断隔离，不算作来宾实际执行证据。
