**اللغات**: [English](../process-emulation.md) | [简体中文](../zh-CN/process-emulation.md) | [繁體中文](../zh-TW/process-emulation.md) | [日本語](../ja/process-emulation.md) | [한국어](../ko/process-emulation.md) | [Français](../fr/process-emulation.md) | [Deutsch](../de/process-emulation.md) | [Español](../es/process-emulation.md) | [Italiano](../it/process-emulation.md) | [Русский](../ru/process-emulation.md) | [العربية](process-emulation.md)

[← فهرس التوثيق](README.md)

# محاكاة عمليات الضيف

ينفذ `neverd emulate` صورة ضمن ملف صريح لنظام الضيف. لمحرك CPU وتحليل الصورة ودخول العملية وخدمات نظام التشغيل ملاك منفصلون. فعّل `NEVERD_ENABLE_CPU_EMULATION=ON`؛ وتمكين محاكاة برامج التشغيل يتضمنه أيضاً.

أول ملف هو `linux-elf64-v1`: يشغّل برامج ELF `ET_EXEC` وبرامج static PIE ذاتية الترحيل من نوع `ET_DYN` لـx64 وAArch64 عند CPL3 أو EL0. يحمّل المقاطع الحقيقية ويبني المكدس الابتدائي ويستأنف التنفيذ على دفعات ويعالج طلبات Linux الصريحة. هذا نموذج عملية مستقل، لا توزيعة Linux كاملة ولا وعد بتشغيل ملفات libc عشوائية. الربط الديناميكي والإشارات والخيوط وأنظمة الملفات والخدمات غير المدعومة تفشل صراحةً. تشمل مجموعة x64 المحدودة بعض SSE/SSE2؛ أما AArch64 فيبقى ملفاً صحيحاً عددياً. عمليات Windows وAndroid وDarwin وأنظمة النواة الأخرى خارج هذا الملف.

## CLI وSDK

```bash
neverd emulate guest.elf --profile=linux-elf64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"],"instruction_limit":100000}'
```

على مضيف Linux مطابق يختار KVM، وعلى Windows مطابق يختار WHP؛ وتستخدم تراكيب المضيف/الضيف الأخرى Unicorn. عدم توفر الخلفية المختارة خطأ بلا رجوع صامت. يظل نموذج Linux مستخدماً حتى عند تشغيل ELF على Windows. راجع [تنفيذ CPU](cpu-execution.md) لقائمة التعليمات والقيود.

يصدر CLI تقرير JSON واحداً: رمز الخروج 0 لحالة ضيف صفر، و2 لحالة أخرى، و3 لتنفيذ غير مكتمل (بما فيه الأعطال والحدود)، و1 لإعداد/API غير صالح. حالة الضيف الفعلية في `exit_status`. نقطة C المضافة هي [`neverd_emulate_process_json`](../../include/neverd/sdk/NeverDCAPIProcess.h)؛ تتطلب جلسة ومساراً غير فارغ وملفاً صريحاً وخيارات اختيارية. حرر النتيجة عبر `neverd_free_string`؛ النتيجة NULL تعني فشل الإعداد ويشرحها `neverd_last_error`. عطل الضيف أو توقف الموارد يعيدان تقريراً، ولا تتطلب صورة التحليل المحملة في الجلسة ولا تتغير.

```python
report = session.emulate_process(
    "guest.elf", "linux-elf64-v1",
    '{"backend":"unicorn","arguments":["guest"],"environment":[]}',
)
output = bytes.fromhex(report["stdout_hex"])
```

## الخيارات والنتائج

خيارات JSON كائن لا يتجاوز 64 KiB. الحقول المجهولة أو null والأنواع غير الصحيحة وNUL المضمّن والحدود غير الموجبة مرفوضة.

| الخيار | الافتراضي | العقد |
|---|---|---|
| `backend` | `auto` | `auto` أو `unicorn` أو `kvm` أو `whp` |
| `arguments` | اسم الإدخال | argv كامل مع argv[0]؛ الفارغ يستخدم الافتراضي |
| `environment` | `[]` | سلاسل الضيف فقط؛ لا يرث بيئة المضيف |
| `instruction_limit` | 100000 | محاولات التعليمات المقبولة المشتركة |
| `event_limit` | 10000 | أحداث system-call؛ تُخصم قبل معالجة الخدمة |
| `timeout_microseconds` | 5000000 | موعد رتيب يبدأ بعد إعداد العملية |
| `memory_limit` | 67108864 | ميزانية الذاكرة الفعلية/المربوطة |
| `stack_size` | 1048576 | مكدس بمحاذاة الصفحة ضمن الميزانية |
| `output_limit` | 1048576 | مجموع stdout/stderr الملتقط |
| `instruction_quantum` | 1024 | فاصل القبول قبل إفساح المجال للمشغل |

`schema_version` يساوي 1. يتضمن التقرير الملف والمعمارية والخلفية المختارة وسببها و`stop_reason` و`exit_status` القابل لـnull والتشخيص وعناوين PC والعدادات وسجلات الخدمة وآخر نتيجة CPU ذات نوع. العناوين وأرقام الخدمات والسجلات والقيم الخام سلاسل سداسية **بلا** `0x` كيلا تفقد الدقة في JSON؛ و`stdout_hex` و`stderr_hex` يحفظان NUL وUTF-8 غير الصالح. نتيجة syscall بقيمة null تعني عدم وجود نتيجة ممذجة، مثل الخروج أو طلب غير مدعوم، ولا تعني صفراً ناجحاً.

## دلالات ملف Linux

يستخدم نموذج نظام التشغيل رؤوس البرامج التي فكها محمل ELF الموجود. تتحقق السياسة من وسوم ABI ومحاذاة المقاطع وجداول رؤوس البرامج المربوطة وحدود عناوين المستخدم. تفحص خطة الربط النطاقات والصلاحيات والتداخل والميزانية قبل التخصيص، ولا تنشر إلا فضاء عناوين خاصاً جاهزاً بالكامل. تحفظ المقاطع بادئة/ذيل صفحات الملف، وتصفّر BSS، وتحترم الصلاحيات وتحجز فجوات حارس للمكدس؛ وتُرفض تخطيطات تداخل الصفحات والرؤوس المتعارضة بدلاً من التخمين.

يحتوي المكدس الابتدائي argc/argv/envp/auxv بمحاذاة، وPHDR/PHENT/PHNUM وentry وحجم الصفحة وقيماً ثابتة للهوية. PID/TID/UID/GID النموذجية تساوي 1000. أول 16 بايتاً من SHA-256 للمدخل هي `AT_RANDOM` لضمان قابلية التكرار؛ وهذه سياسة نموذج حتمية لا عشوائية تشفيرية. HWCAP/HWCAP2 صفريان ولا يوجد vDSO.

الخدمات المنفذة هي `write` و`exit` و`exit_group` و`getpid` و`gettid` مع أرقام منفصلة لـ[x64](https://github.com/torvalds/linux/blob/master/arch/x86/entry/syscalls/syscall_64.tbl) و[ARM64](https://github.com/torvalds/linux/blob/master/include/uapi/asm-generic/unistd.h). عودة SYSCALL على x64 تطبق clobber لـRCX/R11 إضافة إلى RAX وPC التالي؛ يستخدم ARM64 x8 للرقم وx0 للنتيجة. الطلبات الأخرى تتوقف كـ`unsupported_service` ولا تنفذ system calls على المضيف.

الواصفان 1 و2 مصرفا بايتات افتراضيان. يتحقق `write` من صفحات المستخدم المقروءة؛ يعيد بادئة قابلة للقراءة إن تعذر الوصول إلى صفحة لاحقة، و`EFAULT` إن لم تكن أي بايتات مقروءة. الواصف الخاطئ يعيد `EBADF`، والكتابة ذات العدد صفر على واصف صالح لا تفحص المؤشر. لا يحاكي ذلك ذرية أنابيب Linux أو الملفات. حد الإخراج يوقف التنفيذ قبل نشر كتابة تتجاوزه.

## التحقق

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
# In a shared-library/CLI build:
cmake --build build-cpu --target NeverDProcessPublicTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDProcessPublicTests$' --output-on-failure
```

تجمع الاختبارات ملفات ELF أصلية لـISAين، وتفحص بيانات/BSS وبيانات البدء وsyscalls والأخطاء والمخرجات الثنائية والصلاحيات والعطل والميزانيات عبر الدفعات. غياب الخلفية يُسجّل كتخطٍ صريح. الاختبارات العامة تمر عبر C API وCLI وتطابق التقرير ورمز الخروج. التجميع المتقاطع وUnicorn على ARM64 ليسا دليلاً على KVM/WHP أصلي لـARM64.

## static PIE وTLS والتحقق

يستخدم static PIE قيمة load bias حتمية لا تقل عن `0x40000000` وتزداد لمراعاة محاذاة `PT_LOAD`. تستخدم المقاطع وPC الدخول و`AT_PHDR`/`AT_ENTRY` القيمة نفسها، وتبقى قيم ترويسات الملف الأصلية بلا تغيير؛ يظل `AT_BASE` صفراً لعدم وجود مفسر. مصدر الربط هو بايتات الملف الأصلية، لا مقاطع التحليل المعدلة، وعلى بدء الضيف تنفيذ relocation والتهيئة بنفسه. يفك المحمّل `PT_DYNAMIC` من سجلات الملف الأصلية المحدودة دون الاعتماد على section headers؛ ويشترط النموذج جدولاً مقروءاً ومنتهياً لا يتجاوز 4096 إدخالاً. يُرفض `PT_INTERP` وتبعيات الربط الخارجية ووسوم filter/audit؛ لا يوفر النموذج dynamic linker أو حل الرموز أو تشغيل المنشئات.

تُتحقق قوالب TLS الساكنة `PT_TLS` كحقائق من المحمّل: قالب واحد، وحدود ملفات/ذاكرة محدودة، وتوافق المحاذاة وبايتات أولية مقروءة. يخصص بدء الضيف كتل TLS ويهيئها ويركب مؤشر الخيط؛ لا يخترع نموذج Linux بنية TCB أو DTV خاصة بـlibc. هذا يدعم local-exec TLS المولد من المترجم في البرامج المستقلة. TLS الديناميكي وجدولة خيوط OS خارج النطاق.

على x64 تدعم `arch_prctl` عمليات `ARCH_SET_FS` و`ARCH_GET_FS` و`ARCH_SET_GS` و`ARCH_GET_GS`. يقبل Set قاعدة ضمن نطاق المستخدم حتى إن لم تكن مربوطة؛ ويظل الوصول اللاحق خاضعاً للصلاحيات. قاعدة نطاق النواة تعيد `EPERM`، ومؤشر Get غير الصالح يعيد `EFAULT` دون fault للـCPU. العمليات الأخرى غير مدعومة وتفشل صراحة. يثبت ARM64 `TPIDR_EL0` بتعليمة `MSR`؛ وتحافظ `MRS` ومراجع FS/GS واستعادة السياق على مؤشر كل خيط بين الدفعات ومداخل المحرك. هذا لا ينشئ مجدولاً للخيوط.

تتحقق fixtures TLS وstatic PIE من كتل مستقلة ومحاذاة، وصفر BSS، وقيم auxv المترحلة وخانات RELA الأصلية الصفرية قبل أن ينفذ الضيف relocations للبيانات والمؤشرات بنفسه. تختبر حالات x64 أخطاء `arch_prctl` دون إفساد القاعدة السابقة. أضف `NeverDThreadPointerTests` لأوامر البناء والاختبار أعلاه؛ غياب الخلفية يظل تخطياً صريحاً.
