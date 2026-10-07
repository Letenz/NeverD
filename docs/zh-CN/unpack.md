**语言**：[English](../unpack.md) | [简体中文](unpack.md) | [繁體中文](../zh-TW/unpack.md) | [日本語](../ja/unpack.md) | [한국어](../ko/unpack.md) | [Français](../fr/unpack.md) | [Deutsch](../de/unpack.md) | [Español](../es/unpack.md) | [Italiano](../it/unpack.md) | [Русский](../ru/unpack.md) | [العربية](../ar/unpack.md)

[← 文档索引](README.md)

# 加壳可执行文件的脱壳

`neverd unpack` 恢复加壳的可执行文件在自身地址空间中重建出的程序。它把输入作为有界的来宾进程运行，观察外壳把控制权交给它所生成代码的位置，并把该时刻的镜像写成同一容器格式的新文件。它不做去虚拟化：被保护器虚拟化的函数仍保持虚拟化。构建时需要 `NEVERD_ENABLE_CPU_EMULATION=ON`。

## 支持的输入

容器格式决定文件如何校验和重建，指令集决定如何判定一次转移，二者共同决定来宾进程配置。不在此表中的输入会在执行任何代码之前被按名称拒绝。

| 容器 (`format`) | 指令集 | 来宾配置 | 外壳知识 |
| --- | --- | --- | --- |
| PE32+ (`pe64`) | x86-64 | [`windows-pe64-v1`](process-emulation.md) | UPX |
| PE32+ (`pe64`) | ARM64 | [`windows-pe64-v1`](process-emulation.md) | 无；仅靠观察 |

## 用法

```bash
neverd unpack packed.exe -o unpacked.exe
neverd unpack packed.exe -o unpacked.exe \
  --options='{"backend":"unicorn","instruction_limit":400000000,"transfer":2}'
```

该命令输出一份 JSON 报告。退出码 0 表示镜像已写出；3 表示有界运行在接受入口之前结束（`outcome` 为 `no_entry`，不写任何文件）；1 表示输入、选项无效或准备失败。报告会给出实际运行的 `format`、`architecture` 和 `profile`。C 入口是 `neverd_unpack_json`；Python 提供 `Session.unpack`。选项为[进程选项](process-emulation.md)加上 `transfer`。外壳需要更多资源的地方默认值不同：100000000 条指令、600 秒、512 MiB，并且 `windows.defer_unmodeled` 默认开启。

## 入口如何确定

第零代是来宾加载器映射后的镜像。字节与该镜像不同的指令是进程自己生成的。一次转移是指首次执行比当前正在运行的代码更新的代码；`transfers` 列出每一次转移的 RVA、`generation`，以及栈指针是否等于进程入口时的值（`stack_balanced`）。

1. 在入口栈上发生的转移就是程序入口：外壳已经归还了它得到的栈。此时 `entry_source` 为 `transfer`。
2. 在更深的栈上发生的转移是外壳对程序发起的调用，例如 TLS 回调。它会被报告，但不会被接受。当已识别的外壳指明其最终跳转目标时，镜像在该调用处重建（此时程序的任何代码都尚未运行），所指明的地址即为入口。此时 `entry_source` 为 `stub`。
3. `transfer` 按位置显式选择列表中的某次转移，用于分阶段脱壳或以调用方式进入程序的保护器。

不会根据编译器启动代码的形态去预测入口。若运行先行停止，则报告 `no_entry` 以及进程的 `stop_reason`。

## 重建的镜像

各节保持原有 RVA，内容为观察到的内存；每个节的访问权限取其页面在转移时刻的权限。末尾的 `.neverd` 节保存一个新的导入目录，它覆盖程序原本就通过其调用的单元，因此代码和数据都不会移动。`imports` 列出每个单元及其 `origin`：`static` 单元由加载器根据输入自身的目录绑定，`runtime` 单元由外壳写入。镜像固定在观察到的基址上：未观察到针对生成内容的重定位，因此移除重定位目录并设置 `IMAGE_FILE_RELOCS_STRIPPED`。对于 UPX，会重新指向程序自身的 TLS 目录，因为加壳后的目录只能到达外壳的处理函数。

原始节表、导入目录布局和重定位表不会被重建；加壳器并不会在内存中恢复它们。

## 识别

`packer.kind` 只依据文件中的证据来命名保护器。UPX 需要 `upx_section_names`、`upx_pack_header`（魔数、格式、算法和校验和）与 `upx_entry_stub` 三者中的两项。未识别的输入仍可通过观察来脱壳。

## 限制

仅支持可执行文件；不运行 DLL。受检执行一次只放行一条指令，量级约为每秒 10^5 条，因此需要数十亿条指令的外壳会超出任何实际预算。执行按 4 KiB 页面跟踪：写入到正在运行同一代代码的页面中的代码，不会被报告为一次转移。在入口之前运行的程序代码（例如调用了未建模 API 的 TLS 回调）会使运行停止，除非外壳声明了自己的入口。当每个受保护的导入调用仍是六字节、并且以尾调用进入已解析的导出时，VMProtect 加载器可以被脱壳。这些调用点会改写成普通的导入调用。虚拟化的代码仍保持虚拟化。

## 验证

`NeverDUnpackTests` 检查识别。`NeverDUnpackExecutionTests` 在 Unicorn、KVM 和 WHP 上对已入库的 UPX 样本（NRV2B、NRV2D、NRV2E、LZMA 以及一个带 C 运行时的程序）脱壳，把每个节与原件比较，运行恢复出的镜像，并要求所有后端产出逐字节相同的结果。`UnpackGeneratedTests.cpp` 在测试内部为 x86-64 和 ARM64 给一个程序加壳，并对照链接产物检查三种加载器：直接离开的、先调用程序再离开的，以及两级加载器。`NeverDUnpackPublicTests` 覆盖 C ABI 与 CLI。`unittests/unpack/fixtures/Makefile` 用于重新生成 UPX 样本。
