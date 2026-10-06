# Windows 驅動程式搶占式排程

可選的 `scheduling` 物件為 Unicorn、KVM 和 WHP 上的 x64 Windows 驅動程式啟用確定性的 CPU0 搶占排程。省略時保留協作式排程及僅於閒置時推進的虛擬時間。兩個欄位必須為正整數，乘積須能以 `uint64_t` 表示。空物件採用下方預設值。

```json
{
  "scheduling": {
    "quantum_instructions": 1024,
    "instruction_time_100ns": 1
  }
}
```

`quantum_instructions` 限定執行緒獲准嘗試的機器指令數；`instruction_time_100ns` 為每次嘗試指定虛擬時間，不代表硬體速度。模型 API 與指令交易完整結束後才切換。事件邊界不會重設剩餘時間片。報告以 `configuration.scheduling` 保留策略。

可執行續接、新工作項目與系統執行緒共用就緒順序。巢狀呼叫與 SEH 共用所屬執行緒的時間片。切換保留完整 CPU 上下文、邏輯執行緒身分、APC 狀態、有效 IRQL 和處理程序映射。PASSIVE/APC 層級可搶占；DISPATCH 以上遮蔽執行緒切換。臨界區與 guarded region 停用 APC，並不禁止執行緒搶占。

指令執行依期限順序推進計時器與 DMA。取消服務等待取消鎖可用，WDM 另要求 dispatch 已返回。可分頁的 provider 完成、電源策略及 PoFx 服務等待被動層級邊界；原期限保留，觀測記錄實際服務時間。時鐘產生的獨立回呼與阻塞呼叫的同執行緒 PoFx 回呼保留各自所有權。等待結果在期限到達時確定，不會被隨後重設的計時器或事件訊號覆蓋；一次指令嘗試跨過多個期限時也保持此順序。

尚未實作 Windows 優先權類別／動態提升、OS 模型的平行多核心、任意中斷巢狀、APC 交付或 Windows ring3 執行緒建立與排程。CPU 平行執行是獨立能力。測試涵蓋原創編譯驅動、極短時間片、浮點／GS 狀態、定時等待、交錯處理程序附加、取消及 PoFx 續接。ARM64 原生 OS 驗證另行追蹤。
