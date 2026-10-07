# Windows 驱动抢占式调度

可选的 `scheduling` 对象为 Unicorn、KVM 和 WHP 上的 x64 Windows 驱动启用确定性的 CPU0 抢占调度。省略时保留协作式调度及仅在空闲时推进的虚拟时间。两个字段必须为正整数，乘积必须能用 `uint64_t` 表示。空对象采用下方默认值。

```json
{
  "scheduling": {
    "quantum_instructions": 1024,
    "instruction_time_100ns": 1
  }
}
```

`quantum_instructions` 限定线程获准尝试的机器指令数量；`instruction_time_100ns` 为每次尝试分配虚拟时长，不代表硬件速度。模型 API 和指令事务完整结束后才切换。事件边界不会重置剩余时间片。报告在 `configuration.scheduling` 保留所选策略。

`KeSetPriorityThread` 和 `KeQueryPriorityThread` 在 `PASSIVE_LEVEL` 访问运行时优先级。设置接受 1..31，返回原优先级；0 为保留值。确定性配置将每个模拟线程的初始值设为 8。选择最高优先级的就绪线程，同级按就绪顺序及时间片轮转。高优先级线程唤醒或优先级改变后，在 DISPATCH_LEVEL 以下会于下一条客体指令前抢占，并保留被中断的剩余时间片。嵌套回调和 SEH 共享所属线程的优先级。仍被引用的已退出系统线程对象可查询到回收为止；不支持修改正在终止或已退出线程。借用的回调线程对象随所属线程结束而回收，复用栈槽不会继承旧优先级。

内核 mutex 的所有权属于逻辑线程，并跨嵌套回调和 SEH 保持。`KeWaitForSingleObject` 为延迟获取保存等待线程身份；该线程可在任一调用栈上执行 `KeReleaseMutex`。普通 APC 的禁用持续到最后一次递归释放，最外层返回会拒绝遗留的 mutex。

可运行的续接、新工作项和系统线程共用就绪顺序。嵌套调用与 SEH 共享所属线程的时间片。切换保存完整 CPU 上下文、逻辑线程身份、APC 状态、有效 IRQL 和进程映射。PASSIVE/APC 级允许抢占；DISPATCH 及以上屏蔽线程切换。临界区和 guarded region 禁用 APC，但不禁止线程抢占。

指令执行按截止时间顺序推进定时器和 DMA。取消服务等待取消锁可用，WDM 还要求 dispatch 已返回。可分页的 provider 完成、电源策略和 PoFx 服务等待被动级边界；原期限保持待处理，观测记录实际服务时间。时钟产生的独立回调与阻塞调用的同线程 PoFx 回调保持各自所有权。等待结果在期限到达时确定，不会被随后重设的计时器或事件信号覆盖；一条指令尝试跨过多个期限时也保持这一顺序。

此配置尚未实现 Windows 优先级类别／动态提升、OS 模型的并行多核、任意中断嵌套、APC 交付或 Windows ring3 线程创建与调度。CPU 并行执行属于独立能力。测试覆盖原创编译驱动、极短时间片、浮点／GS 状态、定时等待、交错进程附加、取消和 PoFx 续接。ARM64 原生 OS 验证仍单独跟踪。
