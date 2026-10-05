**اللغات**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](darwin-emulation.md)

<!-- i18n-source: 651fc905db4bb2d62a1036bb6495b60cad23dde9b5e655b4b4ca408294a3d83c -->

[← فهرس الوثائق](README.md)

# بيئات العمليات الضيفة لـ macOS وiOS

ينمذج `lib/emulation/os/darwin/` عمليات Mach-O مستقلة ذات حدود صريحة، منفصلةً عن نقل CPU للمضيف. فعّل `NEVERD_ENABLE_CPU_EMULATION`؛ لا تلزم محاكاة برامج تشغيل Windows. يحدد `macos/` و`ios/` ملفات تعريف صريحة للمنصات.

| ملف التعريف | منصة Mach-O | ISA الضيف | صفحة النظام |
| --- | --- | --- | --- |
| `macos-macho64-v1` | macOS | x86-64، ARM64 الأساسي | 4 KiB لـ x64؛ 16 KiB لـ ARM64 |
| `ios-macho64-v1` | جهاز iOS | ARM64 الأساسي | 16 KiB |
| `ios-simulator-macho64-v1` | iOS Simulator | x86-64، ARM64 الأساسي | 4 KiB لـ x64؛ 16 KiB لـ ARM64 |

ملف الجهاز ليس صورة للمحاكي، ولا تُستنتج منصة الضيف من المضيف. عند تطابق ISA على macOS يمكن استخدام [HVF](macos-hvf.md)، وإلا يستخدم `auto` واجهة Unicorn. تبقى دقة تعيين CPU عند 4 KiB. تتشارك [واجهات C وPython وCLI](process-emulation.md) الخيارات والحدود والتقارير.

```sh
neverd emulate guest.macho --profile=ios-macho64-v1 \
  --options='{"backend":"auto","arguments":["guest","argument"],"environment":["MODE=test"]}'
```

## الصورة والبدء

يحافظ `MachOExecutionImage` على البايتات الأصلية دون ترقيعات إعادة التموضع الناتجة عن التحليل. لا تُقبل إلا صور thin little-endian من نوع `MH_EXECUTE` مع منصة ونقطة دخول واضحتين. تتطلب الصورة متعددة المعماريات استخراج الشريحة المطلوبة صراحةً.

يجب أن يلائم الملف كله، بما فيه البيانات الوصفية والبايتات النهائية، حد `memory_limit` قبل التحليل أو النسخ. يقرأ المحمّل لقطة خاصة محدودة من ملف عادي؛ ويرفض مسارات NUL والقراءات القصيرة وتغيّر الحجم. لا يحتفظ بتعيين ملف حي. للملف ولذاكرة الضيف حدان مستقلان بالقيمة نفسها؛ ولا توفر عمليات ملفات المضيف ضمان زمن حقيقي صارماً.

تحافظ المقاطع على الصلاحيات الحالية والقصوى والملء بالأصفار. يحجز `__PAGEZERO` العناوين دون تخصيص امتداده كاملاً. تُفحص نطاقات الملف وVM ومحاذاة صفحات النظام والتداخل بعد التقريب وملكية الرأس والدخول القابل للتنفيذ والميزانية. يجب أن يكون مقطع الرأس قابلاً للقراءة والتنفيذ؛ وتبقى صفحات الحماية وبوابة العودة الخاصة محجوزة. تحتفظ آخر صفحة ملف بالبايتات حتى حد الصفحة أو EOF، وتُصفّر صفحات VM الكاملة التالية، وفق [محمّل XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/mach_loader.c).

يتلقى `LC_MAIN` كلاً من `argc` و`argv` و`envp` ومتجه apple كأربعة معاملات صحيحة؛ وتشكل البتات الثمانية الدنيا من القيمة المعادة حالة الخروج. يُقبل `/usr/lib/dyld` فقط لتسليم الدخول هذا دون imports، ولا يُشغّل dyld المضيف. تُرفض قيمة `stacksize` غير الصفرية لأن خيار المستدعي `stack_size` يملك الميزانية.

يتطلب `LC_UNIXTHREAD` سجلاً واحداً كاملاً للسجلات العامة الأصلية ذات 64 بت، مع تعيين PC فقط. يحتوي المكدس الأولي argc وargv/envp المنتهيين ومتجه apple منتهياً يتضمن `executable_path=<input filename>`. تُرفض قيم SP والأعلام المخصصة والسجلات الأخرى وflavors الإضافية ونقاط الدخول المتعارضة. لا تُورث بيئة المضيف أو متجه Linux المساعد. المرجع: [معمارية dyld](https://github.com/apple-oss-distributions/dyld/blob/main/doc/dyld4.md).

تفشل dylib الخارجية وimports وrebases/chained fixups والبواني/الهادمات ومقاطع TLS وarm64e/PAC وأنواع CPU الفرعية غير المدعومة والتشفير وأوامر التحميل غير المنمذجة قبل التنفيذ. تستخدم PIE دون fixups العناوين المفضلة، وليس ASLR. كتل التوقيع بيانات وصفية وليست تنفيذاً لـ AMFI أو سياسات الاستحقاقات.

## خدمات Darwin

يستخدم ARM64 السجلات X16 وX0–X5 والتعليمة `svc #0x80`؛ ويستخدم x64 فئة BSD وهي `0x02000000` والسجلات RAX وRDI/RSI/RDX/R10/R8/R9. يمحو النجاح carry، بينما يضبطه الخطأ ويعيد errno موجباً. يمحو ARM64 السجل X1؛ ويمحو x64 السجل RDX عند النجاح ويحافظ عليه عند الخطأ. تغييرات سجلات SYSCALL صريحة. يعبّر التقرير عن خطأ BSD بواسطة `result` و`error=true`؛ ولا تملك الطلبات التي لا تعود أو غير المدعومة هذين الحقلين. تتبع القواعد مسارات XNU لـ [ARM64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/arm/systemcalls.c) و[x64](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/dev/i386/systemcalls.c)، دون تضمين شيفرة Apple.

الخدمات هي `exit` و`write` و`getpid` و`getppid` و`getuid` و`geteuid` و`getgid` و`getegid` و`mmap` و`mprotect` و`munmap`. قيم PID/UID/GID هي 1000، وPPID هو 1. يلتقط الواصفان 1 و2 البايتات بما فيها NUL وغير UTF8؛ وتعيد الواصفات المغلقة أو المخصصة للقراءة EBADF. تُحفظ البايتات المنسوخة جزئياً لكن خطأ الوصول اللاحق يبقى EFAULT. طول أكبر من `INT_MAX` يعيد EINVAL قبل فحص الواصف أو المؤشر أو الميزانية، وفق [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).

تدعم الذاكرة تعيينات بيانات خاصة مجهولة المصدر مع `flags=0x1002` وواصف -1 وإزاحة صفر. تُقرّب الأطوال وتلميحات العناوين غير الثابتة إلى أعلى وفق صفحة النظام. إذا كان التلميح مشغولاً يُبحث أولاً باتجاه العناوين الأعلى ثم يُرجع إلى الموضع الافتراضي. يعيد mmap الخام القديم ذو الطول صفر القيمة صفر دون تخصيص؛ ويدعم العقد `MAP_UNIX03` الذي يرفض الطول صفر بخطأ EINVAL. تتطلب unmap/protect عنواناً محاذياً. تُدعم NONE/READ/WRITE ويستلزم WRITE صلاحية READ. تملك كل صفحة نظام ذاكرتها الفعلية؛ يحرر unmap الجزئي ميزانيتها وتكون الصفحات الجديدة صفراً. يترك فشل protect عبر فجوة أو خارج الصلاحيات القصوى كامل النطاق دون تغيير. المرجع: [خدمات VM في XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

تعيينات المشاركة/الثابتة/JIT والذاكرة المجهولة القابلة للتنفيذ وMach traps واستدعاءات النظام غير المباشرة والخيوط والإشارات وملفات المضيف/الشبكة وdyld وبيئات Objective-C/Swift وFoundation/UIKit خارج العقد وتوقف التنفيذ صراحةً. هذا ليس نظام Apple كاملاً ولا تطبيق iOS Simulator.

## التحقق

تُنتج عينات C المكتوبة للمشروع باستخدام Clang و`ld64.lld` دون Apple SDK أو ملفات ثنائية احتكارية. تغطي خمس تركيبات للمنصة/ISA، وسجلات Mach-O المعطوبة، وصفحات 4/16 KiB، والتحرير الجزئي عند امتلاء الميزانية. يقارن `NeverDProcessPublicTests` واجهتَي C API وCLI؛ ويتيح `NEVERD_TEST_LIBNEVERD` و`NEVERD_TEST_DARWIN_FIXTURES` التركيبات الخمس نفسها في Python.

## الملفات والواصفات الصريحة

يوفر `darwin_files` للملفات التعريفية الثلاثة فهرساً مغلقاً لملفات للقراءة فقط. يحتوي الحقل الإلزامي `files` على `path` ضيف مطلق وقياسي و`bytes_hex` بالنظام الست عشري. يحدد `stdin_hex` الاختياري إدخالاً محدوداً؛ غيابه يعني إدخالاً مجهولاً ويوقف القراءة غير الصفرية، والسلسلة الفارغة تعني EOF. يتوقف open عند غياب الفهرس، بينما يعيد المسار المطلق المفقود في الفهرس الفارغ الصريح ENOENT. لا تُستخدم ملفات المضيف أو إدخاله.

الخدمات الجديدة هي `open` و`read` و`pread` و`lseek` و`close` و`dup` و`dup2` و`fcntl`، مع مداخل nocancel لـ read/write/open/close/fcntl/pread. تدعم O_RDONLY/O_CLOEXEC وF_DUPFD وF_DUPFD_CLOEXEC وF_GETFD وF_SETFD وF_GETFL. لكل open مستقل موضعه؛ تتشارك النسخ الموضع مع أعلام close-on-exec منفصلة. لا يغير pread الموضع. يؤثر إغلاق أو استبدال 0/1/2 على العمليات اللاحقة، وتحتفظ نسخة الإخراج بوجهتها وميزانيتها.

الحدود: 256 ملفاً، و16 MiB لمجموع المسارات/NUL/الملفات/الإدخال، ومسار أقصر من 1024 بايت ومكونات حتى 255 بايت. `descriptor_limit` سقف حصري بين 3 و4096 وافتراضيه 256؛ يبقى JSON محدوداً بـ64 KiB. تُرفض الخيارات غير الصالحة قبل التحميل. تعيد read الأكبر من INT_MAX الخطأ EINVAL قبل فحص FD؛ لا يلمس EOF الوجهة ويعيد العنوان غير الصالح EFAULT. يتوقف المخزن القابل للكتابة جزئياً قبل النسخ أو تغيير الموضع. تحفظ أخطاء SET/CUR/END الموضع. الكتابة وstat القديم وseek المتناثر وبقية fcntl غير مدعومة. استخدام ملف كسلف لمسار يعيد ENOTDIR. يُقارن الكائن نفسه بنواة macOS الأصلية؛ وتغطي C/CLI/Python خمسة تراكيب للضيف، دون إثبات على جهاز iOS.

تحقق Release بتاريخ 2026-10-05: عدد التسجيلات 381، نجح 177 وتُخطي 204 دون فشل، ونُفذت الحالات الإلزامية ARM64 HVF كلها 51/51. نجحت أيضاً سبعة برامج macOS أصلية و35 اختبار C/CLI وتقارير وخمسة تراكيب Python و66 اختباراً لسكربتات التحقق. الأعداد متداخلة. لا توجد أدلة أصلية Intel HVF/KVM/WHP للخدمات الجديدة؛ يبقى Intel HVF غير متحقق منه وتظل Actions معلقة. لا يتوفر SDK iOS أو مقارنة مع جهاز فعلي.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839"}],"stdin_hex":"00ff78","descriptor_limit":32}}
```

## الأدلة والمسارات النسبية

يقبل `directories` مسار `path` مطلقاً قياسياً و`metadata` كاملة اختيارية، بما فيها الأدلة الفارغة. الجذر والأسلاف ضمنية؛ لا تنشئ البيانات الوصفية مساراً مفقوداً. mode هو `0x4000` مع الأذونات، وsize مشاهدة صريحة ضمن [0, INT64_MAX]. يجب أن يشير `working_directory` إلى دليل موجود؛ غيابه يجعل CWD مجهولاً دون وراثة المضيف. الحد 256 مساراً مصرحاً به، بما فيها أسلاف البيانات الوصفية؛ المسارات/NUL/المحتوى/الإدخال/CWD معاً 16 MiB.

تشترك `openat` (463) و`openat_nocancel` (464) و`chdir` (12) و`fchdir` (13) و`fstatat64` (470) في حل المسار. تستخدم المسارات النسبية FD دليل أو `AT_FDCWD=-2`، وتتجاهل المطلقة FD. تفحص الفواصل المتكررة و`.` و`..` والشرطة النهائية كل سلف: `/file/..` يعيد ENOTDIR و`/missing/..` يعيد ENOENT. تحفظ الأخطاء وإغلاق FD الأصلي أو إعادة استخدامه أو استبداله CWD. ينسخ `F_GETPATH=50` المسار القياسي وNUL حتى عبر dup، دون تغيير البايتات التالية.

يعيد read/pread للدليل EISDIR حتى للطول صفر؛ وتسبق إزاحة pread السالبة بخطأ EINVAL. يشترك SET/CUR في الموضع ويتطلب END حجماً صريحاً؛ يعيد mmap الخطأ EINVAL. يقبل fstatat64 الصفر و`AT_SYMLINK_NOFOLLOW=0x20` و`AT_SYMLINK_NOFOLLOW_ANY=0x800` و`AT_FDONLY=0x400` (تجاهل المسار). البتات غير الصالحة تعيد EINVAL و`AT_REALDEV=0x200` غير مدعوم. هوية التدفقات مجهولة، والأذونات ليست نموذج تحكم بالوصول؛ التعديلات ما زالت ناقصة. يقارن برنامج `directories` نفسه النواة الأصلية وخمسة ضيوف؛ ويقارن stat ملفات وأدلة حقيقية. تبقى Intel HVF Actions معلقة.

[XNU VFS](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [XNU flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/fcntl.h).

تحقق الأدلة (2026-10-05، Release): 467 تسجيلاً، 227 نجاحاً و240 تخطياً وصفر إخفاقات؛ نُفذت متطلبات ARM64 HVF كلها، 60/60. نجحت 10 برامج macOS أصلية و37 اختبار C/CLI/تقارير دون تخطٍ وخمسة ضيوف Python و66 اختبار أدوات. الأعداد متداخلة. الأدلة: `build-hvf-arm64/darwin-directory-verified-evidence/`. لم تُتحقق الخلفيات الأصلية الأخرى ولا iOS المادي.

```json
{"darwin_files":{"files":[{"path":"/work/data","bytes_hex":"3031"}],"directories":[{"path":"/work/empty"}],"working_directory":"/work"}}
```

## لقطات الأدلة الصريحة

يعدد `getdirentries64` (344) لقطة `contents` الاختيارية الثابتة لعنصر `directories` موجود؛ يستخدم C++ الحقل `DarwinFileOptions::DirectoryContents`. يجب أن تتضمن `entries` بترتيب صريح جميع الأبناء المباشرين و`.` و`..`. يبقى الدليل مجهول المحتوى دون لقطة حتى لو كان فارغاً. لا تنشئ اللقطة مسارات أو stat ولا تستعلم عن المضيف.

تتطلب كل مدخلة `name` و`inode` غير صفري و`type` (0 مجهول، 4 دليل، 8 ملف) و`next_offset` و`seek_offset`. يجب تطابق النوع مع المسار واتساق inode للمسار المحلول نفسه بين اللقطات والبيانات الوصفية. يكون `next_offset` موجباً وفريداً داخل الدليل و<=INT64_MAX، دون شرط التصاعد؛ الصفر يعيد البداية. `seek_offset` مشاهدة d_seekoff مستقلة من 64 بت غير موقعة وتسمح بتكرار الصفر. تتبع الأعداد قواعد سلاسل stat العشرية دون فقد.

يحدد `contents.minimum_buffer_size` الإلزامي الحد الأدنى للحمولة 1–128 MiB، حتى عند EOF. يضيف `minimum_buffer_size` الاختياري للمدخلة (افتراضياً 0) قيداً عند البدء منها. يسجل المثال مشاهدة APFS: 64 بايت لزوج النقطتين الأول، و1 عند EOF؛ وفي المواقع الأخرى يجب استيعاب سجل كامل. محاذاة LP64 ثمانية بايت وحجم السجل `roundUp(25 + nameBytes, 8)`. الحد الإجمالي 4096 مدخلة؛ تدخل بايتات السجلات في ميزانية 16 MiB. تحسب مسارات الأسلاف المعلنة بالبيانات/اللقطة فقط مرة واحدة ضمن 256 مساراً. يبقى JSON محدوداً بـ64 KiB.

لكل open مستقل موضعه ويشترك dup في الموضع. تستأنف القراءة من الصفر أو القيم المعلنة فقط؛ يتوقف الموضع المجهول صراحة. يعاد أكبر بادئة من السجلات الكاملة التي تتسع. عند طول >=1024 تحجز آخر أربعة بايتات مطلوبة لعلم EOF (1 في النهاية وإلا 0)؛ تقيد الحمولة فقط بـ128 MiB. يحافظ عنوان العلم على الحساب الأصلي غير الموقع بما فيه الالتفاف. الترتيب: البيانات، تقدم الموضع، نسخ الموضع السابق، ثم الأعلام. يحفظ EFAULT المتأخر الآثار السابقة؛ يتجاوز EOF نسخ البيانات الفارغة. تتوقف النسخة المنفردة القابلة للكتابة جزئياً قبل تلك النسخة مع حفظ الآثار السابقة.

يقارن `directory-entries` الحقول وdup/الإرجاع والقراءات الصغيرة وEOF وترتيب النسخ مع macOS. يقارن اختبار مستقل جميع بايتات السجلات الأصلية الملتقطة، بما فيها الأسماء الطويلة، مع SDK. لا تعيد القيم الثابتة أجيال APFS الديناميكية. تبقى `getdirentries` القديمة (196) والتعديلات والنواقل الأصلية الأخرى وiOS المادي خارج هذا التحقق.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"3031"}],"directories":[{"path":"/empty"},{"path":"/","contents":{
  "minimum_buffer_size":1,"entries":[
    {"name":".","inode":41,"type":4,"next_offset":11,"seek_offset":0,"minimum_buffer_size":64},
    {"name":"..","inode":41,"type":4,"next_offset":22,"seek_offset":0},
    {"name":"empty","inode":42,"type":4,"next_offset":7,"seek_offset":0},
    {"name":"data","inode":73,"type":8,"next_offset":99,"seek_offset":0}]}}]}}
```

[XNU getdirentries64](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/vfs/vfs_syscalls.c), [dirent ABI](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent.h), [extended flags](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/sys/dirent_private.h).

تحقق التعداد (2026-10-05، Release): 498 حالة Darwin، نجحت 246 وتجاوزت 252 بسبب غياب الخلفية، بلا إخفاق؛ نُفذت جميع حالات ARM64 HVF المطلوبة 63/63. نجحت 11 برنامج macOS أصلياً و40 فحص C/CLI/تقارير دون تجاوز، وخمس تركيبات Python بكل منها ثمانية سيناريوهات ملفات، و66 اختبار أدوات. تتداخل الأعداد. الأدلة: `build-hvf-arm64/darwin-dirents-merged-evidence/`. تبقى Intel HVF Actions معلقة؛ النواقل الأصلية الأخرى وiOS المادي غير متحقق منها.


## تعيينات الملفات الخاصة

يدعم `mmap` ملفات الكتالوج العادية مع `MAP_PRIVATE`: الرايات `flags=0x2` أو `0x40002` مع `MAP_UNIX03`، وإزاحة محاذية لصفحة النظام. حتى الطلب القصير يحفظ جميع بايتات الملف في الصفحة ويصفّر بقية صفحة EOF الأخيرة. تغيّر الكتابة الخاصة هذا التعيين فقط، دون الملف أو التعيينات الأخرى أو البيانات الوصفية الثابتة أو الموضع المشترك. يبقى التعيين بعد close وإعادة استخدام FD. تُهيّأ صفحات القراءة فقط وPROT_NONE أيضاً ويمكن لـ`mprotect` السماح بالكتابة.

تجاوز نهاية الملف حسابياً وطول UNIX03 الصفري وإزاحته غير المحاذية تعيد EINVAL قبل بحث FD؛ يعيد FD غير الصالح EBADF قبل فحص الميزانية. يفحص الطول الصفري القديم FD أيضاً. تتوقف الإزاحات القديمة غير المحاذية والتدفقات والملفات الفارغة والصفحات الكاملة بعد EOF قبل التخصيص. يسمح macOS بتعيين هذه الصفحات لكن الوصول يولد SIGBUS؛ لا يختلق النموذج صفحات صفرية قابلة للقراءة أو تسليم إشارات. تبقى التعيينات المشتركة والثابتة والتنفيذية وJIT غير مدعومة.

يحل `DarwinFiles` الواصفات والبايتات؛ ويدير `DarwinMemory` المواضع والصلاحيات والميزانية والتراجع. تأتي البيانات فقط من `darwin_files`. يفحص البرنامج المشترك `file-mapping` النسخ وclose والمواضع والأخطاء وإعادة الاستخدام المجهول؛ وتقارن تجربة أصلية منفصلة إزاحة غير صفرية وكل بايت في الصفحة وحد SIGBUS.

[XNU mmap](https://github.com/apple-oss-distributions/xnu/blob/xnu-11417.140.69/bsd/kern/kern_mman.c)

### تحقق التعيينات الخاصة، 2026-10-05

بوابة Release Darwin: 438 تسجيلاً فريداً، 210 نجاحات و228 تخطياً وصفر إخفاقات؛ نُفذت جميع متطلبات ARM64 HVF البالغ عددها 57/57 وخمسة تراكيب Unicorn. نجحت تسعة برامج macOS أصلية ومقارنة صفحة كاملة بإزاحة غير صفرية وتحقق SIGBUS في عملية فرعية معزولة. نجحت اختبارات API/التقارير الـ36 دون تخطٍ، وخمسة تراكيب Python مع `file-mapping`، و66 اختبار أدوات و38 اختبار مصدر. تتداخل الأعداد. الأدلة: `build-hvf-arm64/darwin-mmap-verified-evidence/`. لا توجد أدلة جديدة على Intel HVF/KVM/WHP أو iOS مادي؛ تبقى Intel HVF Actions معلقة.

## بيانات وصفية صريحة للملفات

يمكن أن يتضمن الملف `metadata`، وعندها تكون جميع الحقول أدناه مطلوبة. تحفظ السلاسل العشرية عرض العدد كاملاً؛ أرقام JSON محصورة بالأعداد الصحيحة الدقيقة ضمن ±(2^53−1). device عدد موقّع من 32 بت، وmode/link_count غير موقّعين من 16 بت، وinode غير موقّع من 64 بت، وuid/gid/flags/generation غير موقّعة من 32 بت. يجب أن يساوي size عدد البايتات، وألا يتجاوز blocks الحد الموقّع من 64 بت، وأن يكون block_size ضمن المجال الموقّع غير السالب من 32 بت. تستخدم الأوقات ثواني موقّعة من 64 بت و0–999999999 نانوثانية.

تعيد `stat64` (338) و`fstat64` (339) و`lstat64` (340) سجل LP64 نفسه من 144 بايت على ARM64/x64. تشترك مع open في حل المسارات وتحترم dup/close دون تخصيص FD أو تغيير الموضع. تكون rdev والحشو والحقول المحجوزة صفراً. المشاهدات ثابتة: لا يحدّث read الأوقات ولا يغيّر mode إذن الوصول إلى الدليل. تبقى البيانات المفقودة وحالة التدفقات والروابط الرمزية وstat القديم والأمن الموسع غير مدعومة. تسبق أخطاء المسار/FD فحص مؤشر الإخراج؛ يُرفض الإخراج القابل للكتابة جزئياً قبل أي كتابة. يقارن الاختبار الأصلي كل بايت بملف حقيقي وتخطيط SDK، ويتحقق البرنامج المؤلف نفسه من الاستدعاءات الثلاثة.

```json
{"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":{
  "device":1,"inode":"18364758544493064720","mode":33188,"link_count":1,
  "uid":1000,"gid":1000,"size":10,"block_size":4096,"blocks":8,
  "flags":0,"generation":0,
  "access_time":{"seconds":-1,"nanoseconds":1},
  "modification_time":{"seconds":2,"nanoseconds":3},
  "change_time":{"seconds":4,"nanoseconds":5},
  "birth_time":{"seconds":6,"nanoseconds":7}
}}]}}
```

[XNU stat.h](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/sys/stat.h)

### تحقق البيانات الوصفية والخطوات التالية (2026-10-05)

بعد stat64: عدد التسجيلات الفريدة 409؛ نجح 193 وتُخطي 216 دون فشل. نُفذت حالات ARM64 HVF الإلزامية 54/54 وخمسة تراكيب Unicorn. نجحت مقارنة SDK والسجل الحقيقي، وثمانية برامج أصلية، و36 حالة API/تقارير دون تخطٍ، وخمسة تراكيب Python و66 اختبار أدوات؛ تتداخل الأعداد. لكل حالة أصلية ملف إخراج مستقل لمنع بقاء بايتات قديمة بعد إخراج أقصر. لا توجد أدلة أصلية للإضافات على Intel HVF/KVM/WHP أو iOS مادي.

التالي: التعيينات المشتركة وأخطاء صفحات EOF والكتابة المحدودة (صفحات EOF والعمر بعد close وترتيب الأخطاء)، ومشاهدات الوقت/النظام الصريحة وخدمات Mach/الخيوط اللازمة، واعتماديات Mach-O وrebases/binds والتهيئة وTLS. يحتاج Objective-C/Swift وFoundation/UIKit برامج مرجعية أصلية. يحتاج iOS المادي SDK وجهازاً؛ ما زال Intel HVF غير موثق وتبقى Actions الخاصة به معلقة.



```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

يشترط فحص الأعباء المستقل جميع الحالات الأصلية، وعددها 60 على ARM64 أو 40 على x64، بما فيها `LC_MAIN` و`LC_UNIXTHREAD` على كل منصة. غياب حالة إلزامية أو تخطيها أو غياب `ld64.lld` يؤدي إلى الفشل.

```sh
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/darwin-workload-evidence --require-darwin-backend hvf
```

استخدم `kvm` على Linux أو `whp` على Windows. يختبر [سير عمل Darwin](../../.github/workflows/darwin-native.yml) واجهتَي x64 دون Unicorn، ويدعم إعادة كل منهما منفردة. يشغّل [مرجع النواة](../../.github/workflows/darwin-kernel-reference.yml) البرامج مباشرةً على معماريتَي macOS دون NeverD/LLVM. يحدد `DarwinNativeCases.def` الأوضاع وحالات الخروج والبايتات المتوقعة. مرجع المضيف وحده يربط libSystem للدخول الحقيقي عبر dyld. عدم تطابق ISA أو Rosetta أو انتهاء المهلة أو اختلاف النتائج يسبب الفشل؛ وهذه ليست أدلة على نواة جهاز iOS.

## الأدلة والنطاق المتبقي

النتائج بتاريخ 2026-10-03؛ لا تجمع الصفوف المتداخلة:

| النقل | المصدر | نجح | فشل | تم تخطيه | الأعباء الأصلية |
| --- | --- | ---: | ---: | ---: | ---: |
| ARM64 HVF | `defc93928` | 65 | 0 | 221 | 39/39 |
| Intel HVF | `8dcc74c59` | 52 | 0 | 234 | 26/26 |
| x64 KVM | `36e11ca8a` | 51 | 0 | 235 | 26/26 |
| x64 WHP | `36e11ca8a` | 51 | 0 | 235 | 26/26 |

يطابق [تشغيل Intel](https://github.com/NeverSight/NeverD/actions/runs/37106013999) عدد 286 هوية CTest و32 عملية مع XML الأصلي. حالات التخطي البالغ عددها 234 هي 65 حالة Unicorn معطلة و39 ضيف ARM64 و130 منصة مضيفة أخرى. جرى التحقق من SHA-256 للأثر `11267489438` وهو `cd8fabbd7d031ac4ad7b891b8e5a52f3e3abe3c39306d9c4a1893e40912e78ef`. تحققت أيضاً نتائج [KVM/WHP](https://github.com/NeverSight/NeverD/actions/runs/37062839703) بصورة مستقلة. نجح [مرجع النواة](https://github.com/NeverSight/NeverD/actions/runs/37064795867) في 4/4 برامج لكل ISA، بحالة خروج 37 وخرج مطابق تماماً وstderr فارغ.

مع Unicorn، نجح 138 فحصاً لـ C API/CLI وتُخطي 156 دون فشل. يغطي Python التركيبات الخمس؛ ويطابق المحرك المعبأ 18 تقرير CLI على ARM64 مع تحقق توقيع 186 صورة Mach-O. عند تعطيل HVF/Unicorn نجح 38 فحصاً وتُخطي 231، دون ربط Hypervisor.framework. هذه أدلة تكامل وليست عمليات تنفيذ أصلية إضافية. ما زال فحص CPU Intel الكامل غير مكتمل؛ انظر [HVF](macos-hvf.md) و[السجل التفصيلي](../darwin-emulation.md#hosted-native-verification-2026-10-03).
