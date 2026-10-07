# عقود نوى Android GKI المنشورة

تعطي NeverD الأولوية لفروع Android GKI المنشورة من 5.10 إلى 6.18 قبل بقية متغيرات Linux. يختار طلب العملية الفرع صراحةً:

```json
{"linux_kernel":{"gki":"android17-6.18"},"linux_files":{"files":[],"descriptor_limit":16}}
```

يصف عقد API 28 استيرادات Bionic ولا يختار إصدار النواة. يتحكم GKI حالياً في `pidfd_open` والإخراج المتجهي المنفذين فقط؛ ولا يثبت توافق نواة كاملة أو يقلع صورتها أو يستنتج الأجهزة أو مساحات الأسماء أو الصلاحيات أو قائمة العمليات. تتوقف الخدمات غير المدعومة بوضوح. راجع [سياسة GKI الرسمية](https://source.android.com/docs/core/architecture/kernel/gki-releases).

## مراجعات المصدر المثبتة

تستخدم `LinuxGKIKernels.def` وسوم `r1` الرسمية التالية، وقد تحققت بتاريخ 2026-10-07. توفر الالتزامات الثابتة `kernel/pid.c` و`include/uapi/linux/pidfd.h` و`arch/arm64/configs/gki_defconfig` و`kernel/fork.c` و`lib/iov_iter.c` و`fs/read_write.c`. تأتي الأعلام من UAPI والتحقق من الاستدعاءات، لا من مستوى Android API أو نواة المضيف.

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

تحول Bionic الأخطاء الخام السالبة إلى `-1` و`errno` محلي للخيط؛ ويحفظ النجاح `errno`. لا تتوفر بيانات `fstat` الوصفية. يظل الاستطلاع وإشعارات الخروج والإشارات عبر pidfd و`pidfd_getfd` و`fcntl` وioctl الخاص بـpidfs والمهام غير المرصودة غير مدعومة. لا تُستنتج الجدولة أو دورة حياة العملية من pidfd.

## التحقق والتغطية المتبقية

ينفذ `LinuxPIDFDTests.cpp` ملفات ELF مستقلة x64/AArch64 عند O0/O2 للفروع الثمانية والخلفيات المتاحة، لفحص الأعلام والجدول المشترك والحدود وإعادة الاستخدام وترتيب الأخطاء وفشل البيانات الوصفية والحد مقابل النطاق الأصلي والقوائم المغفلة والمغلقة والمهام غير القائدة والبحث قبل نفاد FD. يكرر `AndroidSyscallTests.cpp` ملكية raw/Bionic والبحث وerrno في ستة إعدادات O0/O2 بإعادات تموضع عادية وAndroid packed وRELR. تثبت المصادر والتنفيذ هذا الجزء فقط؛ لم يتحقق إقلاع أصلي لكل صور GKI المثبتة. يتطلب توسيع Linux أدلة إصدارات وإعدادات وملاحظات لكل خدمة.
