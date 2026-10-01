**语言**: [English](../testing.md) | [简体中文](testing.md) | [繁體中文](../zh-TW/testing.md) | [日本語](../ja/testing.md) | [한국어](../ko/testing.md) | [Français](../fr/testing.md) | [Deutsch](../de/testing.md) | [Español](../es/testing.md) | [Italiano](../it/testing.md) | [Русский](../ru/testing.md) | [العربية](../ar/testing.md)

[← 文档索引](README.md)

# 测试 NeverD

NeverD 的测试回答三个不同问题：表示形状是否符合预期、完整 pipeline 路径能否
处理二进制 fixture，以及生成的代码是否保持行为。先选择能回答本次变更问题的
最小套件；对于高风险拉取请求，再运行更广的聚合测试。

## 配置测试构建

除非启用 `BUILD_TESTING`，否则测试不会构建。完整套件通常使用 Release；Debug
保留断言和单步能力，但有意不优化，不代表解码基准性能。

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

完整 fixture 集要求 `clang` 能进行跨目标编译，并要求 LLVM linker（`ld.lld`
与 `lld-link`）位于 `PATH`。CMake 无条件构建许多可重定位 fixture，并在存在
对应 linker 时构建已链接 ELF/PE fixture。因主机无法编译或链接 fixture 而跳过
的测试属于未执行覆盖，不代表该目标通过。

克隆、构建配置与 macOS 预编译 LLVM 说明见
[CONTRIBUTING.md](CONTRIBUTING.md)。

## 解释器恢复检查

```sh
cmake --build build-release --target NeverDInterpreterSpecializationTests \
  NeverDDevirtualizationSourceTests --parallel 4
ctest --test-dir build-release -L '^NeverD(InterpreterSpecialization|DevirtualizationSource)Tests$' \
  --output-on-failure
```

```sh
cmake --build build-release --target NeverDLowIRUndefinedIndependenceTests \
  NeverDOriginalBinaryUndefinedIndependenceTests \
  NeverDX86UndefinedEffectsTests NeverDX86CarryArithmeticFlagTests \
  NeverDX86LogicIdentityTests --parallel 4
build-release/bin/NeverDLowIRUndefinedIndependenceTests
build-release/bin/NeverDOriginalBinaryUndefinedIndependenceTests \
  --gtest_filter='OriginalBinaryUndefinedIndependence.*'
build-release/bin/NeverDX86UndefinedEffectsTests
build-release/bin/NeverDX86CarryArithmeticFlagTests
build-release/bin/NeverDX86LogicIdentityTests
```

恢复 API 测试覆盖 v1/v2/v3 默认值、显式预算、截断结构、各层 reserved 字段和未来尾部兼容。CLI 测试在两种 ABI、两个源码后端下检查字段／查询预算耗尽及成功恢复，拒绝非法十进制上限，并要求 `--devirtualize`。预算耗尽不得发布源码或部分残余图。

`NeverDLowIRRefinementTests` 覆盖实际恢复的残余图、不同结构的有限循环、零次迭代、独立动态生产者、条件见证、重叠输入视图、复制与溢出关联、两边不可变读取证据、强制系统标志和返回槽保留。错误候选、额外写入、不完整或无限路径、过期证据、临时区冲突及共享预算耗尽必须拒绝证书；已有独立性测试仍拒绝可观察的任意值。

同一目标中的 `LowIRLoopRefinement.*` 和 `BinaryLowIRLoopRefinement.*` 覆盖任意 64 位计数、嵌套字典序排名、真实原生残余代码、入口前缀模板、重叠视图及相关溢出。负例拒绝错误循环体、缩小入口域、不下降的排名、无符号回绕、遗忘之前的写入、遗漏切点、畸形模板和共享预算耗尽。成功的有限分支不能授权不完整的归纳证明。

`LowIRLoopInference.*` 和 `BinaryLowIRLoopInference.*` 使用独立编写的计数器、栈存储、提前返回、原生调用和打包标志位用例，覆盖窄位宽算术拓宽以及表达式不同但语义相等的标志状态。畸形图、缺失或伪造的来源、不终止／回绕循环，以及推导或证明预算耗尽均不得产生证书。

同一目标中的 `LowIRLoopPlanPairing.*` 检查寄存器重命名、不同算术体、双方独立前缀快照、谓词保留、共享帧输入、嵌套切点覆盖和独立证明预算。缺失关系、错误写入、无效临时值绑定、不完整配对或元数据预算耗尽均不得产生证书。

两层和三层循环的缓存相等退出测试覆盖操作数相关性、变化的边界、计数器重置和被破坏的复制。

比较缓存回归覆盖相等与不等、带守卫和常量折叠的初始化、扩宽后才出现的字段，以及字节、双字和四字缓存中的第 7/31/63 位。保留被检查位而仅改变相邻位，也必须被完整状态比较拒绝。零步长、移动边界、计数器重置和共享预算耗尽必须拒绝。

泛化前缀回归覆盖汇合入口、首个零次迭代见证、隐藏寄存器／栈帧差异、非规范布尔谓词、原生陷阱约束、相关联的栈溢出保存，以及错误或预算耗尽的计划。独立的两层／三层等值退出计数器及原生字节检查无符号输入边界、零值／最大值输入域、非单位步长和错误原始指令。推断及最终证明均必须拒绝不完整结果。

独立编写的分支循环回归覆盖两种分支方向、错误循环体、不终止的相邻分支，以及共享搜索／证明预算耗尽。 `LowIRLoopInference.AlternativeLoopsReachBothPrefixesWithinSharedBudgets`.

嵌套推导回归覆盖两层及三层循环、递增及递减计数器、自动阶段常量和真实原生循环体切点。不可达或互斥的前缀域、错误循环体、不终止或回绕转换，以及共享搜索／证明预算耗尽都必须拒绝。前缀证据不能替代完整段覆盖。

```sh
cmake --build build-release --target NeverDLowIRRefinementTests --parallel 4
build-release/bin/NeverDLowIRRefinementTests
```

`NeverDLowIRUndefinedIndependenceTests` 检查完整无环 LowIR 图的两次执行独立性。两侧共享普通入口输入；每次新产生的架构未定义值在复制、重叠写入、溢出保存和重载中保持来源关联。控制谓词先于路径假设接受检查。证书要求 `Complete` 效果元数据，并精确绑定每条指令的完整边界和操作摘要。缺少证据、可达循环、调用、未知别名或预算耗尽都会拒绝证书。结论受显式观察项和无故障栈帧契约限制，不是原生代码到 C 的完整等价证明。

`NeverDOriginalBinaryUndefinedIndependenceTests` 使用独立编写、固定映射的 x64 字节，验证物理原生 CALL/RET、改写的返回目标、有限间接目标全集和不可变加载。同一测试目标还检查直接分支完整收集、精确字节／效果／映射／读取见证绑定、外层返回时入口 RSP 及返回地址槽保持，以及栈帧与映像分离前提的可满足性。缺失或重叠指令、不符合精确陷阱和显式环境投影规则的未审计分支、不终止或超预算的循环、不完整目标枚举、执行配置／契约不符和预算耗尽均须拒绝，且不产生证书或残余代码。成功要求每条可行原生路径完整结束。此可选门禁不认证循环不变量、异常分派、启用 CET 的执行或原生代码到 C 的等价性；普通恢复仍独立可用。该目标还检查严格提升的 `INT3`/`UD2` 终止边界及其完整字节、操作摘要绑定。未定义输出附属元数据的 `Missing` 必须保持不变；仅经符号执行证明不可达的陷阱可进入证书，任意可行陷阱路径都须返回 `ContractViolation`，且无证书、无残余代码。不建模陷阱后的顺序执行或异常恢复，不使用 `codeFollowsTrap`，静态 LowIR API 的支持范围保持不变。

打包标志测试覆盖全部标量入口标志组合、特权掩码、两次执行的 TF/AC 条件、不同未定义产生点、相关副本、原生调用、兄弟路径状态、强制最终系统状态观察、畸形证据及资源计费。有限循环必须结束每条可行输入路径；安全分支不能掩盖无限或截断路径。RDSSPD/RDSSPQ 检查覆盖 16 个通用寄存器和两种宽度、高位保持、保留 `Missing` 证据及伪造投影拒绝。机器状态测试在两个 C 后端的 O0/O2 下开启未定义行为陷阱，与独立用户态标志位预言机比较，并检查环境失败状态不会被后续操作清除。 INCSSPD/INCSSPQ 测试覆盖两种宽度和全部通用寄存器、不可达边界保留、安全兄弟路径完成后的可行陷阱、零操作数及伪造陷阱证据。

`NeverDX86UndefinedEffectsTests` 检查未定义位元数据、已定义／保留标志及过期证书拒绝。`NeverDX86CarryArithmeticFlagTests` 用算术参考实现检查寄存器和内存形式 ADC/SBB 的辅助进位。`NeverDX86LogicIdentityTests` 检查相同操作数的 AND 在 64 位模式下写入 32 位目标时，仍清零其所属 64 位寄存器的位 63:32，同时保留窄位宽写入未覆盖的位。

移位回归覆盖全部八位原始次数、零次移位的标志组合、两种 x86 模式、全部标量位宽、CL 与目标重叠、AH/CH/DH/BH、扩展寄存器和内存。按字节建模的符号执行与逐位算术模型对照，检查已定义结果及守卫触发条件。关系测试检查复制与新生标志、溢出保存、循环重访、未定义值派生次数、分支拒绝、畸形编码及摘要和预算失败。有限不可变读取测试覆盖 1/2/4/8 字节、输入相关选择、路径内单地址集合、完整读取见证和地址上限绑定，并拒绝依赖未定义值、缺失、可写、无文件字节、重定位或无界候选。

核心测试检查上下文拆分、固定点汇合、动态循环、重叠寄存器、别名失效、有限目标派发，以及拒绝时不提供部分替代代码。源码测试汇编原创的寄存器式、栈式和有限地址 x64 机器，恢复两条 C 输出路径，在 O0/O2 下开启未定义行为陷阱编译，并与独立的无符号算术和内存参考实现对照执行。有限地址 fixture 覆盖输入选择的记录和相关游标／key 控制字段；原生检查涵盖 SysV 和 Win64 调用约定。测试还覆盖公开 CLI、恢复预算及不支持输入的报告。需要支持跨目标编译的 Clang 和 LLD；原始 ELF 的执行另需 x64 Linux 主机。工具缺失或主机不匹配属于跳过的覆盖，不代表通过。

`ControlStateRecovery.LongTransparentLoop*` 覆盖独立编写的 20 阶段循环、动态算术参考实现、未知 selector 拒绝和预算耗尽。`LongTransparentPhasesKeepExactBitDemands` 检查 selector 同字节内的无关位仍是可观察的运行时数据，不会成为控制需求。`ProducerClosureChargesWorkBeforeAnotherRestart` 检查反向发现和重放在新图启动前消耗共享预算，且不发布部分结果。

`X86ShiftCarry.*` 用连续单比特移位校验窄位宽算术右移的进位、掩码后的计数，以及 APX 目标寄存器和标志抑制行为。
`NarrowArithmeticShiftCarrySurvivesBothSourceBackends` 在 O0/O2 下启用未定义行为陷阱，执行两条恢复 C 路径，覆盖全部字节值和原始计数。
`NeverDLLVMCIntrinsicSemanticTests` 还在 O0/O2 下执行 i1/8/16/32/64/128 的有符号和无符号整数 min/max，检查赋值和内联结果、操作数生成顺序及单次求值。不支持的标量位宽和畸形操作数必须明确失败。

## 结构化 C 控制流与调用检查

`HighControlFlowSemantics.*` 检查移动循环出口或尾部时是否保留其他跳转仍引用的标签。测试覆盖直接进入循环头部、尾部出口及替换后的 break，并以独立返回值预期执行 O0/O2 编译的生成 C。

`HighCPointerAddresses.Required*` / `UnknownConditionsFailOnlyWhenRead` 检查推断出的必需寄存器参数是否保留末尾未知槽位。读取未知的必需参数或条件必须明确触发陷阱；省略、空指针和嵌套操作数不能被悄悄替换为零。已知值和已证明不被读取的多余操作数仍可执行。陷阱是诊断边界，不是恢复行为等价的证明。

## CPU 执行测试

`NeverDIntegerABITests` 为 Windows x64、Linux x64 和 Linux ARM64 构建原始 Clang fixture，通过真实的十参数函数检查寄存器／栈参数与调用帧。Unicorn/KVM/WHP 矩阵会明确跳过不可用的主机／ISA 组合；跳过不代表通过。`NeverDExecutionBudgetTests` 不依赖定时 sleep，检查共享续接预算、预留失败和绝对 deadline。

`NeverDCPUEmulationTests` 覆盖 ARM64 指令、控制流、加载、CPU 上下文、别名、缓存失效和有界循环；软件配置还执行 FP/SIMD 与 TLS。`NeverDUserExecutionTests` 检查 CPL3/EL0 页权限、别名、保护故障、上下文和地址空间切换。`NeverDServiceRequestTests` 验证 SYSCALL/SVC 在进入传输前被拦截、保留状态并恰好消费一次请求；这是交接协议，不代表完整 OS 服务实现。`NeverDExecutionConfigurationTests` 验证工厂与报告共用配置解析、区分构建支持与实时探测，并在修改前拒绝不支持的要求。公开 SDK/CLI 测试不需要 Windows 模型。`NeverDThreadPointerTests` 检查 FS 基址、`TPIDR_EL0`、上下文恢复和权限。`NeverDKvmCancellationTests` 使用不会退出的 x64 来宾验证活动 KVM 中断、恢复及调用方信号状态不变；没有 KVM 时明确跳过。

```bash
cmake --build build-cpu --target NeverDKvmRunTests NeverDKvmCancellationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDKvm(Run|Cancellation)Tests$' --output-on-failure
```

```bash
cmake --build build-cpu --target NeverDIntegerABITests NeverDExecutionBudgetTests NeverDCPUEmulationTests NeverDUserExecutionTests NeverDServiceRequestTests NeverDExecutionConfigurationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(IntegerABI|ExecutionBudget|CPUEmulation|UserExecution|ServiceRequest|ExecutionConfiguration)Tests$' --output-on-failure
```

缺少 ARM64 硬件或 hypervisor 属于原生覆盖被跳过，不是通过。Unicorn 和交叉编译不能证明原生 KVM/WHP 执行。

## Linux 进程配置测试

独立的[进程测试套件](process-emulation.md#验证)编译真实 x64/AArch64 ELF fixture。`NeverDLinuxProcessTests` 检查启动、program-header 策略、服务续接、二进制输出、来宾故障和资源停止。`NeverDProcessPublicTests` 通过 C API/CLI 验证且不修改分析映像。`NeverDExecutionSessionTests` 检查两个 CPU 共享内存／预算以及请求／故障恰好消费一次。`NeverDX64MemoryUpdateTests` 检查内存算术、SETcc、BT、XMM/MXCSR、写入观察器、REP 边界和预备设备读取。`DriverBackendParityTests.cpp` 运行原始与重定位 WDK fixture，并将完整可观察报告与 Unicorn 比较；缺失镜像／后端会明确跳过。

checked x64 还支持带屏蔽的传统 `ADD`、`SUB`、`MUL`、`DIV`、`SQRT`、`MIN` 和 `MAX` 的 `SS`、`SD`、`PS`、`PD` 形式。`X64SSEInstructions.def` 统一定义操作数宽度、对齐和准入规则。`MaskedSSEArithmeticMatchesIndependentHostExecution` 使用独立的本机 CPU 参照验证寄存器与 RAM 形式，覆盖四种舍入模式、FTZ、有符号零、次正规输入和 NaN；`SSEMemoryObserverStopsBeforeResultAndStatusChanges` 验证停止请求发生在效果提交之前。这不开放 DAZ、未屏蔽异常、x87 或 AVX。

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
```

不可用后端会明确跳过。交叉编译与 Unicorn ARM64 不构成原生 KVM/WHP 证据。

## 驱动模拟检查

同时启用 `NEVERD_ENABLE_DRIVER_EMULATION=ON` 与 `BUILD_TESTING=ON`，即可构建专项执行套件及共享 C API／CLI 检查：

```bash
cmake --build build-release --target \
  NeverDDriverEmulationTests NeverDDriverEmulationPublicTests --parallel 4
ctest --test-dir build-release -L '^NeverDDriverEmulation' --output-on-failure
```

fixture 覆盖来宾初始化、成功与失败返回、不支持的行为、内存故障、严格场景解析、有界执行，以及经过 create、传输、cleanup、close 和 unload 的同步 buffered／direct I/O、READ/WRITE、独立文件生命周期、MDL 权限、动态导出解析、来宾变参和结构化 CPU 故障。使用 [`emulate-driver` CLI](driver-emulation.md) 验证 JSON 与进程退出码。生产构建可在 `BUILD_TESTING=OFF` 时启用此功能；`libneverd` 不得依赖仅供测试使用的 Unicorn 配置。

补充测试覆盖驱动独立拥有的非分页池 MDL、描述符与缓冲区的独立生命周期、注册表查询布局与短缓冲区、句柄权限、删除和泄漏，以及无输出 IOCTL 的完整 64 位 `information_hex`。真实样本验收还包括 Zero 的同步直接读写和统计查询。

后端测试验证完整 CPU 上下文（寄存器、标志、SIMD、FPU、CR8）、共享内存及跨后端／故障上下文拒绝。编译后的 `driver_dispatcher.c` 样例实际执行 DPC 与工作项回调，覆盖定时器边界、通知／同步事件与定时器、原因 `Executive` 的非警报 `KernelMode` 等待、超时／延迟、多个阻塞栈、置位后重置仍保留唤醒、回调参数及非法 IRQL／生命周期。工作项测试继续覆盖待处理／完成、队列、停滞和共享预算。这些用例证明所述子集，不代表完整 Windows 异步支持。

`driver_context_limits.c`: API 的 IRQL 上限来自 `KernelAPIIRQL.def`，参数相关限制由所属模型检查。DPC 不能调用注册表 API，也不能分配、释放或访问分页池；Unicode `DbgPrint` 转换要求 `PASSIVE_LEVEL`，支持的 ANSI 输出和非分页操作仍可在 `DISPATCH_LEVEL` 使用。回调栈有明确边界，越界栈指针不能进入另一阻塞工作项的栈。设备扩展中的已启动定时器会阻止设备提前回收。这些检查并未开放通用 IRQL 切换。

`KernelDeviceStackTests.cpp` 检查独立的所有者／附着关系、栈顶选择、失败原子性、栈容量、不透明字段、打开句柄计数、拆链／删除时的工作项及请求保活，以及文件身份与派发栈顶的区别。原创 `driver_wdm_stack.c` 使用真实 WDK 头文件和内联 Copy/Skip/SetCompletion；普通／启用 CFG 映像由可选 `NEVERD_WDM_STACK_FIXTURE`／`NEVERD_WDM_STACK_CFG_FIXTURE` 配置。`DriverWDMStackTests.cpp` 覆盖重定位、真实下层状态、完成顺序和标志、延迟 pending 传播、工作项／DPC、等待、`STATUS_MORE_PROCESSING_REQUIRED`、直接 MDL 保留、嵌套完成及畸形游标／控制值。`DriverScenarioPublicTests.cpp` 覆盖 C API／CLI 转发及 C API 保留／嵌套完成，包括已配置 CFG 映像。缺少产物会明确跳过；Linux 证据仅证明同驱动设备栈子集，不代表 PDO／PnP／电源支持。 `KernelIRPStackTests.cpp` 检查计数游标、完整内联 Copy 前缀、已消耗栈位置清零、状态／pending 传播、MPR 与嵌套完成、续接所有者检查及保留路径。真实 READ/WRITE 与文件生命周期也使用内联 Copy 验证。

`DriverPnpScenarioTests.cpp` 检查 JSON／原生预检一致、显式初始事实、ID／数量限制、字段互斥、最终总线状态及可空观测报告。`KernelPnpDeviceTests.cpp`、`KernelPnpRequestTests.cpp` 和 `KernelPnpCompletionTests.cpp` 检查提供者所有权、AddDevice 成功／失败／泄漏、初始 IRP、文件准入、生命周期回滚、延迟完成、MPR／嵌套／等待续接及失败原子性。原创真实 WDK 样例 `driver_wdm_pnp.c` 使用可选 `NEVERD_WDM_PNP_FIXTURE`／`NEVERD_WDM_PNP_CFG_FIXTURE`。`DriverWDMPnpTests.cpp` 验证普通／启用 CFG 且重定位的 AddDevice、文件 I/O、有序移除、延迟启动／移除、启动／query 失败及干净／泄漏的 AddDevice 失败。缺少产物会明确跳过。执行证据仅来自 Linux，只证明所述无资源 PnP 子集。 `DriverScenarioPublicTests.cpp` 还通过 C API 和 CLI 验证普通／启用 CFG 映像的七请求延迟 PnP 报告。

V9 schema 测试往返验证八种次功能名称，并与生命周期完成共享最终状态校验；QueryStop 0x119 在加载映像前拒绝。扩展模型及真实样例检查 query-stop 回滚、cancel-stop、停止／重启、突然移除、精确成功失败边界、停止／待移除状态的软件 I/O、突然移除后的来宾拒绝、设备身份及混合 AddDevice 结果。`DriverScenarioPublicTests.cpp` 通过 C API 和 CLI，在普通／启用 CFG 样例上运行 16 请求的停止／重启／突然移除序列，保留成功软件 IOCTL 字节、来宾拒绝的 IOCTL 和最终 cleanup/close/remove。公开执行为串行：保留 IRP 若无当前可用生产者，无法等待后续场景请求来启动或清理设备。Remove 排空要求是配置边界，不是通用 Windows I/O 准入策略。证据仍仅来自 Linux。

`KernelRemoveLocksTests.cpp` 检查独立锁／设备身份、NULL／重复 Tag、retail／DBG 大小、立即／延迟排空、失败获取义务、失败原子性、容量及退休。`KernelRemoveLockBridgeTests.cpp` 检查附加前初始化、扩展范围、不透明存储、IRQL 和修改前拒绝不安全 Delete／Detach。真实 `driver_wdm_remove_lock.c` 的 retail／DBG、普通／active-CFG 四种样例通过 `NEVERD_WDM_REMOVE_LOCK_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE`、`NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE` 指定。`DriverWDMRemoveLockTests.cpp` 覆盖包退休后释放、锁排空后总线才完成、最后 release 先于回调返回唤醒、工作项等待及干净 AddDevice 失败。C API／CLI 使用既有 PnP 场景，保留总线接收／完成与最终拆除观测。缺少产物明确跳过；Linux 证据不代表完整 Driver Verifier 或通用并发排空。

`DriverPowerScenarioTests.cpp` 验证必填电源事实、JSON／原生一致性、不透明32位 context、响应 FIFO 限制和独立子报告。`KernelPowerRequestTests.cpp` 与 `KernelPowerCompletionTests.cpp` 检查包布局、路径标志、生命周期与设备通知的区别、FIFO 匹配、最终回调所有权、MPR、等待及释放边界。真实 WDK 原始样例 `driver_wdm_power.c` 使用可选 `NEVERD_WDM_POWER_FIXTURE`／`NEVERD_WDM_POWER_CFG_FIXTURE`；`DriverWDMPowerTests.cpp` 覆盖普通／active-CFG 重定位、直接及嵌套 Query/Set、独立延迟完成、S0 先于 D0、跨等待五参数回调快照、工作项来源子请求、空回调、query 拒绝、独立 PDO 初值／FIFO 和缺失事实错误。`DriverScenarioPublicTests.cpp` 增加格式错误预检及 C API／CLI 的六场景请求／三子请求睡眠唤醒序列。缺少真实产物明确跳过；执行证据仅来自 Linux，只证明文档中的可分页无资源电源子集。

`KernelUsbIdleTests.cpp` 验证协议所有权、精确回调／D2 身份、借用及首个完成原因；`KernelUsbIdleBridgeTests.cpp`／`KernelUsbIdleReceiptTests.cpp` 覆盖真实 IRP、非法准入、排队取消、组合容量及嵌套收到／完成时序，`DriverUsbIdleScenarioTests.cpp` 检查原生／JSON 一致和报告证据。真实 `driver_wdm_usb_idle.c` 使用 `NEVERD_WDM_USB_IDLE_FIXTURE`／`NEVERD_WDM_USB_IDLE_CFG_FIXTURE`；`DriverWdmUsbIdleTests.cpp` 覆盖 idle、取消、D0／D3、真实唤醒、重发重启、独立／组合成员及直达 PDO／FDO 转发和分配方栈。`DriverWdmUsbIdlePublicTests.cpp` 通过 C API／CLI、普通／active-CFG、首选／重定位执行 [USB 场景](../examples/driver-wdm-usb-idle-scenario.json)。缺少产物明确跳过，证据仍仅限 Linux，不代表 KMDF USB 选择性挂起已实现。

`KernelFrameworkUsbIdleTests.cpp`、`KernelFrameworkUsbIdleStorageTests.cpp`、`KernelFrameworkUsbIdleBridgeTests.cpp` 验证策略、真实存储与类型化调度。真实 `driver_kmdf_usb_idle.c` 不自行发出 USB idle 包。`DriverKMDFUsbIdleTests.cpp` 覆盖无许可、受管 I/O 的延迟 D2／D0、回调前／中 StopIdle、arm 失败、显式 Maximum 能力、远程唤醒及组合成员。`DriverKMDFUsbIdlePublicTests.cpp` 通过 C API／CLI、普通／active-CFG、首选／重定位执行 [KMDF USB 场景](../examples/driver-kmdf-usb-idle-scenario.json)，使用 `NEVERD_KMDF_USB_IDLE_FIXTURE`／`NEVERD_KMDF_USB_IDLE_CFG_FIXTURE`。缺少产物明确跳过，执行证据仅限 Linux。 模型测试还验证：唤醒 arm 回调成功后分配耗尽，仍执行真实 disarm 回调、取消 WAIT_WAKE，且不消费 D2 响应。

`DriverKMDFUsbPoFxTests.cpp` 覆盖首次 SystemManaged／WithHint 配置、独立 PoFx／USB 许可、D0 取消、延迟 D2／D0 与真实 worker 延迟 F0 确认、活动及 StopIdle、READ 前唤醒恢复、arm 失败、移除与重启。`DriverKMDFUsbPoFxPublicTests.cpp` 通过 C API／CLI、两种服务模式、普通／active-CFG、首选／重定位运行 [USB PoFx 场景](../examples/driver-kmdf-usb-pofx-scenario.json)。`DriverKMDFUsbIdleTests.cpp` 及公共测试保留原转发回归，新增直接 READ，验证不进入路由回调且 D0Entry 后投递。`KernelFrameworkRequestTests.cpp` 独立覆盖映射、caller-context 所有权、手动／停止队列与 IRQL。分配失败及独立门控由模型 USB／PoFx 桥测试证明，真实样本不耗尽 arena。外部样本缺失明确跳过，执行证据仍仅限 Linux。 `KernelFrameworkUsbPoFxBridge.RemovalPowerUpFailureAcknowledgesRequiredWithoutReleasingIdleWait` 另覆盖 RemovePending 期间真实 D0Entry 失败：精确 Required 确认与 quiesce 允许失败 IRP／硬件清理，不触发 F0／ActiveCondition；不证明普通在场设备 SET_POWER 失败或框架自主意外移除。

`KernelPowerCompletionTests.cpp`, `KernelProviderWaitWakeTests.cpp`, `KernelWdmWakeEventTests.cpp`, `DriverWdmWaitWakeTests.cpp`: [原生 WAIT_WAKE 场景](../examples/driver-wdm-wait-wake-scenario.json) 使用真实 WDK `driver_wdm_wait_wake.c`，通过 `NEVERD_WDM_WAIT_WAKE_FIXTURE`／`NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE` 运行。测试覆盖实际 START 中提交、回调参数、唤醒不自动 D0、重发、取消、MPR、DPC 取消后工作线程提交 D0、精确事件捕获和独立提供方。普通／active-CFG 及首选／重定位地址执行证据仅来自 Linux；缺少文件明确跳过。

`KernelPowerCompletionTests.cpp` 覆盖 APC／DPC 准入、容量耗尽的原子重试、仅提供方同步／延迟有／无回调、捕获路径及 MPR。真实 `DriverWdmWaitWakeTests.cpp` 检查 DPC 取消回调直接请求 D0、独立 APC／DPC Query/Set、IRQL／CR8 不变、调用返回早于 PASSIVE 派发及完成，以及提升 IRQL 的 WAIT_WAKE 仍被拒绝。[提升 IRQL 场景](../examples/driver-wdm-elevated-power-scenario.json) 经 `DriverWdmWaitWakePublicTests.cpp` 覆盖 C API／CLI、普通／active-CFG 及首选／重定位；执行证据仍仅限 Linux。

`DriverResourceScenarioTests.cpp` 检查显式 JSON／原生事实、整数宽度、数量、物理／寄存器区间重叠、对齐、ID、空银行及配置序列化。`KernelMMIOTests.cpp`、`KernelMMIOFailureTests.cpp`、`KernelResourceBridgeTests.cpp` 与 `UnicornMMIOTests.cpp` 覆盖银行／映射所有权、别名、资源身份、紧凑清单生命周期、提供者时序、重启后的值保留、突然移除／电源可访问性、精确 CPU／API 事务及失败原子性。原创真实 WDK `driver_wdm_resources.c` 使用 `NEVERD_WDM_RESOURCE_FIXTURE`／`NEVERD_WDM_RESOURCE_CFG_FIXTURE`；`DriverWDMResourceTests.cpp` 执行真实标量及 REP 访问函数、普通／active-CFG 重定位、子区间别名、页尾映射、STOP／重启及非法访问。C API／CLI 测试在加载映像前拒绝无效事实，并执行相同的 14 请求重启场景，核对持久 IOCTL 输出及精确映射／取消映射次数。共享 [driver-register-bank-scenario.json](../examples/driver-register-bank-scenario.json) 需要此样例的寄存器／IOCTL 协议。缺少产物明确跳过，证据仅来自 Linux；测试不访问宿主物理内存，也不覆盖通用设备后端。

`DriverDMAScenarioTests.cpp` 验证显式能力、逻辑地址域、字节／数量／时间上限、严格事件方向和独立配置／观测。`KernelPhysicalMemoryTests.cpp` 与 `BackendBackingTests.cpp` 检查同页分配边界、固定引用、CPU 权限不变、MMIO／重入排除和整区间失败原子性；`KernelRequestMDLTests.cpp` 检查已构建描述符的别名及模型只读 PFN 与同一物理身份一致。`KernelDMATests.cpp`、`KernelDMABridgeTests.cpp` 和 `SchedulerDMATests.cpp` 覆盖实际 RAM 字节、适配器绑定的表调用、内嵌／排队 FIFO 所有权、独立回调／映射寿命、页片段、错误方向、释放预检、独立 PDO 地址域及资源代次／电源失败。原创真实 WDK `driver_wdm_dma.c` 使用 `NEVERD_WDM_DMA_FIXTURE`／`NEVERD_WDM_DMA_CFG_FIXTURE`；`DriverWDMDMATests.cpp` 及 C API／CLI 覆盖真实适配器指针、公共／SG 存储和分别配置的 DMA／中断事件。共享 [driver-dma-scenario.json](../examples/driver-dma-scenario.json)要求该 fixture 的协议。缺少产物明确跳过；执行证据仅限 Linux，不代表宿主 DMA、PCI 或通用设备引擎。 `pluginsdk/python/tests/test_driver_dma_integration.py` 使用 `NEVERD_TEST_LIBNEVERD`、`NEVERD_TEST_WDM_DMA_FIXTURE` 和 `NEVERD_TEST_WDM_DMA_CFG_FIXTURE` 执行现有带所有权管理的 JSON 绑定，覆盖实际字节、回调顺序和报告中的失败。

`KernelSEHTests.cpp` 检查纯展开计划、作用域顺序、非易失 GPR 恢复、有界栈及明确不支持的元数据；`KernelExceptionTests.cpp` 检查确切 API 参数个数、低 32 位状态、类型化异常、IRQL 上限及模型／CPU 状态不变。真实 WDK `/GS-` `driver_wdm_seh.c` 使用可选 `NEVERD_WDM_SEH_FIXTURE`／`NEVERD_WDM_SEH_CFG_FIXTURE`；`DriverWDMSEHTests.cpp` 执行普通／活动 CFG／重定位镜像，覆盖直接及辅助函数抛出、嵌套处理器、再次抛出、未捕获异常以及过滤器／finally／CPU 故障的明确拒绝。C API／CLI 执行 [driver-seh-scenario.json](../examples/driver-seh-scenario.json)，验证 null API 结果及真实来宾处理器消息。`pluginsdk/python/tests/test_driver_seh_integration.py` 使用 `NEVERD_TEST_LIBNEVERD`、`NEVERD_TEST_WDM_SEH_FIXTURE` 和 `NEVERD_TEST_WDM_SEH_CFG_FIXTURE`。缺少外部镜像明确跳过；证据仍限 Linux，不代表已支持用户缓冲区或通用 SEH。

`KernelDMAChannelTests.cpp`、`KernelDMAChannelBridgeTests.cpp` 和共用 `SchedulerDMATests.cpp` 检查混合分配 FIFO、回调返回宽度、纯接纳／释放预检、寄存器复用、连续页片段、整次操作刷新、CurrentIrp 快照及包／MDL／设备生命周期。原创真实 WDK `driver_wdm_dma_channel.c` 使用可选 `NEVERD_WDM_DMA_CHANNEL_FIXTURE`／`NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`；`DriverWDMDMAChannelTests.cpp` 执行普通／活动 CFG／重定位驱动，覆盖真实 MapTransfer 与 FlushAdapterBuffers 调用、公共／SG／通道共享额度、显式设备事务、IRQ/DPC 完成、连续操作、两个 PDO 及失败案例。C API／CLI 运行七请求 [driver-dma-channel-scenario.json](../examples/driver-dma-channel-scenario.json)，包括一个跨越两个已映射片段的单独事务。`pluginsdk/python/tests/test_driver_dma_channel_integration.py` 使用 `NEVERD_TEST_LIBNEVERD`、`NEVERD_TEST_WDM_DMA_CHANNEL_FIXTURE` 和 `NEVERD_TEST_WDM_DMA_CHANNEL_CFG_FIXTURE` 验证同一公开 JSON 接口。缺少产物明确跳过；Linux 证据不代表已支持系统 DMA 控制器或任意 HAL 映射／刷新模式。

`DriverInterruptScenarioTests.cpp` 覆盖显式原始／转换后描述符、混合及纯中断分配、严格事件字段／数量、源身份及独立 BOOLEAN 观测。`KernelInterruptsTests.cpp`、`KernelInterruptBridgeTests.cpp` 与 `SchedulerInterruptTests.cpp` 覆盖独占元组匹配、不透明令牌、资源代次／连接捕获、事件生命周期、精确选定的 Ex 字段、共用锁和 IRQL 恢复、回调所有权、同一时刻 ISR 优先级及修改前的容量失败。`KernelFrameworkRequestTests.cpp` 检查纯取消预览和批量令牌容量，不发布回调或消耗引用。原创真实 WDK `driver_wdm_interrupts.c` 使用 `NEVERD_WDM_INTERRUPT_FIXTURE`／`NEVERD_WDM_INTERRUPT_CFG_FIXTURE`；`DriverWDMInterruptTests.cpp` 执行普通／active-CFG 重定位、传统十一参数 ABI、Ex 版本 1／2／4、真实 ISR→DPC 完成、低位 AL 的 FALSE、同步／手动锁、独立 PDO、重启资源代次及非法硬件事实。C API／CLI 测试在加载映像前拒绝非法声明，并执行七请求的 [driver-interrupt-scenario.json](../examples/driver-interrupt-scenario.json)，检查 pending IOCTL 字节和独立递送观测。缺少映像明确跳过，执行证据仅来自 Linux，不代表支持共享／电平／MSI 中断或指令级抢占。

`DriverGuardTests.cpp` 与四个原创 `driver_guard.c` 变体覆盖启用／未启用的 CFG、重定位、检查／分派 ABI 和畸形目标。`KernelFrameworkTests.cpp`、`KernelFrameworkControlTests.cpp`、`KernelFrameworkQueueTests.cpp` 和 `KernelFrameworkRequestTests.cpp` 覆盖绑定、可回滚的设备创建、队列路由、缓冲区逻辑长度，以及清理顺序与 IRP／上下文生命周期。原创 `driver_kmdf_lifecycle.c` 与 `driver_kmdf_control.c` 可选用真实 WDK 1.33 头文件编译，并通过真正的 `FxDriverEntry` 库链接。将 CMake 缓存路径 `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` 指向生命周期映像，将 `NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` 指向普通／启用 CFG 的控制设备映像。缺少外部产物时会明确跳过。`DriverKMDFLifecycleTests.cpp`、`DriverKMDFControlTests.cpp` 及 `DriverScenarioPublicTests.cpp` 中的 C API／CLI 用例覆盖实际回调、缓冲／直接 I/O、工作项完成待处理请求、失败状态、卸载和重定位后的 CFG 执行。验证证据仍限于 Linux，不代表完整 KMDF 或 PnP／电源管理支持。

旧版取消测试将 API 续接保留到取消、嵌套清理与最终销毁结束；对于已经取消的请求，Ex 仍返回取消状态而不递送回调。`KernelFrameworkRequestAccessorTests.cpp` 与 `KernelRequestMDLTests.cpp` 覆盖共享的 64 位 Information、完成时长度验证、来源队列／IRP 身份、NULL WDF 文件句柄、保留句柄的 getter 结果、缓冲 MDL 缓存与首个方向的 ByteCount、直接描述符身份与延后映射、完成时回收，以及拒绝绕过 WDF 完成流程。真实控制设备 fixture 的 L、M、D、C 模式在普通／启用 CFG 映像中分别执行旧版取消、缓冲 MDL／信息、直接 READ／WRITE MDL 和完成后的访问。

取消测试覆盖仅允许传输请求配置的虚拟期限及报告字段、完成优先与已取消路径、标记／解除标记结果、排队与已递送回调的完成权限、回调等待及内部引用生命周期。调度器测试独立验证 DPC／取消／工作项顺序、容量、身份隔离和暂停／恢复。WDM 取消仍明确报告模型错误。


## 测试布局

`add_neverd_unittest` 创建一个 GoogleTest 可执行文件，并为每个发现的用例分配
与该可执行目标同名的 CTest 标签。

| 源码区域 | 目标与 CTest 标签 | 覆盖内容 |
|----------|-------------------|----------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | 跨平台子进程调用、引号、重定向与退出码 |
| `unittests/libc` | `NeverDLibCTests` | 已知 libc 名称与分类 |
| `unittests/safety` | `NeverDSafetyTests`、`NeverDSafetyIntegrationTests` | 汇目录、身份优先序、参数预过滤、拷贝越界猎取、堆生命周期审计，以及强制执行的 PE/ELF/Mach-O × x86-64/AArch64 六单元矩阵 |
| `unittests/lift` | `NeverDLiftTests` | Decoder/lifter LowIR 形状、IR 阶段、loader、重定位、格式 fixture、反编译与代表性 patch 流程 |
| `unittests/semantic` 中的大多数文件 | `NeverDSemanticTests` | 指令、ABI、控制流、C 表达式和 lift/recompile 差分语义 |
| `unittests/evm` | `NeverDEVMOpcodeTests`、`NeverDEVMBytecodeTests`、`NeverDEVMLoaderTests`、`NeverDEVMABITests`、`NeverDEVMAnalyzerTests`、`NeverDEVMDecoderPropertyTests`、`NeverDEVMProxyTests`、`NeverDEVMCallTests`、`NeverDEVMSemanticTests`、`NeverDEVMEmitterTests`、`NeverDEVMIntegrationTests` | 硬分叉元数据、输入规范化、ABI/签名歧义、CFG/SSA/恢复、穷举 decoder 边界与恶意输入、proxy/call 事实、解释器语义、LLVM/C/Solidity 差分执行及公共 API 路由 |
| `unittests/sbf` | `NeverDSBFMetadataTests`、`NeverDSBFProgramImageTests`、`NeverDSBFLoaderTests`、`NeverDSBFAnalyzerTests`、`NeverDSBFVerifierTests`、`NeverDSBFISAConformanceTests`、`NeverDSBFAgaveConformanceTests`、`NeverDSBFSemanticTests`、`NeverDSBFEmitterTests`、`NeverDSBFLLVMEmitterTests`、`NeverDSBFLLVMDifferentialTests`、`NeverDSBFSourceDifferentialTests`、`NeverDSBFMalformedCorpusTests`、`NeverDSBFUpstreamConformanceTests`、`NeverDSBFExternalOracleTests`、`NeverDSBFSolanaModelTests`、`NeverDSBFIntegrationTests` | v0-v4 元数据与 ELF 布局、严格 verifier/loader 行为、23 个固定 ELF 工件、独立 official oracle、全部 opcode 可用性、恶意输入、CFG/恢复及已执行的 LLVM/C/Rust 差分 |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | 四 ISA×三对象格式的重写/混淆等价性 |
| `unittests/semantic` 中的聚焦变换文件 | `NeverDSwitchXformTests`、`NeverDIndCallXformTests`、`NeverDCFGLoopXformTests`、`NeverDTwoTableXformTests`、`NeverDAvxUpperXformTests` | 从大型语义二进制拆出的快速重链接探针 |
| `unittests/corpus`（子模块） | `NeverDWindowsEHCorpusTests`、`NeverDRustEHCorpusTests`、`NeverDGoEHCorpusTests`、`NeverDCxxItaniumEHCorpusTests`、`NeverDObjCEHCorpusTests`、`NeverDAdaDEHCorpusTests` | 从 545 个钉住的真实二进制中读出的异常与运行时元数据，每个都在清单里声明了其恢复必须达到的下限 |

注册的事实来源是
[`unittests/CMakeLists.txt`](../../unittests/CMakeLists.txt)、
[`unittests/lift/CMakeLists.txt`](../../unittests/lift/CMakeLists.txt) 和
[`unittests/semantic/CMakeLists.txt`](../../unittests/semantic/CMakeLists.txt)、
[`unittests/evm/CMakeLists.txt`](../../unittests/evm/CMakeLists.txt) 和
[`unittests/sbf/CMakeLists.txt`](../../unittests/sbf/CMakeLists.txt) 和
[`unittests/safety/CMakeLists.txt`](../../unittests/safety/CMakeLists.txt)。

### 钉住的二进制 corpus

其它每个测试套件都自己构建被测对象，corpus 不是：它是一个子模块，装的是真实工具链
在本仓库够不到的宿主机上、为够不到的目标产出的二进制，每一个都按摘要钉住，旁边的
清单声明了它的恢复必须达到的下限。要回答"NeverD 从一个 `-O2` stripped 的 `armv7`
共享库里到底读出了什么"这类问题，只有这里给得出答案而不是论断。

这些套件只在 configure 被告知去找它们时才构建，所以这个开关就是它们是否受测的全部：

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

`check-neverd-corpus` 跑全部产线；`check-neverd-windows-eh-corpus`、
`check-neverd-rust-eh-corpus`、`check-neverd-go-eh-corpus`、
`check-neverd-cxx-itanium-eh-corpus`、`check-neverd-objc-eh-corpus` 与 `check-neverd-ada-d-eh-corpus` 各跑一条。三个
CI 宿主都带着这个开关配置并跑全部六条产线：字节到处都一样，但读字节的东西不一样，
在一台宿主上跑通不能说明另外两台。`scripts/audit_ci_test_inventory.py` 会拒绝缺少六
个标签中任何一个的清单——构建悄悄不再读 corpus 是一种没有任何测试能捕获的回归，因为
消失的正是那个测试。

EVM 操作码审计每次运行都会用 `git fetch --depth=1 --force` 强制获取官方默认分支的远端
`HEAD`：`https://github.com/ethereum/go-ethereum.git`。脚本解析并报告刚取得的精确
SHA，再在 detached 临时 worktree 中探测该对象。每次运行都使用名称不可预测的私有临时 bare
repository，在 detached worktree 的整个生命周期持有官方 fetch 的 authority ref 与精确 SHA，
最后一起销毁 repository 和 worktree。不使用共享持久 Git repository 或 cache。
本地与 CI 都不读取 `local_docs`、已有源码 checkout 或 submodule。固定 submodule 反而会在
最需发现实时漂移时陈旧：

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

公开 CLI 唯一接受的选项是 `--manifest-output`，不提供 remote/ref/toolchain override。输出
manifest 的封闭契约是 `schema 3`。

每条 Git 命令都会先清空全部继承的 `GIT_*`（包括 `GIT_CONFIG_*`），再只装入经过审计的
设置。`GIT_CONFIG_NOSYSTEM` 与 `GIT_CONFIG_GLOBAL` 禁用 system/global 配置；
`GIT_ATTR_NOSYSTEM` 与按命令设置的 `core.attributesFile` 禁用 system/global attributes，
`core.hooksPath` 禁用 hooks。意外的 private-repository 配置、graft、`objects/info/alternates` 或
`refs/replace` 都会让校验失败；`GIT_NO_REPLACE_OBJECTS` 会禁用 replacement 查找。

CI 仅在 `dev` 分支 push、pull request、手动触发与每日定时任务中运行同一项在线审计。
Go 探针反射 `params.Rules` 导出的全部 bool 字段，针对每个映射分叉调用公开的
`LookupInstructionSet(params.Rules)`，并扫描全部 256 个 byte slot。
`EVMUpstreamOpcodePolicy.def` 管理名称别名及类型化的历史/未排期 EOF 排除项，并校验
overlap/inactive 不变量；正交的 `EVMUpstreamSemanticsPolicy.def` 管理封闭的 Rules
清单、分叉映射、base-stack 例外与 EIP-8024 dynamic opcode family 声明。封闭 manifest 检查精确
revision、fork activation、byte/name、`base_min_stack` 和 `net_stack_delta`，拒绝未知或
重复字段、规则、分叉、名称与字节。槽位分配只依据 `operation.undefined`；`HasCost` 只用于
费用交叉检查，因为已定义的零费用操作也返回 false。每个 `defined && !HasCost` 槽位都必须
从声明的分叉起与 `EVM_GETH_ACTIVE_WITHOUT_COST` 精确匹配。未定义却有费用、未经评审却已
定义，或 marker 消失都会封闭失败。失败的 CI 会上传精确 revision、manifest 与日志 artifact。
parser 与漂移诊断有独立 Python 单元测试：

`EVMUpstreamSemanticsPolicy.def` 用唯一一条 `EVM_GETH_RULE_FIELD` 将每个导出的布尔
`params.Rules` 字段归入 `MappedForkSelector`、`NoOpcodeAllocation` 或
`ExcludedSelectorExpectedError`。probe 每次只启用一个字段并调用 `LookupInstructionSet`；前两类
必须无错误，第三类必须报错，返回的完整 256 槽 opcode/stack 指纹都必须等于 `ExpectedFork`。
当前 `IsEIP155`、`IsEIP2929`、`IsEIP4762` 与 `IsPetersburg` 是 Frontier 指纹的无分配字段；
`IsUBT` 必须报错并呈现 Cancun 指纹。

EIP-8024 dynamic opcode family 的成员与启用条件由 `EVMUpstreamSemanticsPolicy.def` 声明；
`EVMEIP8024Immediates.def` 仍是 single/pair 各字节 immediate semantics 的唯一权威，其清单都
显式分类全部 256 个字节。生产代码直接查表；实时审计以 `go -overlay` 向 `core/vm` 虚拟注入
wrapper，取得真正的私有 `operation.execute` handler，并对每个 active table/family 执行
`DUPN`、`SWAPN` 和 `EXCHANGE` 的 `3x256` candidates 加 `3 missing-operand cases`。测试核对
接受性、PC 增量、marker 推导的 operand/stack 变更、有效值的精确 underflow 和缺少 operand 时的
`0x00`；Python 对照同一 `.def`，不重复公式。

`EVM_HARDFORK_LATEST` 只有一个规范目标；封闭的 `EVMUpstreamForkAliases.def` 将 Prague 映射到
Pectra，将 Osaka 与 BPO1 至 BPO5 映射到 Fusaka，而 Paris/Shanghai/Cancun/Amsterdam/Bogota
映射到自身。未知名称封闭失败。单次审计记录的 `audit_unix_time` 同时驱动
`MainnetChainConfig.LatestFork(time)`（必须等于 NeverD latest）和
`LatestFork(max uint64)` 的 alias/已探测规范分叉检查。探针枚举真实的
`canonical fork jump tables` 与 `mainnet active/scheduled jump tables`，逐表完整比较，并显式
记录 dynamic family 或分叉的 `inactive` 状态。只得到部分表、family 或探针的 `partial` result
不会被接受，而会封闭失败。manifest 固定
`authority=official-fresh-fetch`、官方 URL、请求的 `HEAD` 与 SHA；公开 CLI 没有
remote/ref/toolchain 绕过，probe 使用 `GOTOOLCHAIN=local`。

Go request/response 与 Python controller 会在分配恶意元数据前执行
`input/collection/string hard limits`，超限输入、数组或字符串均封闭失败。它们还独立执行
`bounded diagnostic output`：超长展示包含 full-content `digest` 与
`explicit truncated marker`。每条命令都有有界的子进程输出和共享 deadline；超时或输出超限会
终止整个 `process group` 及其后代 process tree，并排空 pipe。所有 `.def parser` 都会拒绝
unparsed、unknown、duplicate、missing、out-of-range 条目并封闭失败。

当前 schema-3 实时回执记录 `schema_version=3`、`audit_unix_time=1787534659`、
`authority=official-fresh-fetch`、`remote=https://github.com/ethereum/go-ethereum.git`、
`ref=HEAD`、revision `02b73d4ea7181464175e0a6cbecc0a3a2655a562`、本地 `Go 1.24.0`、
`stack_limit=1024` 与 `diagnostics=[]`。它覆盖 `21 fork tables` 和 `20 Rules probes`，分类为
`15 mapped/4 no-op/1 expected-error`。两个 `mainnet active/scheduled` 记录均报告
`upstream BPO2`，由封闭映射对应到 `NeverD Fusaka`。EIP-8024 有 `23 table targets`，其中只有
`Amsterdam/Bogota` 为 active，产生 `1536 candidate executions` 与
`6 missing-operand cases`。`three handler symbols` 在两个 active target 间一致。Python audit 为
`67/67`，`C++ Opcode 10/10`。macOS 真实运行在 `sandbox-exec` 下成功，最终 `go run` 保持
offline；Linux workflow 强制 `bubblewrap`。

所有 Go 阶段——`go env`、`go mod init`、`go mod edit`、`go mod tidy`、
`go mod download` 与 `go run`——都必须经过 `capability-root` 文件系统沙箱。其读取能力只包含
私有 probe、fresh geth、校验后的 `resolved GOROOT` 和精确必需的系统 runtime root；只有隔离的
environment root 可写。网络仅授予需要它的依赖阶段，最终运行保持离线。测试在
`host HOME/workspace` 中放置 sentinel，要求访问被拒，并要求任何输出都不含其内容。Linux 验证
同构的 `bubblewrap` 策略，且不使用 `/` broad bind。

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

当前 CMake 注册的 11 个 EVM 测试目标为：

```text
NeverDEVMOpcodeTests
NeverDEVMBytecodeTests
NeverDEVMLoaderTests
NeverDEVMABITests
NeverDEVMAnalyzerTests
NeverDEVMDecoderPropertyTests
NeverDEVMProxyTests
NeverDEVMCallTests
NeverDEVMSemanticTests
NeverDEVMEmitterTests
NeverDEVMIntegrationTests
```

`NeverDEVMDecoderPropertyTests` 会在每个改变 decoder 的分叉上穷举全部双字节输入，比较
完整解码和精确 `JUMPDEST` 边界；它还以长度受限的确定性恶意输入覆盖所有分叉。

修改 EVM 控制流时，先运行不动点与高度域契约：

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

这些用例覆盖跨基本块 internal return、有限多目标合并、循环收敛与确定性边排序、
路径相关 whole-stack lane、相关性保留、未知跳转、精确非法目标，以及包括
`MaxAbstractInstructionTransfers` 在内的 fail-loud 分析预算。strict 只在已证明
`Reachable` 的 lane 上拒绝未知或分叉未激活 opcode；`MayReachable` 只保留 CFG 候选，
不能产出确定语义。随后应
运行全部 11 个 EVM 测试目标与在线上游审计；CFG 修改也可能影响 emitter 与集成行为。

修改 MedIR/HighIR 数据流时，还要运行 constant-phi、selector、类型化操作数、
格式错误图和深链契约：

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

这些用例验证相等与冲突的循环 phi、非相邻和跨基本块 selector 表达式、等式两种操作数
顺序、精确 ABI 位宽检查、类型化 storage/event/calldata 操作数、只从 root lane 沿
dispatcher 不匹配边恢复 selector/receive/fallback、共享 selector 的标准歧义、逐标准
`KnownFunctionVariantInfo` 选择，以及只在所有已证明可达的成功终态返回形状一致时输出
return list。它们还覆盖格式错误 MedIR 的确定性处理与深 producer walk。

## fixture 如何生成

### Lift 与格式 fixture

`unittests/lift/CMakeLists.txt` 在构建期间跨目标编译 C 与汇编源码。Clang target
triple 生成 x86-64、i386、AArch64、ARM32 ELF 对象，PE/COFF 对象和已链接
镜像，以及 PIC/no-PIC Mach-O i386 对象。存在 LLD 时，选定对象还会链接为
patch 测试所需的可执行文件。`NeverDLiftTests` 依赖 `lift-test-objects` 目标，
因此正常构建该测试二进制会刷新生成的 fixture。

多数 lift 测试使用 `NeverDLiftFixture.h` 调用构建出的 `neverd` CLI，并检查
LowIR、MedIR、HighIR、LLVM IR、生成的 C 或重写后的二进制。聚焦手动实验可用
`NEVERD` 环境变量覆盖 CLI 路径；普通 CTest 运行使用 CMake 嵌入的可执行文件。

### 内存安全 fixture

`unittests/safety/fixtures/binaries` 检入了 x86-64 与 AArch64 的 PE、ELF、Mach-O 镜像，以及各格式对应的 PDB 或 dSYM 伴生文件，每个镜像还附带一份链接器 MAP。MAP 是被 strip 的构建唯一还会留下的身份信息，因此每个单元还会显式指定 MAP 再分析一遍，用来钉住在既无类型也无源码行号时结论还能说什么。`NeverDSafetyIntegrationTests` 在每个主机上运行全部六个单元；任何必需镜像或伴生文件缺失都会在配置阶段失败，测试不存在按宿主工具链跳过的路径。

六个等价二进制来自同一源文件。`make` 只重建宿主原生 smoke fixture；完整矩阵用：

```bash
make -C unittests/safety/fixtures matrix
```

完整重建需要 Clang 的 Linux／Windows 交叉目标、LLD COFF 工具、两个 Darwin 架构与 `dsymutil`。规则会重映射调试路径并关闭 CodeView 命令行记录，避免检入的伴生文件捕获开发者工作区绝对路径。

### Windows 异常重建

修改 Windows 表驱动异常时，既要测试表示层，也要对已链接 PE 运行 patch 测试。
下面的聚焦 lift-suite 过滤器覆盖规范化 unwind/SEH/C++ 模型、损坏输入处理、
异常 CFG 边、HighIR、LLVM WinEH 生成、异常目录替换，以及 Guard CF/EH
continuation 重建：

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

受保护的 x64 汇编 fixture 需要 Clang Windows target 和 `lld-link`；其 CMake
链接使用 `/guard:cf` 与 `/guard:ehcont`。因缺少交叉链接器而跳过，不能作为
final-image 路径的有效证据。集成用例通过后，才证明重写后的 PE 可以重新加载，
且 runtime-function、unwind、load-config、Guard CF 与 Guard EH continuation
表保持有序、由文件承载，并只指向可执行目标。

已链接的 FH3 fixture 独立覆盖原生 C++ 闭包：固定状态表、HighC 注释、
personality 保留、生成的 catch 目标，以及重新加载后的 IP-to-state 图。

分析/原生支持矩阵和 fail-closed patch 契约见
[Windows 异常重建](windows-exception-reconstruction.md)。

### 语言异常模型

除 Windows 表模型以外的一切都集中在一个聚焦 target 中。
`NeverDLanguageEHTests` 覆盖 DWARF 帧链、Itanium 语言特定数据区、ARM EHABI、
Darwin compact unwind、Go 运行时帧元数据、Rust panic 机制，以及三种
Objective-C 运行时：

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

本套件中的表是逐字节手工装配而非编译出来的，因为其中大多数要验证的组合
没有任何单一工具链会同时产出。Objective-C 是最典型的例子：三种运行时都发出
Itanium LSDA，差别只在类型表槽位里放什么——而这个差别是彻底的，不是程度问题。
Apple 的槽位指向 `objc_typeinfo`，其前两个字段刻意模仿 `std::type_info`；
GNUstep 的 Objective-C++ 槽位指向真正的 `std::type_info` 子类；GNU 运行时的
槽位根本不是指针，而是类名字符串本身。把一种运行时的约定套到另一种的表上
不会报错，只会报出一个从别的东西中间读出来的类名——所以在读任何槽位之前，
先由帧的 personality 确定运行时。

同一套件还钉住两个容易混为一谈、但混淆即错误的区分。`@catch(id)` 与
`@catch(...)` 是不同的处理器——前者接收任意 Objective-C 对象，并放外来异常
从旁边继续传播——而每种运行时对二者的拼写都不同，所以把两者都报成 catch-all
的解码器，等于给那些本会飞过去的异常安上了处理器。另外，setjmp/longjmp 的
call-site 表索引的是调用点序号而不是地址，因此没能认出某个 SJLJ personality
的读取器不会报错，而是会凭空造出程序从未指定过的保护区间和 landing pad。

认出这种形式，和拒绝解码它，是两回事。一条 SJLJ 条目是一对 ULEB128 值——
一个派发选择子和一个动作偏移——而这个动作偏移在此处的含义与地址形式中完全
一致，所以动作链、catch 类型、异常规格，全都能从一张根本不指名任何代码的表
里读出来。唯一读不出的是每条条目守护的区间，因为说明它的是函数自己对
call-site 槽位的写入，而不是表里的任何东西。该套件还钉住了此处唯一不可信的
那个字节：GCC 把 call-site 编码写成 `DW_EH_PE_uleb128`，LLVM 写成
`DW_EH_PE_udata4`，两者随后都照样发射 ULEB128，而没有任何 personality 会去
读它——所以解码器也不许读。

personality 身份同样在这里钉住，因为它决定了上面每张表该怎么读。GNAT 用
GCC 给每个前端的那三种拼法命名自己的例程——`_v0`、`_sj0`、`_seh0`——并且在
Windows 上注册一个符号却转发到另一个，所以这四种拼法都必须落到 Ada 上。D
则是镜像的情形：三个编译器，同一个例程的三个名字，背后是同一套表。

### Unicorn 差分往返

语义 fixture 测试行为而不是文本形状：

1. 编写一个小型 C/汇编用例，或构造 LLVM IR。
2. 用 Clang/LLVM 为请求的目标编译它。
3. 在 Unicorn 中执行原始机器码，并捕获期望返回值或 fixture 定义的其他状态。
4. 通过 NeverD 加载并提升，发射 LLVM IR，再将结果编译回机器码。
5. 使用相同 ABI、输入、内存布局和 CPU 模型执行再生成的代码。
6. 比较可观察结果。

主要实现位于
[`SemanticRoundTripFixture.h`](../../unittests/semantic/SemanticRoundTripFixture.h)。
patch-full fixture 使用 `Codegen::compileForRewrite`（与 patch 操作相同的重写
backend），随后在完整 4×3 ISA/格式网格中比较基线与变换后代码。

确定性的 NeverD 语义失败应当成为失败测试。跳过只用于明确的外部能力边界，并应
阅读 skip 原因：缺少跨目标 linker 的绿色摘要不能证明该格式路径实际运行。

### EVM 差分后端

EVM 解释器测试提供确定性的 256 位 oracle。emitter suite 直接编译执行生成 LLVM，
通过 Clang lower 生成 C23 并在同一 host harness 中执行；安装 `solc`、`anvil`、
`cast` 与 `jq` 后，还会将生成 Solidity harness 部署至本地 Anvil。测试比较 status、
storage 与 instruction trace count。独立 raw-bytecode corpus 直接在 Anvil 原生 EVM
中执行 pre-Fusaka 标量 ALU、calldata/memory 复制、重叠 `MCOPY`、Keccak 与 return data。

解释器在任何 opcode 特有副作用之前执行类型化 stack preflight；
`EVMForkSemantics.def` 规定字节 `0x44` 在 Paris 前为 `DIFFICULTY`、从 Paris 起为
`PREVRANDAO`。`REVERT`、fault、step limit 和资源耗尽都会回滚事务状态；分配失败标为
`ExecutionFaultKind::ResourceExhausted`，若入口快照都无法建立，
`HasPersistentStateSnapshot` 为 false，结果不可提交。

### EVM 公开边界与预算回归

公开 API 测试会分别篡改规范
`Code`/`Fork`/`Instructions`/`JumpDestinations`，以及每个 LowIR table、range、ID、lane 与
edge reference。`execute` 必须在查找 instruction 前返回 `llvm::Error`；`lowerToMedIR`
必须在建立索引或按输入规模分配输出前，拒绝完整结构非法或超预算的 LowIR。
`lowerToMedIR` 测试还强制 option validation、resource validation、structure validation 的顺序，
并要求它们先于逐字段 `canonical decode replay` 和 `lowerCanonicalLowToMedIR`。公开 HighIR
恢复会重放校验外部 LowIR/MedIR；只有 `analyze` 能对自己持有的规范 IR 使用
`lowerCanonicalLowToMedIR` 与 `recoverCanonicalHighIR`，既避免递归或重复重放，也继续强制
所有 HighIR option/resource 预算。解释器随后对
`EVMInterpreterLimits.def` 声明的所有上限执行 exact-boundary 与 +1 测试：`MaxSteps`
保持专用 `StepLimit`；`MaxMemoryBytes`、`MaxTraceEntries`、`MaxLogEntries`、aggregate
`MaxLogDataBytes` 与运行期 `MaxPersistentStateEntries` 耗尽都返回
`ResourceExhausted` 并回滚事务效果。初始 aggregate `MaxHostReturnDataBytes` 或 persistent
state 过大是 API error。初始 `MaxCalldataBytes`、横跨 `BlockHashes`/`Balances`/`CodeHashes`/
`ExternalCode`/`BlobHashes` 的 aggregate `MaxHostEnvironmentEntries`，以及 aggregate
`MaxExternalCodeBytes` 同样属于 API error。`const execute preflight` 会在复制 environment、
snapshot 或 result 前拒绝它们。测试也覆盖 return-data `ArrayRef` view 与排序表 `lower_bound`
lookup，无需复制 buffer 或建立 PC map。

独立的 LowIR 边界测试覆盖 aggregate diagnostic 上限 `MaxLowDiagnostics` 与
`MaxLowDiagnosticBytes`，验证线性 decode/CFG 构造按精确数量和最终字节预先计费并拒绝零上限。
HighIR 安全测试覆盖按 lane 排序的 `Any/Exact/Excluded` domain、相等 match/exclusion、原始
`XOR(selector, constant)` 的 false-edge match 与 true-edge mismatch、零 word/calldata
size/call value 的逐边精化，以及 unknown condition 的 fail-closed 行为。
测试还包含 `EQ` 与 `raw XOR` 两类 back-jump regression，确保 `arguments`、`mutability`、
`return shape`、`region` 不受另一函数污染。其 exact-boundary 与 -1 测试覆盖
`EVMAnalysisLimits.def` 中的 `MaxHighDispatchCandidates`、aggregate
`MaxHighRecoveredArguments`、`MaxHighDiagnostics`、`MaxHighDiagnosticBytes`、
`MaxHighReferenceVisits`、`MaxHighMemoryTransferCells` 与
`MaxHighMemoryValueVisits`；所有输出 diagnostic（包括固定 malformed diagnostic）都必须在
分配前计入数量与最终字节数。LowIR 与 HighIR diagnostic 预算会独立测试；构造默认根 CFG
region 时必须在 reserve 或复制 block-PC 清单前计入 `MaxHighRegionBlockReferences`。
外部 CALL/CREATE 结果作为非确定 host outcome 探索两条精确 CFG 边，因此保留 ERC-1167
fallback 恢复；不可读的 selector 条件仍是 Unknown，不能凭空产生 fallback 或 function 事实。

控制流测试从 `EVMLowFaultKinds.def` 取得 `InvalidJumpDestination`，并用于
`end-of-code JUMPI`：目标非法且条件确定为 true 时没有成功 tail，属于确定 fault；条件确定为
false 时成功；条件未知时保留可能成功的 false 路径，不把整条 lane 标为确定 fault。

ABI 测试在精确上限和 +1 位置验证 `EVMABIParserLimits.def` 的 grammar 边界，以及
`EVMABITableLimits.def` 的公开表基数/text 边界；还会拒绝非法 kind/standard/evidence
enum、错配 metadata、非规范 signature/return list、被错误标记为 independent 的共享
selector、悬空或重复 variant，以及非 word 宽度的 event-topic `APInt`，再进入索引化
selector 或排序 topic lookup。

`NeverDEVMOpcodeTests` 还约束 metadata 架构：每个已分配 opcode 都在 byte encoding 与
typed value 之间往返，测试 family helper 边界与 hardfork alias，完整 stack contract
和 host argument maximum 保持推导而不在 backend 中重复。

### Solana SBF 差分后端

SBF 元数据测试会验证每个版本特性、操作码冲突边界、Murmur3 syscall hash、重定位、ELF machine、寄存器和 VM 地址常量。Loader fixture 不依赖 vendored 二进制，直接生成旧式 v0-v2 section 布局和无 section 的严格 v3/v4 program-header 布局。

`NeverDSBFISAConformanceTests` 按 v0-v4 的每个版本，将每一种 byte encoding
与独立审计的 typed manifest 对照。`NeverDSBFExternalOracleTests` 随后把 activation
和 boundary 决策与单独构建的官方 Anza 进程比较。
`NeverDSBFUpstreamConformanceTests` 为固定 Anza revision 中的全部 23 个 ELF
指定明确结果。

`NeverDSBFSemanticTests` 直接执行已验证的指令字节而不消费 MedIR，因此修改或破坏规范化 IR 不会让源 oracle 与后端意外达成一致。覆盖范围包括非单调的 v2 语义、内存、syscall、内部调用帧、fault、trace 和资源限制。LLVM module 会被验证；生成的 C 以 warnings-as-errors 编译，Rust 使用 `-D warnings`。公共 API 测试从生成的严格 SBF ELF 出发，遍历所有 IR 阶段、反汇编、CFG、元数据、LLVM、C 与 Rust。

## 一次性目标

自定义目标会构建其依赖，然后以主机 CPU 推导的并行度运行 CTest：

| CMake 目标 | 选择范围 |
|------------|----------|
| `check-neverd` | 所有已注册测试 |
| `check-neverd-semantic` | 仅 `NeverDSemanticTests` |
| `check-neverd-sbf` | 所有 `NeverDSBF*Tests` 目标/用例 |
| `check-neverd-patch-full` | 仅 `NeverDPatchFullTests` |
| `check-neverd-switch-xform` | 仅 `NeverDSwitchXformTests` |
| `check-neverd-cfgloop-xform` | 仅 `NeverDCFGLoopXformTests` |
| `check-neverd-twotable-xform` | 仅 `NeverDTwoTableXformTests` |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

`NeverDIndCallXformTests` 与 `NeverDAvxUpperXformTests` 当前没有
`check-neverd-*` 便捷目标；请按下文先构建，再用标签选择。
`check-neverd-semantic` 也不包含单独的变换或 patch-full 二进制；完整聚合应使用
`check-neverd`。

## 增量 CTest 工作流

先构建所属可执行文件，再选择其标签。这样可以避免重链接无关的大型语义目标。

```bash
# Lifter, loader, and format tests
cmake --build build-release --target NeverDLiftTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDLiftTests$' --output-on-failure --parallel 4

# Main semantic binary
cmake --build build-release --target NeverDSemanticTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDSemanticTests$' --output-on-failure --parallel 4

# A label-only focused transform binary
cmake --build build-release --target NeverDIndCallXformTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -L '^NeverDIndCallXformTests$' --output-on-failure --parallel 4

# 所有聚焦的 EVM 目标/用例
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# 所有聚焦的 Solana SBF 目标/用例
cmake --build build-release --target check-neverd-sbf --parallel 4
```

用 GoogleTest 派生的 CTest 名称运行单个回归：

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

常用选择器：

| 命令 | 用途 |
|------|------|
| `ctest --test-dir build-release -N` | 列出已发现用例而不运行 |
| `ctest --test-dir build-release -L '<regex>'` | 选择测试二进制标签 |
| `ctest --test-dir build-release -R '<regex>'` | 选择用例名称 |
| `ctest --test-dir build-release --output-on-failure` | 仅为失败显示诊断 |
| `ctest --test-dir build-release --stop-on-failure` | 第一个失败后停止 |
| `ctest --test-dir build-release --parallel 4` | 最多并行运行四个用例 |

GoogleTest 发现使用 `DISCOVERY_MODE PRE_TEST`，因此 CTest 枚举前必须存在对应测试
二进制。每用例 timeout 与独立的发现 timeout 定义于 `cmake/AddNeverD.cmake`，
只有存在测得的重型用例时才应放宽。

## 哪些测试应随代码变化？

| 变更区域 | 从这里开始 | 随后考虑 |
|----------|------------|----------|
| 架构 lifter 或 decode | `NeverDLiftTests` 中的命名用例 | 对应 ISA 语义往返 |
| LowIR CFG、函数检测、跳转表 | Lift CFG/switch 用例 | `NeverDSwitchXformTests`、`NeverDCFGLoopXformTests` 或 `NeverDTwoTableXformTests` |
| MedIR、ABI、标志、类型、SSA | MedIR/调用约定 lift 用例 | 跨 ISA 的 `NeverDSemanticTests` 用例 |
| HighIR 或结构化 C | HighIR/decompile 用例 | `NeverDCFGLoopXformTests` 与生成 C 编译检查 |
| PE/ELF/Mach-O loader 或输入重定位 | 对应的 `unittests/lift` 格式 fixture | 该单元格的全阶段加载/反编译测试 |
| 重写 codegen 或输出重定位 | `RewriteCodegenRTTests` 用例 | `NeverDPatchFullTests` 及存在时的已链接 patch fixture |
| patch 使用的 LLVM IR 变换 | 聚焦变换二进制 | `NeverDPatchFullTests` 组合 pass 网格 |
| C API 或 CLI | 直接 SDK/query 测试与 `unittests/semantic/CLIEndToEndTests.cpp` | 相关 pipeline/格式套件 |
| EVM loader、opcode、IR 或 backend | 最小的所属 `NeverDEVM*Tests` 目标 | 所有 EVM 目标，以及生成 C/Solidity 的编译检查 |
| SBF loader、ISA、IR 或后端 | 最小的所属 `NeverDSBF*Tests` 目标 | 所有 SBF 目标，以及生成 C/Rust 的编译检查 |
| Libc 识别 | `NeverDLibCTests` | 行为变化时的语义 call/ABI 用例 |
| 堆生命周期审计或拷贝越界猎取 | `NeverDSafetyTests` | `NeverDSafetyIntegrationTests` 的全部六个单元 |
| 进程执行或 quoting | `NeverDTestProcessTests` | 每个受支持主机上的一个受影响 CLI/语义用例 |

测试应在最低的稳定边界表达契约。LowIR 形状测试适合归因到 lifter；若两种看似
合理的 IR 形状可能行为不同，则必须使用语义往返。若小型 opcode、CFG 或可观察状态
断言已经足够，应避免保存整个函数的 golden dump。

## 与 CI 的关系

CI 在 Linux、macOS 和 Windows 上以 Release 开启测试构建，先审核发现的测试清单，
再应用平台特定的标签排除。配置定义于 `.github/workflows/ci.yml` 和
`scripts/audit_ci_test_inventory.py`。每个矩阵主机都必须包含 `NeverDSafetyTests`
和 `NeverDSafetyIntegrationTests`，而且每次都读取同一组已检入的 PE、ELF、Mach-O × x86-64、AArch64 fixture。由于没有单个矩阵 shard 代表所有昂贵套件，当机器具备全部跨目标工具时，本地 `check-neverd` 仍是最清晰的完整合并前信号。

## 当前 Solana SBF 一致性与 sanitizer 配置

本节的当前清单取代上方较短的 SBF 清单。source differential suite 除 clang 外还
需要 `rustc`；compiler skip 表示覆盖缺失。完整 aggregate 包括
`NeverDSBFProgramImageTests`、`NeverDSBFMalformedCorpusTests`、
`NeverDSBFISAConformanceTests`、`NeverDSBFUpstreamConformanceTests`、
`NeverDSBFLLVMDifferentialTests`、`NeverDSBFSourceDifferentialTests`，以及 metadata、
loader、analyzer、semantic、emitter、integration target。integrated profile 记录
命名 target 与结果，不冻结快速变化的汇总 case 数。

sanitizer profile 单独构建在 `build-sbf-asan-ubsan`。按 revision 锁定的 prebuilt
package 已包含所需的 fork-only header，因此 integration 也在同一个 fail-fast
ASan/UBSan profile 中运行。

```bash
cmake --build build-sbf-asan-ubsan --parallel 4 --target \
  NeverDSBFMetadataTests NeverDSBFProgramImageTests NeverDSBFLoaderTests \
  NeverDSBFAnalyzerTests NeverDSBFISAConformanceTests \
  NeverDSBFVerifierTests NeverDSBFAgaveConformanceTests \
  NeverDSBFSemanticTests NeverDSBFEmitterTests NeverDSBFLLVMEmitterTests \
  NeverDSBFLLVMDifferentialTests NeverDSBFSourceDifferentialTests \
  NeverDSBFMalformedCorpusTests NeverDSBFUpstreamConformanceTests \
  NeverDSBFSolanaModelTests NeverDSBFIntegrationTests

ASAN_OPTIONS=abort_on_error=1:detect_leaks=0:strict_string_checks=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
NEVERD_SBPF_ROOT=/path/to/sbpf \
NEVERD_AGAVE_CONFORMANCE_ROOT=/path/to/firedancer-test-vectors \
NEVERD_AGAVE_CONFORMANCE_REVISION=68bb4af40235562e8852fa23d5727e49c2a0b862 \
ctest --test-dir build-sbf-asan-ubsan --output-on-failure --parallel 4 \
  -L '^NeverDSBF'
```

### 固定的 SBF 证据快照（2026-08-24）

gate 将 Anza `sbpf` 固定在
`2510663bb8d894e8e3094be351e4bb4b604f1f84`、Agave 固定在
`ef210d67f2fabeee1730498188fa78854260c679`、Solana SDK 固定在
`122f32e571ce39face4beffaccea733e37c207fd`。官方 ELF manifest 全部 23/23 通过；
`NeverDSBFExternalOracleTests` 经 `SBFOfficialOracleProtocol.def` 和
`SBFOfficialVerifierCases.def` 与 `SBFOfficialExecutionConstants.def` 对照
1,411 个 opcode/verifier boundary case。
`SBFOfficialELFMutations.def` 是畸形 ELF 的表驱动契约；其总数仍会演进，因此不冻结。
另有独立的 `41-case strict ELF differential`，将完整 strict-v3 mutation matrix 送入
官方 `verify-elf-batch` 与 NeverD；这 41 个 case 不计入 1,411 总数。
`NeverDSBFAgaveConformanceTests` 认证 Firedancer test-vectors 的
`68bb4af40235562e8852fa23d5727e49c2a0b862`，匹配全部 1,955 `sol_compat_elf_loader_v1` 个 loader fixture
（接受 1,399、拒绝 556），并为每个接受的 ELF 比较 `entry_pc`、`text_off`、`text_cnt`、
`rodata_hash` 与 `calldests_hash`。此门禁不运行后续 instruction verifier。

额外的官方执行矩阵单独统计：恰有 508 个 active `(Version,Opcode)` case，另有
58 个 boundary case，共 566 个 exact execution case。它既不替代、也不计入
1,411 个 verifier probe 或 `41-case strict ELF differential`。
Linux Release CI 使用 `--print-pinned-revision`、`--print-test-vectors-revision` 与
`--print-toolchain`，并导出 `NEVERD_SBPF_ORACLE` 和
`NEVERD_AGAVE_CONFORMANCE_ROOT`，因此两个 external gate 都强制执行；普通本地运行
未提供明确 oracle/corpus env 时仍会发现 case，但允许 skip。

`SBF_RUNTIME_VERSION` 让 `RuntimeVersionPolicy::ChainProfile` 按历史 cluster/slot
计算：官方 feature account activation 使最大 ISA 从 V0 依次推进到 V1、V2、V3；
当前仍是 V3。显式 v4 使用 `RuntimeVersionPolicy::UpstreamToolchain` 做离线分析。
当前 10 MiB 上限精确为
`10'485'760` byte；65,536 仅是历史 provenance/test。`SBFFaultCodes.def` 固定
execution fault 的稳定值；`SBFSourceStatuses.def` 单独拥有 generated-source ABI。

10,000 规模 fixture 守护 worklist、function ownership 与 multi-latch，不固定某台机器
的耗时。cluster/account/slot row 支持 `RPC activation audit`，普通测试仍保持
deterministic 与 offline。

## 移动 SDK 导出证据

手动工作流 `Mobile SDK Export Evidence` 针对固定的 Xcode SDK 运行 `collect_mobile_ios_sdk_declarations.py --exports-only`。它原样留存 iOS 真机和模拟器 SDK 的 Foundation、CoreFoundation、UIKit 链接器映射，并记录目标、SDK 版本、SDK 设置哈希、文件大小和 SHA-256。常规声明收集器也会留存这些映射。文件缺失、为空、超出大小限制或位于 SDK 外部时，收集失败，并保留已完成的证据。链接器映射提供符号导出证据，不能证明调用 ABI 或方法恢复成功。

## 移动端 Swift String ABI 证据

手动工作流 `Mobile Swift String ABI Evidence` 使用 Xcode 26.5，为 arm64 iOS 设备与模拟器编译固定的 Swift 相等、排序比较探针及 C `swiftcall` 探针。`collect_mobile_swift_string_abi.py` 保存源码、LLVM IR、汇编、编译器身份、SDK 设置与 `libswiftCore.tbd` 及其哈希。两种语言都必须显示精确比较导入采用五个参数并返回 `i1`；C 必须将该结果显式扩展为一字节。目标或签名不符、命令失败及超时均保留部分证据并令采集失败。这些编译器证据不会安装运行时声明，也不证明方法已恢复。可在无 SDK 环境运行 `python3 -m unittest scripts.tests.test_mobile_swift_string_abi` 验证采集器。

## 模块化 MBA 简化

`SymReadability.*` 覆盖减法与补码的打印形式、结合律运算的代价、单比特和宽字面量、共享树大小饱和、有预算的候选选择，以及关闭采样时三比特的穷举等价性。`SymMBASample.*` 将窄值和任意精度验证与 AP 求值器比较，覆盖全部运算符、确定性赋值和未使用的宽输入。比较不同评分版本的候选质量时，必须用同一指标重算两边输出；SDK 随版本变化的大小计数仅供诊断。

## ARM32 与栈帧转发测试矩阵

```sh
cmake --build build-release --target NeverDSymbolicTests \
  NeverDSymSimplifyGuardTests NeverDLiftTests NeverDMBASourceTests \
  NeverDHighCStoreForwardingTests NeverDMetadataJSONTests --parallel 4
build-release/bin/NeverDSymbolicTests
build-release/bin/NeverDSymSimplifyGuardTests
build-release/bin/NeverDLiftTests \
  --gtest_filter='HighSymSimplify.*:HighFrameStoreForwarding.*:ELFARM32ModeTest.*'
build-release/bin/NeverDHighCStoreForwardingTests
build-release/bin/NeverDMBASourceTests
build-release/bin/NeverDMetadataJSONTests --gtest_filter='ELFARM32ModeCAPITest.*'
```

栈帧溢出测试矩阵通过两套 C 后端覆盖 x86-32（ELF/COFF/Mach-O）、ARM32（ARM 与 Thumb ELF）和 AArch64（ELF/COFF/Mach-O）。重复的私有栈帧读取必须化简为加减法，并在两个优化级别正确执行字节对、跨字边界对和确定性随机字。Clang AST 检查完整函数中的残留 MBA 运算，同时区分合法地址表达式。HighFrameStoreForwarding 覆盖精确访问位宽、局部变量变更、内存写入、前缀别名、部分重叠、有序内存、畸形或循环图，以及渲染展开预算；其窄位取补回归在语义化简后执行。HighCStoreForwarding 在四种架构上保持转发值所依赖的定义存活，包括浮点重新解释与额外直接使用。SymSimplifyGuard 检查加载身份和顺序、volatile/atomic 状态及 poison 边界。ELFARM32ModeTest 验证 ARM/Thumb 选择、地址规范化、纯映射符号对象、混合模式元数据保留及同一地址矛盾证据的拒绝。ELFARM32ModeCAPITest 在同一 SDK 会话中以混合元数据替换 Thumb 映像，验证反汇编、HighC、LLVMC 的明确错误，再重载 Thumb 验证解码器恢复。InstructionMode 覆盖相应的解码、代码指针、直接分支与代码生成边界。缺少跨目标 Clang 应标记为跳过，不能当作格式通过的证据。

## x64 原生同步异常

checked x64 的 `DIV`/`IDIV` 使用处理器产生的结果和 `#DE`。KVM 通过私有 supervisor IDT/IST 接收异常，WHP 使用明确的异常拦截位图；异常保留原始上下文和可用的错误码，与后端传输错误分开。OS 模型必须先消费可恢复事件，再安装明确的继续执行上下文。Windows 驱动将零除及商溢出映射为 `STATUS_INTEGER_DIVIDE_BY_ZERO`，并执行实际 SEH filter、`__finally` 和重试。`NeverDX64ExceptionTests` 可在禁用 Unicorn 时构建；原始 WDK 用例由 `DriverWDMCPUException` 验证。缺少的 WHP/ARM64 主机覆盖会明确跳过。

## 分阶段提交 RAM 效果

`RAMTransaction` 在物理执行租约内，只保存一条指令明确声明的写入范围的物理并集。结果观察器运行前恢复原始 RAM；取消、后端传输错误和观察器异常不会发布部分 RAM 或寄存器。CPU 异常在 RAM 回滚后保留架构异常状态。ARM64 的单次和成对写入共用该内存权威层。x64 支持 8/16/32/64 位 `XCHG`、`XADD`、`CMPXCHG`，LOCK 或隐式锁定形式要求自然对齐。`NeverDRAMTransactionTests` 将结果与独立宿主 CPU 对照，并验证回滚、别名和权限；不可用的平台明确跳过。设备事务和并行 SMP 仍不在此契约内；CPU 快照不会撤销已经提交的 RAM。

## 完整 x87 状态

`NeverDEmulationArch` 独立负责 ISA、页表及 FP 状态布局，原生与 Unicorn 传输共用该层。x64 上下文保存 x87 控制、状态、TOP、物理标签、操作码、指令／数据指针和八个 80 位寄存器。`FP0`–`FP7` 使用 `RegisterValue`，标量访问拒绝截断；`FPTag` 是物理非空位图。`NeverDX64FPTests` 覆盖全部 TOP、精确运算的宿主 FXSAVE/FXRSTOR 对照及上下文恢复。这不新增 checked x87 指令，也不证明全部舍入语义；缺少原生主机时明确跳过。

`driver-strict` 支持匹配的 Linux x64 主机上的 KVM 和 Windows x64 主机上的 WHP；`auto` 选择对应原生传输，跨 ISA 执行选择 Unicorn。显式 Unicorn 和原有 V1 API 保留可移植软件配置。原生执行在进入 CPU 前检查规范地址和指令效果；硬件不可用时明确失败且不回退。未支持的指令及 OS 行为仍明确报错。原生 ARM64/WHP 的实机证据仍待补充，这不表示兼容任意驱动或 Android/Darwin 环境。

使用 `executionCapabilities(Contract, ISA, Backend)` 查询所选后端的能力配置。`NativeLegacyX64` 描述原生 x64 驱动执行；`NeverDNativeDriverTests` 验证原有驱动集，也可在关闭 Unicorn 的构建中运行。

现有 CI 工作流在通用测试配置前运行完整模拟测试目录，并在 `emulation-focused` 保存发现清单、JUnit 结果和 CTest 日志。其他模块的失败不会阻止这组测试执行。硬件不可用及可选驱动样例缺失仍明确记录为跳过；软件运行或编译通过不能作为原生执行证据。

在 Linux 上，`NeverDUnicornDeadlineTests` 通过受控的 pthread 调度，让实际计时线程在客体入口前完成。测试覆盖 x64、ARM32 和 ARM64，要求入口前取消不产生客体效果，并验证下一次运行使用独立预算。测试调用公开引擎 API，不修改引擎私有状态。

`NeverDX64ExceptionTests` 中的 `X64StateTransition` 在原生 CPU 上执行独立 RAM 读取和 CR8 读取，交替改变 TLS 基址与权限级，在重复除法异常后恢复，并在取消进入后更改 TLS。修改原生状态传输时，应运行所属 CTest 标签，同时覆盖别名重映射、CPU 上下文、FP 状态和原始驱动结果对比。KVM/WHP 不可用仍显式跳过。


`NeverDKvmRunTests` 无需 `/dev/kvm` 即可验证 `KvmRunControl` 借用的传输回调。`StateTransfersUseTheEntryThreadAndPrepareOnceAcrossRetries` 检查准备、读取和被拦截的宿主进入使用同一线程，且中断重试期间只准备一次。其他用例覆盖准备失败而不进入、读取失败、准备期间停止及活动进入被取消；随后重新运行，确认不会复用旧回调。验证仍应包含真实取消、RAM 回滚、异常和原有驱动测试。

`ReusesCapturedStateAndInstallsHostChangesAcrossFaultsAndStops` 在宿主修改通用寄存器、首尾 XMM 寄存器、MXCSR 和 x87 控制字后，验证连续执行及真实 CPU 写入。停止进入后的实际 `FXSAVE64` 字节验证全部物理 80 位寄存器、TOP、标签、操作码和指针；重复除法异常也会使复用失效。这些机器边界测试不向 checked 配置开放额外的 x87 指令。

`NeverDKvmStateTransferTests` 在真实 KVM 执行后注入寄存器或 XSAVE 读取失败，然后用未改变的输入重试。独立的整数和打包字节结果证明失败的读取不会复用已经前进的原生状态。只有该测试程序包装 `ioctl`；原生主机不可用时明确跳过。

ARM64 原生整数状态读取由统一的 ISA 层负责。`AArch64GeneralState.def` 列出 X0–X30、SP、PC、NZCV 和 TPIDR_EL0；`captureAArch64GeneralState` 先暂存全部读取结果，再规范化 NZCV 并一次提交完整状态。KVM 和 WHP 共用该函数。任一读取失败都会保留全部输入状态，权限级、向量和未传输的寄存器保持不变。这不新增原生 FP/SIMD 指令准入。

checked ARM64 的标量及成对 RAM 访问可在 EL0 和 EL1 跨越具有独立底层内存或别名映射的页面。ISA 计算操作数范围，共享地址空间在进入前检查每个页面，并报告首个失败片段。`RAMTransaction` 在完整 CPU 步骤成功后提交声明的物理字节；故障和观察器停止会保留 RAM、寄存器及地址写回。`NeverDAArch64MemoryTests` 使用 `AArch64CrossPageCases.def` 中汇编生成的样例；这不新增 FP/SIMD 或 Windows ARM64 驱动加载。

`NeverDAArch64GeneralStateTests` 无需 Unicorn 或虚拟化设备，验证完整读取、NZCV 掩码、35 个读取位置分别失败、缺少读取回调及两种权限级下的成功重试。这些可移植状态验证和交叉编译不能替代 ARM64 KVM/WHP 实机运行证据。

`NeverDAArch64MemoryTests` 在两种权限级下覆盖 Unicorn、KVM 和 WHP 的 18 种标量及成对指令，检查所有跨页偏移、符号扩展及宽度结果、观察器顺序、第二页权限不足或缺失、显式消费故障后重试、重复物理别名，以及别名替换后的上下文恢复。修改前已复现合法跨页加载被拒绝的情况。不可用后端明确跳过；Unicorn 验证及交叉编译不能替代 ARM64 KVM/WHP 实机证据。

`NeverDDriverGuardMetadataTests` (`DriverGuardCases.def`) 检查零标志的未启用 CFG 元数据、两种加载地址下不变的回退指针、无效指针槽/目标及缺失重定位。执行用例显式选择 Unicorn/KVM/WHP，并使用 `driver-strict` 和 `checked-x64-v1`；不可用的后端分别跳过。`DriverPublicCLICases.def` 为 CLI 与兼容的 v1 C API 对比选择 `--backend unicorn`。原生后端及 `auto` 选择保留独立的公共接口覆盖，主机 API 不可用时不会静默回退。
