**اللغات**: [English](../darwin-emulation.md) | [简体中文](../zh-CN/darwin-emulation.md) | [繁體中文](../zh-TW/darwin-emulation.md) | [日本語](../ja/darwin-emulation.md) | [한국어](../ko/darwin-emulation.md) | [Français](../fr/darwin-emulation.md) | [Deutsch](../de/darwin-emulation.md) | [Español](../es/darwin-emulation.md) | [Italiano](../it/darwin-emulation.md) | [Русский](../ru/darwin-emulation.md) | [العربية](darwin-emulation.md)

<!-- i18n-source: b3b4285b341fef4afed8cfe49f7fe8396536b9e63f924d14402ec7d0d3eb0b01 -->

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

الخدمات هي `exit` و`write` و`getpid` و`getppid` و`getuid` و`geteuid` و`getgid` و`getegid` و`mmap` و`mprotect` و`munmap`. قيم PID/UID/GID هي 1000، وPPID هو 1. يلتقط الواصفان 1 و2 البايتات بما فيها NUL وغير UTF8؛ وتعيد الواصفات الأخرى EBADF. تُحفظ البايتات المنسوخة جزئياً لكن خطأ الوصول اللاحق يبقى EFAULT. طول أكبر من `INT_MAX` يعيد EINVAL قبل فحص الواصف أو المؤشر أو الميزانية، وفق [XNU write](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/sys_generic.c).

تدعم الذاكرة تعيينات بيانات خاصة مجهولة المصدر مع `flags=0x1002` وواصف -1 وإزاحة صفر. تُقرّب الأطوال وتلميحات العناوين غير الثابتة إلى أعلى وفق صفحة النظام. إذا كان التلميح مشغولاً يُبحث أولاً باتجاه العناوين الأعلى ثم يُرجع إلى الموضع الافتراضي. يعيد mmap الخام القديم ذو الطول صفر القيمة صفر دون تخصيص؛ ولا يشمل العقد `MAP_UNIX03`. تتطلب unmap/protect عنواناً محاذياً. تُدعم NONE/READ/WRITE ويستلزم WRITE صلاحية READ. تملك كل صفحة نظام ذاكرتها الفعلية؛ يحرر unmap الجزئي ميزانيتها وتكون الصفحات الجديدة صفراً. يترك فشل protect عبر فجوة أو خارج الصلاحيات القصوى كامل النطاق دون تغيير. المرجع: [خدمات VM في XNU](https://github.com/apple-oss-distributions/xnu/blob/f6217f891ac0bb64f3d375211650a4c1ff8ca1ea/bsd/kern/kern_mman.c).

تعيينات الملفات/المشاركة/الثابتة/JIT والذاكرة المجهولة القابلة للتنفيذ وMach traps واستدعاءات النظام غير المباشرة والخيوط والإشارات والملفات/الشبكة وdyld وبيئات Objective-C/Swift وFoundation/UIKit خارج العقد وتوقف التنفيذ صراحةً. هذا ليس نظام Apple كاملاً ولا تطبيق iOS Simulator.

## التحقق

تُنتج عينات C المكتوبة للمشروع باستخدام Clang و`ld64.lld` دون Apple SDK أو ملفات ثنائية احتكارية. تغطي خمس تركيبات للمنصة/ISA، وسجلات Mach-O المعطوبة، وصفحات 4/16 KiB، والتحرير الجزئي عند امتلاء الميزانية. يقارن `NeverDProcessPublicTests` واجهتَي C API وCLI؛ ويتيح `NEVERD_TEST_LIBNEVERD` و`NEVERD_TEST_DARWIN_FIXTURES` التركيبات الخمس نفسها في Python.

```sh
cmake --build build-hvf --target NeverDDarwinProcessTests NeverDProcessPublicTests --parallel 8
ctest --test-dir build-hvf -L '^(NeverDDarwinProcessTests|NeverDProcessPublicTests)$' --output-on-failure
python3 scripts/run_native_cpu_ci.py --build build-hvf-native \
  --evidence build-hvf-native/native-evidence --require-hvf
```

يشترط فحص الأعباء المستقل جميع الحالات الأصلية، وعددها 39 على ARM64 أو 26 على x64، بما فيها `LC_MAIN` و`LC_UNIXTHREAD` على كل منصة. غياب حالة إلزامية أو تخطيها أو غياب `ld64.lld` يؤدي إلى الفشل.

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
