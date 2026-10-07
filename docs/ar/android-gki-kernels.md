# عقود نوى Android GKI المنشورة

تعطي NeverD الأولوية لفروع Android GKI المنشورة من 5.10 إلى 6.18 قبل بقية متغيرات Linux. يختار طلب العملية الفرع صراحةً:

```json
{"linux_kernel":{"gki":"android17-6.18"},"linux_files":{"files":[],"descriptor_limit":16}}
```

يصف عقد API 28 لملف Android الأصلي استيرادات Bionic ولا يختار إصدار النواة. يتحكم اختيار GKI في عقود `pidfd_open` والإخراج المتجهي وساعات CPU المرمّزة للعمليات و`ppoll` لواصفات العمليات بمهلة صفر، ضمن العقود المنفّذة. لا يصادق على نواة كاملة ولا يشغّلها ولا يستنتج الأجهزة أو نطاقات الأسماء أو الصلاحيات أو قوائم العمليات. تتوقف الخدمات غير المدعومة صراحةً. راجع [سياسة GKI الرسمية](https://source.android.com/docs/core/architecture/kernel/gki-releases).

## مراجعات المصدر المثبتة

تستخدم `LinuxGKIKernels.def` وسوم `r1` الرسمية التالية، وقد تحققت بتاريخ 2026-10-07. توفر الالتزامات الثابتة `kernel/pid.c` و`include/uapi/linux/pidfd.h` و`arch/arm64/configs/gki_defconfig` و`kernel/fork.c` و`lib/iov_iter.c` و`fs/read_write.c`. تأتي الأعلام من UAPI والتحقق من الاستدعاءات، لا من مستوى Android API أو نواة المضيف. ترد مصادر ساعات CPU المثبّتة في الجدول أدناه.

| الفرع المطلوب | وسم الإصدار | التزام المصدر المثبت | الأعلام المسموحة | استيراد iovec |
| --- | --- | --- | --- | --- |
| `android12-5.10` | `android12-5.10-2026-07_r1` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/pid.c) | `PIDFD_NONBLOCK` (`0x800`) | [نسخ جميع البيانات الوصفية أولاً](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/lib/iov_iter.c) |
| `android13-5.10` | `android13-5.10-2026-07_r1` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/pid.c) | `PIDFD_NONBLOCK` | [نسخ جميع البيانات الوصفية أولاً](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/lib/iov_iter.c) |
| `android13-5.15` | `android13-5.15-2026-09_r1` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/pid.c) | `PIDFD_NONBLOCK` | [نسخ جميع البيانات الوصفية أولاً](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/lib/iov_iter.c) |
| `android14-5.15` | `android14-5.15-2026-07_r1` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/pid.c) | `PIDFD_NONBLOCK` | [نسخ جميع البيانات الوصفية أولاً](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/lib/iov_iter.c) |
| `android14-6.1` | `android14-6.1-2026-09_r1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/pid.c) | `PIDFD_NONBLOCK` | [نسخ جميع البيانات الوصفية أولاً](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/lib/iov_iter.c) |
| `android15-6.6` | `android15-6.6-2026-07_r1` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/pid.c) | `PIDFD_NONBLOCK` | [مسار المخزن الواحد](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/lib/iov_iter.c) |
| `android16-6.12` | `android16-6.12-2026-09_r1` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/pid.c) | `PIDFD_NONBLOCK` و `PIDFD_THREAD` (`0x80`) | [مسار المخزن الواحد](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/lib/iov_iter.c) |
| `android17-6.18` | `android17-6.18-2026-09_r1` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/pid.c) | `PIDFD_NONBLOCK` و `PIDFD_THREAD` | [مسار المخزن الواحد](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/lib/iov_iter.c) |

تحدد هذه المراجعات العقد الحالي. يتطلب إصدار أحدث أو نقل إصلاحات تحققاً من المصدر واختبارات رجعية. اختيار GKI مع ملاحظة صريحة بغياب `pidfd_open` تناقض يُرفض قبل التحميل.

## الجزء المنفذ من واصفات العمليات

تشترك مصائد x64/AArch64 الخام و`syscall` في Bionic في `LinuxServices` وجدول واصفات تملكه الحمولة. تستخدم البتات الـ32 الدنيا من PID والأعلام؛ وتعيد الأعلام المجهولة أو معرّفات PID الموقعة غير الموجبة `EINVAL` قبل تخصيص الواصف. مصفوفة `tasks` الاختيارية قائمة ثابتة ومغلقة للمهام الضيفية الحية الأخرى:

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true},{"id":3000,"group_leader":false}]},"linux_files":{"files":[],"descriptor_limit":16}}
```

قائد المجموعة الجاري PID 1000 موجود ضمنياً حتى مع مصفوفة فارغة. يبقى البحث عن أهداف أخرى غير مدعوم عند إغفال القائمة؛ ويعيد PID موجب صالح خارج القائمة المعلنة `ESRCH` قبل التخصيص. تعيد المهمة الحية غير القائدة دون `PIDFD_THREAD` القيمة `EINVAL` في 5.10–6.12 و`ENOENT` في [`pidfd_prepare` للإصدار 6.18](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/fork.c). يسمح العلم المقبول في 6.12/6.18 بفتح المهام المعلنة غير القائدة. يسبق فحص الأعلام البحث دائماً.

تتطلب كل خانة `id` صحيحاً ضمن 1..2147483647 و`group_leader` منطقياً، بحد 4096 خانة. تُرفض التكرارات والحقول الإضافية ووصف PID 1000 بأنه غير قائد والقائمة دون GKI. يجب أن تشير ملاحظات الأولوية إلى القائمة أو العملية الجارية. لا تقترن القائمة الثابتة بخيوط Android التعاونية (`thread_limit > 1`)؛ إذ تتطلب الإنشاء وإعادة الجمع والصلاحيات وترجمة مساحات الأسماء إدارة مستقلة للعمر والملكية.

يلزم `linux_files` كي تشترك الملفات العادية وpidfd في الملكية والحد. يُخصص أصغر رقم متاح؛ يعيد النفاد `EMFILE`، ويحرر `close` الرقم، وتعطي الإعادة `EBADF`. يمكن إعادة استخدام رقم التدفق القياسي المغلق. لا تُستدعى pidfd أو ملفات أو عمليات المضيف.

تعيد `read`/`write` على pidfd صالح `EINVAL` قبل الوصول للبيانات، و`lseek` يعيد `ESPIPE` بعد فحص نقطة الأصل. يستورد `writev` البيانات الوصفية ويفحص نطاقات المستخدم أولاً؛ لذا قد يسبق `EFAULT` خطأ `EINVAL` لعملية الكتابة الغائبة. لا تُقرأ بيانات الحمولة ولا تُلتقط مخرجاتها. تستخدم stdout/stderr المستورد نفسه حسب الإصدار؛ راجع [ترتيب VFS](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/read_write.c).

تنسخ 5.10/5.15/6.1 مصفوفة iovec كاملة قبل فحص الأطوال؛ طول سالب مبكر مع بيانات وصفية لاحقة غير متاحة يعطي `EFAULT`. تُفحص النطاقات الأصلية قبل الحد حتى للمتجه الواحد. تفحص 6.6/6.12/6.18 العناصر تباعاً وتعطي `EINVAL` في الحالة نفسها؛ يُحد المخزن الواحد قبل فحص نطاقه، وتبقى فحوص النطاقات الأصلية لجميع المتجهات المتعددة. تستند القواعد إلى `copy_iovec_from_user` و`__import_iovec` و`import_ubuf`. دون GKI تبقى سياسة المخزن الواحد الحالية بلا استنتاج إصدار.

تحول Bionic الأخطاء الخام السالبة إلى `-1` و`errno` محلي للخيط؛ ويحفظ النجاح `errno`. لا تتوفر بيانات `fstat` الوصفية. يظل الاستطلاع الحاجب وإشعارات الخروج والإشارات عبر pidfd و`pidfd_getfd` و`fcntl` وioctl الخاص بـpidfs والمهام غير المرصودة غير مدعومة. لا تُستنتج الجدولة أو دورة حياة العملية من pidfd.

## مجموعة الاستطلاع المنفّذة بمهلة صفر

عند اختيار GKI تقبل استدعاءات `ppoll` الخام على x64/AArch64 و`syscall` متغير الوسائط في Bionic قيمة timespec صفرية صريحة وقناع إشارات مؤقتًا null. يوفر `linux_files.descriptor_limit` حد RLIMIT_NOFILE للضيف؛ ولا يجوز أن تتجاوزه البتات الـ32 الدنيا غير الموقعة من `nfds`. لا يشير الجدول المشترك إلى جاهزية pidfd الحية المرصودة، ويتجاهل الواصفات السالبة ويعيد `POLLNVAL` (`0x20`) للمغلقة. تُحسب كل خانة غير صفرية منفردة، بما فيها المكررات. تتوقف الجاهزية غير المرصودة للأنواع الأخرى صراحةً.

يسبق نسخ المهلة والتحقق منها معالجة القناع والواصفات. يُفحص حجم القناع غير null بعرضه الكامل ونطاقه المقروء قبل بلوغ حد القناع غير المدعوم؛ ويتجاهل القناع null الحجم. تُستورد جميع البيانات الوصفية قبل اختيار الجاهزية أو الإخراج. تُكتب حقول `revents` ذات 16 بت فقط بترتيب الخانات؛ ويحفظ العطل اللاحق الكتابات السابقة. يحتفظ الوصول المختلط داخل الحقل بحد النسخ الجزئي غير المدعوم المشترك. يشمل فحص نطاق المستخدم النهائي المصفوفة الفارغة أيضًا. لا تُعاد كتابة timespec الصفرية، لذا تكفي قابلية القراءة حتى لو كانت للقراءة فقط. لا يقرأ الاستدعاء ساعة الحائط ولا يقدمها.

رُوجعت القواعد في الإصدارات الثمانية الثابتة من `fs/select.c`: الدوال `ppoll` و`do_sys_poll` و`do_pollfd` و`poll_select_set_timeout` و`poll_select_finish`. تتبع جاهزية pidfd الحية `pidfd_poll` في `kernel/fork.c` للإصدارات الستة الأولى وفي pidfs للاثنين الأخيرين:

[5.10 fs/select.c](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/fs/select.c) · [6.18 fs/select.c](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/fs/select.c) · [6.6 kernel/fork.c](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/fork.c) · [6.12 fs/pidfs.c](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/fs/pidfs.c) · [6.18 fs/pidfs.c](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/fs/pidfs.c)

تختلف إشعارات الخروج وجمع العملية حسب الإصدار وتبقى خارج مجموعة المهام الحية الثابتة. تحتاج الانتظارات الحاجبة والأقنعة المؤقتة وتسليم الإشارات ومغلفات Bionic `ppoll` المسماة إلى عقود مستقلة.

## نطاق ساعات CPU للعمليات المنفّذ

مع اختيار GKI صراحةً، يقبل `clock_gettime` معرّفات العمليات السالبة المرمّزة PROF وVIRT وSCHED، ويفسّر البتات الدنيا الـ32 بإشارة. يحدد PID والنوع عينة صريحة في `linux_time`. توجد العملية الحالية ضمنيًا؛ ويجب إعلان العمليات الأخرى كقادة مجموعات أحياء في قائمة المهام المغلقة قبل تقديم عيناتها.

```json
{"linux_kernel":{"gki":"android17-6.18","tasks":[{"id":2000,"group_leader":true}]},"linux_time":{"advance_on_idle":true,"clocks":[{"id":1,"seconds":10,"nanoseconds":0},{"id":2,"seconds":3,"nanoseconds":4},{"id":-16006,"seconds":7,"nanoseconds":9}]}}
```

يشير `-16006` إلى SCHED للعملية PID 2000. ملاحظتا PROF وVIRT مستقلتان. تتشارك معرّفات SCHED الحالية 2 و-6 ‏(PID صفر) و-8006 ‏(PID 1000) عينة واحدة؛ والأسماء البديلة لـPROF هي -8/-8008 ولـVIRT هي -7/-8007. تُرفض البدائل المكررة ولو تساوت القيم. ثواني CPU غير سالبة والنانوثواني مطبّعة. يغيّر تقدم الخمول ساعات الوقت الجداري 0 و1 و7 فقط؛ وتظل عينات CPU ثابتة. لا يُستنتج استهلاك CPU من تنفيذ التعليمات.

يحدد TID الخاص بالمهمة الحالية مجموعة عمليتها أيضًا، بما في ذلك خيوط Android التعاونية دون قائمة خارجية. يعيد PID الخارجي الغائب من القائمة المغلقة أو الحي غير القائد `EINVAL` قبل الوصول إلى وجهة الإخراج. غياب القائمة أو عينة مجموعة معروفة يوقف العملية كغير مدعومة قبل النسخ. النوع غير الصالح يعيد `EINVAL`؛ وقد ينتج عن نسخ عينة صالحة إلى المستخدم `EFAULT`. تحتفظ المصائد الخام بالأخطاء السالبة؛ وحده Bionic يحدّث errno ويعيد -1.

تتبع قواعد الهدف والنوع `pid_for_clock` و`posix_cpu_clock_get` وموزّع الساعات وتعريفات المعرّفات في كل إصدار مثبّت:

| الفرع المطلوب | مصدر ساعات CPU للعمليات |
| --- | --- |
| `android12-5.10` | [b14525331e0d](https://android.googlesource.com/kernel/common/+/b14525331e0d5d335b037d6ed17d40424ed47b0a/kernel/time/posix-cpu-timers.c) |
| `android13-5.10` | [b9c8cb19d426](https://android.googlesource.com/kernel/common/+/b9c8cb19d426ec591e0a34dcc2d8638147e4ebc1/kernel/time/posix-cpu-timers.c) |
| `android13-5.15` | [0b6028f1f30d](https://android.googlesource.com/kernel/common/+/0b6028f1f30da3c2143bb40eee912c4974108e5c/kernel/time/posix-cpu-timers.c) |
| `android14-5.15` | [9938d39e2fe9](https://android.googlesource.com/kernel/common/+/9938d39e2fe99593abe347df3b82ea2f85c83d1d/kernel/time/posix-cpu-timers.c) |
| `android14-6.1` | [79480508eb1e](https://android.googlesource.com/kernel/common/+/79480508eb1eed09620f1cd5484dbfa4a677d0a9/kernel/time/posix-cpu-timers.c) |
| `android15-6.6` | [5556e039c32f](https://android.googlesource.com/kernel/common/+/5556e039c32fa02b239611dc8e5ebb958a7f12e1/kernel/time/posix-cpu-timers.c) |
| `android16-6.12` | [894a317b5382](https://android.googlesource.com/kernel/common/+/894a317b5382555614ef2be7a79cca79083c6cf2/kernel/time/posix-cpu-timers.c) |
| `android17-6.18` | [bab5f6aca819](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/time/posix-cpu-timers.c) |

يفعّل `init/Kconfig` في الإصدارات الثابتة مؤقتات POSIX افتراضيًا، ولا يعطّلها GKI defconfig. راجع [6.18 init/Kconfig](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/init/Kconfig).

يتبع تمييز ساعات FD وتوجيه CPU أيضًا [موزّع 6.18](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/kernel/time/posix-timers.c) و[تعريفات المعرّفات](https://android.googlesource.com/kernel/common/+/bab5f6aca819542b9dd13a70d3c62271e81b8e85/include/linux/posix-timers_types.h) المثبّتة. تظل ساعات FD وساعات CPU المرمّزة لكل خيط غير مدعومة. القائمة ملاحظة ضيف ثابتة؛ وتتطلب الصلاحيات ونطاقات الأسماء وعمر العملية وقياس CPU عقودًا خاصة بها.

## التحقق والتغطية المتبقية

ينفذ `LinuxPIDFDTests.cpp` ملفات ELF مستقلة x64/AArch64 عند O0/O2 للفروع الثمانية والخلفيات المتاحة، لفحص الأعلام والجدول المشترك والحدود وإعادة الاستخدام وترتيب الأخطاء وفشل البيانات الوصفية والحد مقابل النطاق الأصلي والقوائم المغفلة والمغلقة والمهام غير القائدة والبحث قبل نفاد FD. يكرر `AndroidSyscallTests.cpp` ملكية raw/Bionic والبحث وerrno في ستة إعدادات O0/O2 بإعادات تموضع عادية وAndroid packed وRELR. تثبت المصادر والتنفيذ هذا الجزء فقط؛ لم يتحقق إقلاع أصلي لكل صور GKI المثبتة. يتطلب توسيع Linux أدلة إصدارات وإعدادات وملاحظات لكل خدمة.

تختبر حالات CPU الهوية وترتيب الإخراج والأنواع المستقلة والعينات الصريحة وفصلها عن تقدم الوقت الجداري. يتحقق `AndroidTimeTests.cpp` من الإخراج المسمى/الخام وقيم الحراسة؛ ويتحقق syscall التعاوني من البديل الخاص بـTID الحالي غير القائد.

يفحص `ZeroTimeoutPollRetainsReadinessAndOrderedCopies` الاستدعاءات الخام O0/O2 لثمانية إصدارات GKI: الواصفات الحية والسالبة والمغلقة، وعد المكررات وتضييق الوسائط وترتيب المهلة والقناع وtimespec الصفرية للقراءة فقط، والاستيراد الكامل قبل الجاهزية وحفظ `revents` السابقة عند عطل لاحق. يحفظ `ZeroTimeoutPollKeepsUnobservedBoundaries` حدود النواة والموارد والأقنعة والانتظار والجاهزية غير المرصودة. يفحص `ReleasedGKIZeroTimeoutPollSharesRawAndBionicResults` في Android الجدول المشترك وملكية errno عبر ستة ملفات حزم.
