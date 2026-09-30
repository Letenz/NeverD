**اللغات**: [English](../testing.md) | [简体中文](../zh-CN/testing.md) | [繁體中文](../zh-TW/testing.md) | [日本語](../ja/testing.md) | [한국어](../ko/testing.md) | [Français](../fr/testing.md) | [Deutsch](../de/testing.md) | [Español](../es/testing.md) | [Italiano](../it/testing.md) | [Русский](../ru/testing.md) | [العربية](testing.md)

[← فهرس التوثيق](README.md)

# اختبار NeverD

تجيب اختبارات NeverD عن ثلاثة أسئلة مختلفة: هل للتمثيل الشكل المتوقع، وهل يعمل
مسار pipeline كامل مع fixture ثنائية، وهل تحافظ الشفرة المولدة على السلوك؟ اختر
أصغر مجموعة تجيب عن سؤال التغيير، ثم شغّل التجميع الأوسع قبل طلب سحب عالي المخاطر.

## تهيئة بناء الاختبار

تكون الاختبارات معطلة ما لم يُفعّل `BUILD_TESTING`. يمثل Release الخيار العادي
للمجموعة الكاملة؛ يحتفظ Debug بالتأكيدات والتتبع، لكنه غير محسّن عمدًا ولا يمثل
معايير decode.

```bash
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON
cmake --build build-release --parallel 4
```

تحتاج مجموعة fixtures الكاملة إلى `clang` للتجميع العابر للأهداف وإلى linker
الخاصة بـ LLVM ‏(`ld.lld` و`lld-link`) في `PATH`. يبني CMake كثيرًا من fixtures
القابلة لإعادة التموضع دائمًا، وfixtures ‏ELF/PE المرتبطة عند توفر linker المطابق.
الاختبار المتخطى لأن المضيف لا يستطيع تجميع fixture أو ربطها هو تغطية غير
منفذة، وليس نجاحًا للهدف.

راجع [CONTRIBUTING.md](CONTRIBUTING.md) للاستنساخ وملفات البناء وLLVM
الجاهز على macOS.

## فحوص استعادة المفسّر

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

تغطي اختبارات API افتراضيات v1/v2/v3 والميزانيات الصريحة والبنى المبتورة وكل حقول reserved والذيول المستقبلية. تتحقق اختبارات CLI من نفاد حدود الحقول والاستعلامات ومن نجاح الاستعادة عبر واجهتي ABI وخلفيتي المصدر، وترفض الحدود العشرية غير الصالحة وتتطلب `--devirtualize`. يجب ألا ينشر التشغيل المستنفد مصدرًا أو رسمًا متبقيًا جزئيًا.

تختبر `NeverDLowIRRefinementTests` الرسوم المستعادة فعلياً والحلقات المنتهية ذات البنى المختلفة وحالات عدم التكرار والمنتجين الديناميكيين والاختيارات المشروطة وتداخل مدخلات السجلات وارتباط النسخ والحفظ وأدلة القراءات الثابتة في الجانبين وأعلام النظام وحفظ خانة العودة. يجب رفض الشهادة للمرشحات الخاطئة والكتابات الإضافية والمسارات الناقصة أو اللانهائية والأدلة القديمة وتداخل المؤقتات ونفاد الميزانيات المشتركة. وتستمر اختبارات الاستقلال السابقة في رفض القيم الاعتباطية القابلة للملاحظة.

تغطي `LowIRLoopRefinement.*` و`BinaryLowIRLoopRefinement.*` ضمن الهدف نفسه عدادات 64 بت اعتباطية، وترتيبًا معجميًا لحلقات متداخلة، وكودًا متبقيًا من تعليمات أصلية فعلية، وقوالب بادئات الدخول، والمشاهد المتداخلة، والحفظ المترابط. ترفض الاختبارات السلبية الأجسام الخاطئة وتضييق مجال الدخول والترتيب غير المتناقص والالتفاف غير الموقّع ونسيان الكتابات السابقة ونقاط القطع المفقودة والقوالب المشوهة ونفاد الميزانيات المشتركة. لا يجيز نجاح فرع محدود إثباتًا استقرائيًا غير مكتمل.

تستخدم `LowIRLoopInference.*` و`BinaryLowIRLoopInference.*` عدادات وعمليات حفظ على المكدس وعودة مبكرة واستدعاءات أصلية وأعلاماً مجمعة مكتوبة بصورة مستقلة. تشمل الاختبارات توسيع الحساب ضيق العرض وأعلاماً متساوية دلالياً ذات تعبيرات مختلفة. يجب ألا تنتج الرسوم المشوهة والمصادر المفقودة أو المزورة والحلقات غير المنتهية أو الملتفة ونفاد ميزانيات الاستنتاج أو البرهان أي شهادة.

تختبر `LowIRLoopPlanPairing.*` في الهدف نفسه إعادة تسمية السجلات واختلاف العمليات الحسابية ولقطات البادئة الخاصة بكل طرف والاحتفاظ بالشرطين ومدخلات ذاكرة الإطار المشتركة وتغطية نقاط القطع المتداخلة وميزانيات الإثبات المستقلة. يجب ألا تنتج شهادة عند غياب العلاقات أو خطأ الكتابات أو فساد ربط القيم المؤقتة أو نقص الإقران أو نفاد حدود البيانات الوصفية.

تختبر مخارج المساواة المخزّنة في حلقات من مستويين وثلاثة مستويات ترابط المعاملات وتغيّر الحدود وإعادة تعيين العدّادات وفساد النسخ.

تغطي اختبارات المقارنات المخزنة المساواة وعدم المساواة وشروط الدخول والتهيئة الثابتة والحقول التي تظهر بعد التوسيع والبتات 7/31/63 في حقول من 1/4/8 بايت. يجب رفض تغيير بت مجاور فقط مع الإبقاء على البت المختبر عبر مقارنة الحالة الكاملة. ترفض الخطوات الصفرية والحدود المتحركة وإعادة ضبط العداد ونفاد الميزانيات المشتركة.

تغطي اختبارات التعميم المداخل المندمجة والشاهد الأول بلا تكرار والفروق الخفية في السجلات والإطار والشروط المنطقية غير المعيارية وشروط المصائد الأصلية وعمليات الحفظ المترابطة والخطط غير الصحيحة أو المستنفدة للميزانية. تختبر عدّادات مستقلة بمستويين أو ثلاثة وخروج بالمساواة، مع بايتات أصلية، الحدود غير الموقعة ومجالات الصفر/القيمة القصوى والخطوات غير الأحادية والتعليمات الأصلية الخاطئة. يجب أن يرفض الاستدلال والإثبات النهائي النتائج غير الكاملة.

تغطي اختبارات انحدار مستقلة للحلقات البديلة اتجاهي التفرع والأجسام الخاطئة وفرعًا مجاورًا لا ينتهي ونفاد ميزانيات البحث/الإثبات المشتركة. `LowIRLoopInference.AlternativeLoopsReachBothPrefixesWithinSharedBudgets`.

تغطي اختبارات الاستنتاج مستويين وثلاثة مستويات متداخلة، وعدادات متزايدة ومتناقصة، ومراحل مستنتجة، ونقاط قطع في أجسام حلقات أصلية فعلية. يجب رفض مجالات البادئة غير القابلة للوصول أو المنفصلة والأجسام الخاطئة والانتقالات اللانهائية أو الملتفة ونفاد ميزانيات البحث والإثبات المشتركة. لا يحل شاهد البادئة محل التغطية الكاملة للمقاطع.

```sh
cmake --build build-release --target NeverDLowIRRefinementTests --parallel 4
build-release/bin/NeverDLowIRRefinementTests
```

يتحقق `NeverDLowIRUndefinedIndependenceTests` من استقلال تنفيذين لرسم LowIR كامل وخالٍ من الدورات. يشترك التنفيذان في مدخلات الدخول العادية، وتحافظ كل قيمة جديدة غير معرّفة معماريًا على ترابطها عبر النسخ والكتابات المتداخلة والحفظ في المكدس وإعادة التحميل. تُفحص شروط التحكم قبل إضافة افتراضات المسار. تتطلب الشهادة بيانات وصفية للتأثيرات بحالة `Complete` مرتبطة بدقة بحدود كل تعليمة كاملة وبصمة عملياتها. تُرفض الشهادة عند نقص الأدلة أو وجود حلقات قابلة للوصول أو استدعاءات أو تداخلات عناوين مجهولة أو نفاد الميزانية. يقتصر الاستنتاج على عناصر الملاحظة الصريحة وعقد الوصول إلى إطار المكدس دون أخطاء؛ وهو ليس برهان تكافؤ كامل من الشيفرة الأصلية إلى C.

يستخدم `NeverDOriginalBinaryUndefinedIndependenceTests` بايتات x64 مستقلة ذات خرائط ثابتة لاختبار CALL/RET الفعليين، وأهداف الرجوع المعدلة، والتعداد الكامل للأهداف غير المباشرة المحدودة، والقراءات الثابتة. يتحقق الهدف نفسه من جمع الفروع المباشرة كاملة، والربط الدقيق للبايتات والتأثيرات والخرائط وشواهد القراءة، وحفظ RSP وخانة الرجوع عند الرجوع الخارجي، وانفصال الإطار عن الصورة. يجب رفض التعليمات المفقودة أو المتداخلة والفروع غير المدققة خارج قواعد الفخاخ الدقيقة والإسقاط ذي الملف الصريح والحلقات غير المنتهية أو المتجاوزة للميزانية والتعداد الناقص وعدم تطابق ملفات التنفيذ والعقود ونفاد الميزانيات، دون شهادة أو شيفرة متبقية. يتطلب النجاح اكتمال كل مسار أصلي قابل للتنفيذ. لا يعتمد هذا الشرط الاختياري ثوابت الحلقات أو الاستثناءات أو التنفيذ مع CET أو تكافؤ الشيفرة الأصلية مع C؛ وتظل الاستعادة العادية منفصلة. يتحقق الهدف أيضًا من الحدود النهائية لتعليمات `INT3`/`UD2` المرفوعة بصرامة ومن ربط جميع بايتاتها وبصمات عملياتها. يجب أن تبقى البيانات المرافقة للمخرجات غير المعرّفة ذات الحالة `Missing` كما هي؛ ولا تظهر في الشهادة إلا المصائد التي يثبت التنفيذ الرمزي تعذّر الوصول إليها، بينما يجب أن يعيد كل مسار قابل للتنفيذ يصل إلى مصيدة `ContractViolation` دون شهادة أو شيفرة متبقية. لا يُمثّل الاستمرار بعد المصائد أو العودة من الاستثناءات، ولا يُستخدم `codeFollowsTrap`، ويبقى دعم واجهة LowIR الثابتة دون تغيير.

تغطي اختبارات الأعلام المجمعة جميع مجموعات أعلام الدخول القياسية وأقنعة الامتياز وشرط TF/AC في التنفيذين والمنتجين غير المحددين المنفصلين والنسخ المترابطة والاستدعاءات الأصلية وحالة الفروع الشقيقة والمراقبة النهائية الإلزامية والأدلة المشوهة وحدود الموارد. يجب أن تنتهي جميع مسارات الإدخال الممكنة للحلقات المحدودة؛ لا يخفي فرع آمن مسارًا لا نهائيًا أو مبتورًا. تشمل اختبارات RDSSPD/RDSSPQ السجلات العامة الستة عشر والعرضين وحفظ البتات العليا وأدلة `Missing` ورفض الإسقاطات المزورة. تقارن اختبارات حالة الآلة مساري C عند O0/O2 مع فخاخ السلوك غير المحدد بمرجع مستقل لأعلام وضع المستخدم، وتتحقق من بقاء حالة فشل الملف مسجلة. تغطي اختبارات INCSSPD/INCSSPQ العرضين وجميع السجلات العامة وحفظ الحدود غير القابلة للوصول والفخاخ الممكنة بعد اكتمال فرع شقيق والمعاملات الصفرية وأدلة الفخاخ المزورة.

يتحقق `NeverDX86UndefinedEffectsTests` من بيانات البتات غير المعرّفة والأعلام المعرّفة أو المحفوظة ورفض الشهادات القديمة. يقارن `NeverDX86CarryArithmeticFlagTests` الحمل المساعد في ADC/SBB لصيغ السجلات والذاكرة بمرجع حسابي. ويتحقق `NeverDX86LogicIdentityTests` من أن AND بمعاملين متطابقين ما زال يصفر البتات 63:32 من السجل المقابل ذي 64 بت عند الكتابة إلى وجهة من 32 بت في نمط 64 بت، مع حفظ البتات التي لا تشملها الكتابات الأضيق.

تغطي اختبارات الإزاحة جميع الأعداد الخام ذات الثمانية بتات، وتركيبات الأعلام عند الصفر، ونمطي x86، وكل العروض، وتداخل CL، وAH/CH/DH/BH، والسجلات الممتدة والذاكرة. يُقارن التنفيذ الرمزي الدقيق بالبايت بحساب متكرر لبت واحد. تختبر العلاقات الأعلام المنسوخة والجديدة والحفظ والعودة إلى الحلقات والأعداد المشتقة من قيم غير محددة والفروع والترميزات غير الصالحة والبصمات والميزانيات. تشمل القراءات المنتهية 1/2/4/8 بايت، والاختيار حسب المدخلات، وعنواناً وحيداً لكل مسار، والأدلة الكاملة والحدود؛ وترفض المرشحين المعتمدين أو المفقودين أو القابلين للكتابة أو غير المسندين بملف أو المعاد توطينهم أو غير المحدودين.

تفحص اختبارات النواة فصل السياقات والدمج عند نقطة ثابتة والحلقات الديناميكية والسجلات المتداخلة وإبطال معلومات التداخل في الذاكرة والتوزيع المنتهي والرفض دون بديل جزئي. تجمع اختبارات المصدر آلات x64 أصلية تعتمد على السجلات أو المكدس أو العناوين المنتهية؛ وتشمل حقول تحكم مترابطة ومرجعًا أصليًا مستقلاً لاتفاقيتي SysV وWin64. يُترجَم مسارا C عند O0/O2 مع مصائد السلوك غير المعرّف وتُقارَن نتائجهما بمرجع للحساب غير الموقّع والكتابة إلى الذاكرة وقيم حراسة المخرجات. تفحص الحالات السلبية الشهادات الناقصة والميزانيات غير الكافية. وتفحص واجهة CLI العامة وتقاريرها عناصر التحكم والميزانيات والعدادات والرفض. يلزم Clang متعدد الأهداف وLLD؛ ويتطلب تنفيذ ELF الأصلي أيضًا مضيف Linux x64. غياب أداة أو عدم توافق المضيف يعني تخطي التغطية، وليس نجاحها.

تغطي اختبارات `ControlStateRecovery.LongTransparentLoop*` حلقة مستقلة التأليف من 20 مرحلة، ومراجع حسابية ديناميكية، ورفض المحددات المجهولة، ونفاد الميزانية. ويتحقق `LongTransparentPhasesKeepExactBitDemands` من بقاء البتات غير ذات الصلة في بايت المحدد بيانات قابلة للملاحظة وقت التشغيل، دون تحويلها إلى طلبات تحكم. ويتحقق `ProducerClosureChargesWorkBeforeAnotherRestart` من احتساب الاكتشاف العكسي وإعادة التقييم ضمن الميزانيات المشتركة قبل بدء رسم جديد، دون نشر نتائج جزئية.

يتحقق `X86ShiftCarry.*` من حمل الإزاحة الحسابية اليمنى للأعداد الضيقة، والعدّاد
بعد القناع، وسجلات الوجهة وكبت الأعلام في APX، بالمقارنة مع إزاحات متتالية من بت واحد.
يشغّل `NarrowArithmeticShiftCarrySurvivesBothSourceBackends` ناتجَي C عند O0/O2
مع مصائد السلوك غير المعرّف، ويغطي جميع قيم البايت والعدّادات الأصلية.
كما يشغّل `NeverDLLVMCIntrinsicSemanticTests` عمليات min/max الصحيحة الموقّعة
وغير الموقّعة بعروض i1/8/16/32/64/128 عند O0/O2، ويتحقق من النتائج المسندة
والمضمّنة وترتيب إنتاج المعاملات وتقييمها مرة واحدة. يجب رفض العروض القياسية
غير المدعومة والمعاملات غير السليمة صراحةً.

## فحوص تدفق التحكم والاستدعاءات في C المهيكل

تتحقق `HighControlFlowSemantics.*` من حفظ التسميات التي تشير إليها قفزات أخرى عند نقل مخارج الحلقات أو أجزائها اللاحقة. تشمل الاختبارات الدخول المباشر إلى مخارج بداية الحلقة ونهايتها واستبدال break، وتشغّل C المولّد عند O0/O2 مع قيم إرجاع متوقعة مستقلة.

تتحقق `HighCPointerAddresses.Required*` / `UnknownConditionsFailOnlyWhenRead` من حفظ الخانات الأخيرة المجهولة لوسائط السجلات المطلوبة المستنتجة. يجب أن يؤدي تقييم وسيط مطلوب مجهول أو شرط مجهول إلى مصيدة صريحة؛ ولا يجوز تحويل المعاملات المحذوفة أو الفارغة أو المتداخلة إلى صفر بصمت. تظل القيم المعروفة والمعاملات الإضافية التي ثبت عدم قراءتها قابلة للتنفيذ. المصيدة حد تشخيصي وليست دليلاً على تكافؤ السلوك المستعاد.

## اختبارات تنفيذ CPU

تبني `NeverDIntegerABITests` عينات أصلية من Clang لـWindows x64 وLinux x64 وLinux ARM64، وتختبر دوالاً ذات عشرة وسائط حقيقية عبر السجلات والمكدس وإطارات الاستدعاء. تغطي مصفوفة Unicorn/KVM/WHP؛ وتُسجّل تركيبات المضيف/ISA غير المتاحة كتخطٍ صريح، لا كنجاح. يفحص `NeverDExecutionBudgetTests` ميزانية الاستمرار المشتركة والموعد المطلق من دون نوم معتمد على التوقيت.

تغطي `NeverDCPUEmulationTests` تعليمات ARM64 والتحكم والتحميلات والسياقات والاسماء المستعارة وإبطال cache والحلقات المحدودة؛ ينفذ الملف البرمجي كذلك FP/SIMD وTLS. تختبر `NeverDUserExecutionTests` صلاحيات صفحات CPL3/EL0 والاسماء المستعارة واسترداد أخطاء الحماية والسياقات وتبديل فضاء العناوين. تثبت `NeverDServiceRequestTests` أن SYSCALL/SVC يعترضان قبل دخول النقل، ويحفظان الحالة ويمنعان التعديل حتى استهلاك الطلب مرة واحدة؛ هذا بروتوكول تسليم وليس نظام خدمات OS كاملاً. وتفحص `NeverDExecutionConfigurationTests` التحقق المشترك بين المصنع والتقرير، والتمييز بين دعم البناء وفحص المضيف المباشر، وفشل التركيبات غير المدعومة قبل التعديل. الاختبارات العامة لـSDK/CLI لا تتطلب نموذج Windows. تختبر `NeverDThreadPointerTests` FS-base و`TPIDR_EL0` واستعادة السياق والصلاحيات. تستخدم `NeverDKvmCancellationTests` ضيف x64 لا ينتهي للتحقق من إيقاف KVM النشط واستئنافه وحالة إشارات التطبيق؛ تُتخطى صراحةً عند غياب KVM.

```bash
cmake --build build-cpu --target NeverDKvmRunTests NeverDKvmCancellationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverDKvm(Run|Cancellation)Tests$' --output-on-failure
```

```bash
cmake --build build-cpu --target NeverDIntegerABITests NeverDExecutionBudgetTests NeverDCPUEmulationTests NeverDUserExecutionTests NeverDServiceRequestTests NeverDExecutionConfigurationTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(IntegerABI|ExecutionBudget|CPUEmulation|UserExecution|ServiceRequest|ExecutionConfiguration)Tests$' --output-on-failure
```

الغياب عن مضيف ARM64 أو عن المحاكي الافتراضي تخطٍ للتغطية الأصلية وليس نجاحاً؛ Unicorn والتجميع المتقاطع لا يثبتان تنفيذ KVM/WHP أصلياً.

## اختبارات ملف عمليات Linux

تجمع مجموعة [محاكاة العمليات](process-emulation.md#التحقق) ملفات ELF حقيقية لـx64/AArch64. يتحقق `NeverDLinuxProcessTests` من البدء وسياسة program headers وطلبات الخدمة والمخرجات الثنائية والأعطال والموارد. يختبر `NeverDProcessPublicTests` C API وCLI دون تغيير صورة التحليل. ويغطي `NeverDExecutionSessionTests` CPUين يشتركان في الذاكرة والميزانية واستهلاك الطلب/العطل مرة واحدة؛ أما `NeverDX64MemoryUpdateTests` فيتحقق من حسابات الذاكرة وSETcc وBT وXMM/MXCSR ومراقبي الكتابة وحدود REP والقراءة المحضرة للأجهزة. وتعيد `DriverBackendParityTests.cpp` تشغيل صور WDK الأصلية والمعاد تموضعها وتقارن التقرير المرصود بالكامل بـUnicorn؛ الحالات والأجهزة غير المتاحة تُتخطى صراحة.

يقبل x64 المفحوص أيضاً الأشكال القديمة المقنّعة `SS` و`SD` و`PS` و`PD` للتعليمات `ADD` و`SUB` و`MUL` و`DIV` و`SQRT` و`MIN` و`MAX`. يوحّد `X64SSEInstructions.def` عروض المعاملات والمحاذاة وقواعد القبول. يقارن `MaskedSSEArithmeticMatchesIndependentHostExecution` أشكال السجلات وRAM بمرجع مستقل على CPU المضيف، شاملاً أوضاع التقريب الأربعة وFTZ والأصفار الموقّعة والمدخلات دون الطبيعية وNaN. ويتحقق `SSEMemoryObserverStopsBeforeResultAndStatusChanges` من التوقف قبل الآثار. تبقى DAZ والاستثناءات غير المقنّعة وx87 وAVX غير مقبولة.

```bash
cmake --build build-cpu --target NeverDLinuxProcessTests NeverDExecutionSessionTests NeverDX64MemoryUpdateTests NeverDThreadPointerTests --parallel 4
ctest --test-dir build-cpu -L '^NeverD(LinuxProcess|ExecutionSession|X64MemoryUpdate|ThreadPointer)Tests$' --output-on-failure
```

عدم توفر المحرك يسجّل كتخطٍ. اجتياز Unicorn ARM64 أو التجميع المتقاطع لا يمثل دليلاً على KVM/WHP أصلي.

## فحوص محاكاة برامج التشغيل

فعّل `NEVERD_ENABLE_DRIVER_EMULATION=ON` مع `BUILD_TESTING=ON` لبناء مجموعة اختبارات التنفيذ المركّزة وفحوص واجهة C API/CLI المشتركة:

```bash
cmake --build build-release --target \
  NeverDDriverEmulationTests NeverDDriverEmulationPublicTests --parallel 4
ctest --test-dir build-release -L '^NeverDDriverEmulation' --output-on-failure
```

تختبر حالات الاختبار تهيئة الضيف والعودة الناجحة والفاشلة والسلوك غير المدعوم وأعطال الذاكرة والتحليل الصارم للسيناريو والتنفيذ المحدود، وكذلك الإدخال/الإخراج المتزامن buffered/direct وREAD/WRITE وأعمار الملفات المستقلة وصلاحيات MDL وحل الصادرات الديناميكي ووسائط الضيف المتغيرة وأعطال المعالج المنظمة عبر create وعمليات النقل وcleanup وclose وunload. استخدم [واجهة `emulate-driver` لسطر الأوامر](driver-emulation.md) للتحقق من JSON ورموز خروج العملية. يمكن لبنى الإنتاج تفعيل هذه الميزة مع `BUILD_TESTING=OFF`؛ ويجب ألا تتطلب `libneverd` إعداد Unicorn المخصص للاختبارات وحدها.

تغطي الاختبارات الإضافية واصفات MDL المملوكة لبرنامج التشغيل لمخزن الذاكرة غير القابل للترحيل، والعمر المستقل للواصف والمخزن المؤقت، وتخطيطات استعلام السجل والمخازن القصيرة، وصلاحيات المقابض وحذفها وتسربها، وجميع بتات `information_hex` البالغ عددها 64 لطلبات IOCTL بلا خرج. يشمل التحقق الخارجي أيضًا القراءة والكتابة المباشرتين المتزامنتين واستعلامات الإحصاءات في Zero.

تتحقق الاختبارات من سياقات CPU الكاملة (السجلات والأعلام وSIMD وFPU وCR8) والذاكرة المشتركة ورفض السياقات الأجنبية أو بعد العطل. تنفذ عينات `driver_dispatcher.c` المترجمة استدعاءات DPC وعمل فعلية ومواعيد المؤقتات وأحداث/مؤقتات الإشعار والمزامنة وانتظار `KernelMode` غير القابل للتنبيه بسبب `Executive` والمهلة/التأخير ومكدسات محجوبة متعددة وحفظ الاستيقاظ بعد set/reset والوسائط وأخطاء IRQL/العمر. تبقى تغطية التعليق/الإكمال والطوابير والتعطل والميزانيات المشتركة. يثبت ذلك المجموعة الموثقة ولا يعني دعم Windows غير المتزامن بالكامل.

`driver_context_limits.c`: تأتي حدود IRQL من `KernelAPIIRQL.def` وتتحقق النماذج المسؤولة من الشروط المعتمدة على الوسائط. لا يمكن لـDPC استدعاء واجهات السجل أو تخصيص المخزن المقسّم إلى صفحات أو تحريره أو الوصول إليه. تتطلب تحويلات Unicode في `DbgPrint` مستوى `PASSIVE_LEVEL`، بينما يبقى إخراج ANSI والعمليات غير القابلة للترحيل المدعومة متاحة عند `DISPATCH_LEVEL`. للمكدسات حدود؛ فلا يدخل مؤشر مكدس خارج النطاق مكدس عامل آخر محجوب. تمنع المؤقتات الفعالة في امتداد الجهاز تحريره المبكر. لا تتيح هذه الفحوص تغييرات IRQL العامة.

يتحقق `KernelDeviceStackTests.cpp` من استقلال الملكية والإرفاق واختيار القمة وذرية الفشل والسعة والحقول المعتمة وعدّ المقابض واحتفاظ العمل/الطلبات بعد الفصل والحذف واختلاف هوية الملف عن هدف الإرسال. يستخدم `driver_wdm_stack.c` الأصلي ترويسات WDK حقيقية وCopy/Skip/SetCompletion مضمنة؛ تحدد المسارات الاختيارية `NEVERD_WDM_STACK_FIXTURE`/`NEVERD_WDM_STACK_CFG_FIXTURE` الصور العادية/ذات CFG النشط. يغطي `DriverWDMStackTests.cpp` إعادة التموضع والحالة الأدنى وترتيب/أعلام الإكمال ونقل pending المتأخر والعامل/DPC والانتظار و`STATUS_MORE_PROCESSING_REQUIRED` واحتفاظ MDL المباشر والإكمال المتداخل والمؤشرات/التحكم غير الصالح. يغطي `DriverScenarioPublicTests.cpp` التمرير عبر C API/CLI والإكمال المحتفظ به والمتداخل عبر C API، بما فيه صور CFG المهيأة. تُتخطى المنتجات المفقودة صراحة. تثبت اختبارات Linux مكدس برنامج التشغيل نفسه فقط، ولا تثبت دعم PDO/PnP/الطاقة. يتحقق `KernelIRPStackTests.cpp` من المؤشرات المعدودة وبادئات Copy المضمنة كاملة وتصفير المواضع المستهلكة ونقل الحالة/pending وMPR والإكمال المتداخل ومالكي الاستمراريات والمسارات المحتفظ بها. تختبر READ/WRITE الحقيقية ودورة حياة الملف أيضًا Copy المضمنة.

يتحقق `DriverPnpScenarioTests.cpp` من تطابق JSON والتحقق الأصلي الصارم، والحقائق الابتدائية الصريحة، وحدود المعرّفات والأعداد، وتركيبات الحقول المحظورة، وحالات الناقل النهائية والتقارير التي تقبل null. تغطي `KernelPnpDeviceTests.cpp` و`KernelPnpRequestTests.cpp` و`KernelPnpCompletionTests.cpp` ملكية المزوّد ونجاح AddDevice وفشله وتسرباته، وIRP الابتدائية، وقبول الملفات، والتراجع عن حالة دورة الحياة، والإكمال المؤجل، واستمراريات MPR والمتداخلة والمنتظرة، وذرية الفشل. يستخدم المثال الأصلي `driver_wdm_pnp.c` المبني بـWDK الحقيقي المسارين الاختياريين `NEVERD_WDM_PNP_FIXTURE` و`NEVERD_WDM_PNP_CFG_FIXTURE`. يختبر `DriverWDMPnpTests.cpp` صورًا عادية وأخرى ذات CFG نشط بعد إعادة التموضع، بما يشمل AddDevice وعمليات الملفات والإزالة المنظمة وتأخير البدء والإزالة وفشل البدء وquery وفشل AddDevice مع تنظيف أو تسرب. يغطي `DriverScenarioPublicTests.cpp` أيضًا سيناريو PnP مؤجلًا من سبعة طلبات عبر C API وCLI، بما فيه صور CFG المهيأة. تُتخطى المنتجات المفقودة صراحة. أدلة التنفيذ خاصة بـLinux ولا تثبت إلا نطاق PnP الموثق بلا موارد.

تتحقق اختبارات مخطط V9 من قراءة وكتابة أسماء الوظائف الثانوية الثماني، وتشارك التحقق من الحالة النهائية مع إكمال دورة الحياة؛ وتُرفض QueryStop ‏0x119 قبل تحميل الصورة. تغطي اختبارات النموذج والمثال الحقيقي الموسعة التراجع بعد query-stop وcancel-stop والتوقف وإعادة البدء والإزالة المفاجئة وفشل عقود النجاح المحدد وعمليات I/O البرمجية أثناء التوقف أو انتظار الإزالة ورفض الضيف بعد الإزالة المفاجئة وهوية الجهاز ونتائج AddDevice المختلطة. ينفذ `DriverScenarioPublicTests.cpp` تسلسلًا من 16 طلبًا للتوقف وإعادة البدء والإزالة المفاجئة عبر C API وCLI، بصور عادية وأخرى ذات CFG نشط، مع الاحتفاظ ببايتات IOCTL البرمجي الناجح وفشل IOCTL الذي أعاده الضيف بعد الإزالة المفاجئة وcleanup/close/remove النهائية. التنفيذ العام تسلسلي: لا يستطيع IRP المحتفظ به دون مصدر إكمال متاح حاليًا انتظار طلب لاحق في السيناريو لبدء الجهاز أو تنظيفه. قيود تصريف الطلبات قبل Remove حدود للنمط، وليست سياسة عامة لقبول I/O في Windows. تظل الأدلة خاصة بـLinux.

يتحقق `DriverPowerScenarioTests.cpp` من حقائق حزم الطاقة الصارمة، وتطابق JSON والمسار الأصلي، والسياق المعتم ذي 32 بت، وحدود طوابير الاستجابة FIFO، وتقارير الطلبات الفرعية المستقلة. يفحص `KernelPowerRequestTests.cpp` و`KernelPowerCompletionTests.cpp` تخطيط الحزمة الفعلي وأعلام المسار والتمييز بين دورة الحياة والإخطار لكل كائن ومطابقة FIFO وملكية الاستدعاء النهائي وMPR والانتظار وحدود التحرير. يستخدم المثال الأصلي `driver_wdm_power.c` المبني بـWDK الحقيقي المسارين الاختياريين `NEVERD_WDM_POWER_FIXTURE` / `NEVERD_WDM_POWER_CFG_FIXTURE`. يغطي `DriverWDMPowerTests.cpp` إعادة تموضع الصور العادية وذات CFG النشط، وQuery/Set المباشرين والمتداخلين، والإكمال المستقل المؤجل، وترتيب S0 قبل D0، وبقاء لقطات الاستدعاء ذي المعاملات الخمسة أثناء الانتظار، والطلبات الفرعية التي تبدأ من عناصر العمل، والاستدعاءات null، ورفض query، وبذور PDO وطوابيرها المستقلة، والفشل الصريح عند غياب الحقائق. يضيف `DriverScenarioPublicTests.cpp` تحققًا مسبقًا من حزم الطاقة المشوهة وتسلسل نوم واستيقاظ من ستة طلبات سيناريو وثلاثة طلبات فرعية عبر C API وCLI. تُتخطى المنتجات الحقيقية المفقودة صراحة؛ وتظل أدلة التنفيذ خاصة بـLinux ولا تثبت إلا نطاق الطاقة الموثق القابل للترحيل وبلا موارد.

يتحقق `KernelUsbIdleTests.cpp` من الملكية وD2 والاستعارة وأول سبب؛ وتغطي `KernelUsbIdleBridgeTests.cpp` / `KernelUsbIdleReceiptTests.cpp` طلبات IRP الحقيقية والإلغاء وسعة المجموعة وترتيب الاستقبال/الإكمال، و`DriverUsbIdleScenarioTests.cpp` المدخلات والتقارير. يستخدم `DriverWdmUsbIdleTests.cpp` العينة الحقيقية `driver_wdm_usb_idle.c` عبر `NEVERD_WDM_USB_IDLE_FIXTURE` / `NEVERD_WDM_USB_IDLE_CFG_FIXTURE` لفحص idle وD0/D3 والإيقاظ والإلغاء وإعادة التسجيل/التشغيل والدوال المستقلة/المركبة ومسارات PDO/FDO. ينفذ `DriverWdmUsbIdlePublicTests.cpp` [سيناريو USB](../examples/driver-wdm-usb-idle-scenario.json) عبر C API/CLI والصور العادية/active-CFG والعناوين المفضلة/المنقولة. تُتجاوز الأدلة المفقودة صراحة؛ التنفيذ مثبت على Linux فقط ولا يثبت دعم USB في KMDF.

تتحقق `KernelFrameworkUsbIdleTests.cpp` و`KernelFrameworkUsbIdleStorageTests.cpp` و`KernelFrameworkUsbIdleBridgeTests.cpp` من السياسة والتخزين والجدولة المُنمّطة. لا ترسل العينة الحقيقية `driver_kmdf_usb_idle.c` طلب idle IRP بنفسها. يغطي `DriverKMDFUsbIdleTests.cpp` غياب الإذن وI/O المُدار مع D2/D0 المؤجلين وStopIdle قبل/أثناء الاستدعاء وفشل arm وMaximum الصريح والإيقاظ والمجموعة المركبة. ينفذ `DriverKMDFUsbIdlePublicTests.cpp` [سيناريو KMDF USB](../examples/driver-kmdf-usb-idle-scenario.json) عبر `NEVERD_KMDF_USB_IDLE_FIXTURE` / `NEVERD_KMDF_USB_IDLE_CFG_FIXTURE` وC API/CLI والصور العادية/active-CFG والعناوين المفضلة/المنقولة. تُتجاوز الأدلة المفقودة صراحة؛ التنفيذ مثبت على Linux فقط. تتحقق اختبارات النموذج من أن نفاد التخصيص بعد نجاح استدعاء تسليح التنبيه ينفّذ استدعاء نزع التسليح الحقيقي ويلغي WAIT_WAKE دون استهلاك استجابة D2.

يغطي `DriverKMDFUsbPoFxTests.cpp` إعداد SystemManaged/WithHint الأولي والإذنين وإلغاء D0 وD2/D0 المؤجلين وتأكيد F0 بعامل حقيقي وStopIdle والإيقاظ قبل READ وفشل arm والإزالة وإعادة البدء. ينفذ `DriverKMDFUsbPoFxPublicTests.cpp` [سيناريو USB PoFx](../examples/driver-kmdf-usb-pofx-scenario.json) عبر C API/CLI بالنمطين والصور العادية/active-CFG والعنوان المفضل/المنقول. يحفظ `DriverKMDFUsbIdleTests.cpp` اختبارات التحويل ويختبر READ المباشر بعد D0Entry. يفحص `KernelFrameworkRequestTests.cpp` الربط وملكية caller-context والطوابير اليدوية/المتوقفة وIRQL. تثبت اختبارات جسر النموذج نفاد التخصيص والبوابتين المستقلتين، لا العينة الحقيقية. تُتجاوز الصور المفقودة صراحة؛ دليل التنفيذ على Linux فقط. يغطي `KernelFrameworkUsbPoFxBridge.RemovalPowerUpFailureAcknowledgesRequiredWithoutReleasingIdleWait` أيضًا فشل D0Entry الحقيقي أثناء RemovePending: يسمح تأكيد Required الدقيق وquiesce بالتنظيف دون F0/ActiveCondition، ولا يثبت دعم فشل SET_POWER العام أو الإزالة المفاجئة الذاتية.

`KernelPowerCompletionTests.cpp`, `KernelProviderWaitWakeTests.cpp`, `KernelWdmWakeEventTests.cpp`, `DriverWdmWaitWakeTests.cpp`: يستخدم [سيناريو WAIT_WAKE الأصلي](../examples/driver-wdm-wait-wake-scenario.json) عينة WDK الحقيقية `driver_wdm_wait_wake.c` عبر `NEVERD_WDM_WAIT_WAKE_FIXTURE` / `NEVERD_WDM_WAIT_WAKE_CFG_FIXTURE`. يغطي الإرسال أثناء START والمعاملات والإيقاظ دون D0 ضمني وإعادة التسليح والإلغاء وMPR وإلغاء DPC ثم D0 من عامل والالتقاط الدقيق والمزودين المستقلين. أدلة الصور العادية/active-CFG والعناوين المفضلة/المنقولة تخص Linux فقط؛ وتُتخطى الملفات الغائبة صراحة.

يغطي `KernelPowerCompletionTests.cpp` قبول APC/DPC وفشل السعة الذري وإعادة المحاولة والمزود وحده بإكمال متزامن/مؤجل مع/دون استدعاء والمسار المحتفظ به وMPR. تتحقق العينة الحقيقية `DriverWdmWaitWakeTests.cpp` من D0 مباشرة من إلغاء DPC وQuery/Set عند APC/DPC وثبات IRQL/CR8 والعودة قبل تنفيذ PASSIVE ورفض WAIT_WAKE المرتفع. يمر [السيناريو المرتفع](../examples/driver-wdm-elevated-power-scenario.json) عبر `DriverWdmWaitWakePublicTests.cpp` وC API/CLI والصور العادية/active-CFG والعناوين المفضلة/المنقولة؛ والأدلة تخص Linux فقط.

يفحص `KernelRemoveLocksTests.cpp` استقلال هوية القفل والجهاز، وقيم Tag الفارغة والمتكررة، وأحجام retail/DBG الدقيقة، والتفريغ الفوري والمؤجل، والتزامات الحيازة الفاشلة، وذرية الفشل والسعة وإنهاء عمر التخزين. يغطي `KernelRemoveLockBridgeTests.cpp` و`DriverWDMRemoveLockTests.cpp` التهيئة قبل الإرفاق، والتخزين المعتم داخل امتداد الجهاز، وحدود IRQL، والتحرير بعد انتهاء عمر الحزمة، وإكمال المزوّد بعد تفريغ القفل، والجاهزية عند التحرير الأخير قبل عودة الاستدعاء، وعناصر العمل المنتظرة وتنظيف فشل AddDevice. يُبنى المثال الأصلي `driver_wdm_remove_lock.c` باستخدام WDK الحقيقي بإصداري retail/DBG وبصور عادية أو ذات CFG نشط عبر المسارات الاختيارية `NEVERD_WDM_REMOVE_LOCK_FIXTURE` و`NEVERD_WDM_REMOVE_LOCK_CFG_FIXTURE` و`NEVERD_WDM_REMOVE_LOCK_DBG_FIXTURE` و`NEVERD_WDM_REMOVE_LOCK_DBG_CFG_FIXTURE`. تستخدم اختبارات C API وCLI العامة مخطط PnP الحالي وتحافظ على الفصل بين ملاحظات استلام الناقل وإكماله والتفكيك النهائي. تُتخطى المنتجات المفقودة صراحة؛ وتقتصر أدلة التنفيذ على Linux ولا تثبت Driver Verifier كاملًا أو تفريغ الطلبات المتزامنة عمومًا.

يتحقق `DriverResourceScenarioTests.cpp` من حقائق JSON/C++ الصريحة وعروض الأعداد وأعداد العناصر وتداخل النطاقات الفيزيائية والسجلات والمحاذاة والمعرفات والبنوك الفارغة وتسلسل التكوين. تغطي `KernelMMIOTests.cpp` و`KernelMMIOFailureTests.cpp` و`KernelResourceBridgeTests.cpp` و`UnicornMMIOTests.cpp` ملكية البنوك والتعيينات وعناوينها البديلة وأجيال الموارد وعمر القوائم المحزّمة وتوقيت المزوّد والاستمرار عبر إعادة التشغيل وإمكانية الوصول بعد الإزالة المفاجئة أو تغيير الطاقة والمعاملات الدقيقة للمعالج وAPI وذرية الفشل. يستخدم المثال الأصلي `driver_wdm_resources.c` المبني بـWDK الحقيقي المسارين `NEVERD_WDM_RESOURCE_FIXTURE` / `NEVERD_WDM_RESOURCE_CFG_FIXTURE`؛ وينفذ `DriverWDMResourceTests.cpp` تعليمات الوصول المفردة وREP الحقيقية وإعادة تموضع الصور العادية وذات CFG النشط وتعيينات النطاقات الفرعية ونهاية الصفحة وSTOP/إعادة التشغيل والوصول غير الصالح. ترفض اختبارات C API وCLI الحقائق غير الصالحة قبل تحميل الصورة وتنفذ سيناريو إعادة التشغيل نفسه من 14 طلبًا مع مخرجات IOCTL مستمرة وأعداد map/unmap دقيقة. يتطلب الملف المشترك [driver-register-bank-scenario.json](../examples/driver-register-bank-scenario.json) بروتوكول السجلات وIOCTL لهذا المثال. تُتخطى المنتجات المفقودة صراحة؛ والأدلة خاصة بـLinux ولا تستخدم ذاكرة المضيف الفيزيائية أو خلفية أجهزة عامة.

يغطي `DriverInterruptScenarioTests.cpp` الواصفات الخام والمترجمة الصريحة والتخصيصات المختلطة والمقتصرة على المقاطعات وصرامة حقول الأحداث وأعدادها وهوية المصدر ومشاهدات BOOLEAN المستقلة. تغطي `KernelInterruptsTests.cpp` و`KernelInterruptBridgeTests.cpp` و`SchedulerInterruptTests.cpp` مطابقة القيم الحصرية والرموز المعتمة والتقاط الجيل والاتصال وعمر الأحداث وحقول Ex المحددة بدقة واستعادة القفل المشترك وIRQL وملكية الاستدعاءات وأولوية ISR في اللحظة نفسها وفشل السعة قبل التعديل. يتحقق `KernelFrameworkRequestTests.cpp` من معاينات الإلغاء الخالصة وسعة دفعات الرموز دون نشر استدعاءات أو استهلاك مراجع. يستخدم المثال الأصلي `driver_wdm_interrupts.c` المبني بـWDK الحقيقي `NEVERD_WDM_INTERRUPT_FIXTURE` / `NEVERD_WDM_INTERRUPT_CFG_FIXTURE`؛ ويختبر `DriverWDMInterruptTests.cpp` إعادة التموضع العادية وذات CFG النشط وABI التقليدي ذي الأحد عشر وسيطًا وإصدارات Ex ‏1/2/4 والإكمال الحقيقي ISR→DPC وFALSE في AL المنخفض والمزامنة والأقفال اليدوية وPDO المستقلة وأجيال إعادة التشغيل وحقائق العتاد غير الصالحة. ترفض اختبارات C API/CLI التصريحات غير الصالحة قبل تحميل الصورة وتنفذ طلبات [driver-interrupt-scenario.json](../examples/driver-interrupt-scenario.json) السبعة مع فحص بايتات IOCTL المعلق ومشاهدات التسليم المنفصلة. تُتخطى الصور المفقودة صراحة؛ وتظل الأدلة خاصة بـLinux ولا تثبت دعم المقاطعات المشتركة/المستوية/MSI أو استباق التعليمات.

يتحقق `DriverDMAScenarioTests.cpp` من القدرات الصريحة والنطاقات المنطقية وحدود البايتات والأعداد والوقت والاتجاهات الصارمة وفصل الإعدادات عن المشاهدات. يفحص `KernelPhysicalMemoryTests.cpp` و`BackendBackingTests.cpp` حدود التخصيص على الصفحات المشتركة والتثبيت وثبات صلاحيات CPU واستبعاد MMIO وإعادة الدخول وذرية فشل النطاق الكامل؛ ويفحص `KernelRequestMDLTests.cpp` PFN للقراءة فقط وعناوين الواصفات المبنية البديلة المرتبطة بالهويات الفيزيائية نفسها. يختبر `KernelDMATests.cpp` و`KernelDMABridgeTests.cpp` و`SchedulerDMATests.cpp` بايتات RAM الفعلية واستدعاءات الجدول المرتبط بالمحوّل وملكية FIFO الفورية والمصطفة والأعمار المنفصلة للاستدعاء والتعيين وأجزاء الصفحات والاتجاهات الخاطئة والتحقق قبل التحرير ونطاقات PDO المستقلة وأخطاء الجيل والطاقة. يستخدم المثال الأصلي الحقيقي `driver_wdm_dma.c` الخيارين `NEVERD_WDM_DMA_FIXTURE` / `NEVERD_WDM_DMA_CFG_FIXTURE`؛ وتنفذ `DriverWDMDMATests.cpp` واختبارات C API/CLI مؤشرات المحوّل الحقيقية وذاكرة common/SG وأحداث DMA والمقاطعات المعدّة بصورة منفصلة. يتطلب المثال المشترك [driver-dma-scenario.json](../examples/driver-dma-scenario.json) بروتوكول هذا البرنامج. تُتخطى المنتجات الغائبة صراحةً؛ والأدلة مقصورة على Linux ولا تثبت DMA حقيقيًا للمضيف أو PCI أو محرك أجهزة عام. يستخدم `pluginsdk/python/tests/test_driver_dma_integration.py` ربط JSON الحالي ذا الملكية الصريحة مع `NEVERD_TEST_LIBNEVERD` / `NEVERD_TEST_WDM_DMA_FIXTURE` / `NEVERD_TEST_WDM_DMA_CFG_FIXTURE` للتحقق من البايتات وترتيب الاستدعاءات ومشاهدات الفشل.

يتحقق `KernelSEHTests.cpp` من خطط فك المكدس الصرفة وترتيب النطاقات واستعادة السجلات العامة غير المتطايرة والمكدسات المحدودة والبيانات الوصفية غير المدعومة صراحةً؛ ويتحقق `KernelExceptionTests.cpp` من عدد معاملات API الدقيق وحالات البتات الدنيا الـ32 والاستثناءات محددة النوع وحدود IRQL وعدم تغير حالة النموذج/CPU. يستخدم المثال الأصلي `driver_wdm_seh.c` المبني بـWDK حقيقي و`/GS-` الخيارين `NEVERD_WDM_SEH_FIXTURE` / `NEVERD_WDM_SEH_CFG_FIXTURE`؛ ويشغل `DriverWDMSEHTests.cpp` الصور العادية وذات CFG النشط والمعاد تموضعها لاختبار الرفع المباشر وعبر الدوال المساعدة والمعالجات المتداخلة وإعادة الرفع من المعالج والاستثناءات غير المعالجة والرفض الصريح للمرشحات/finally/أعطال CPU. يشغل C API/CLI المثال [driver-seh-scenario.json](../examples/driver-seh-scenario.json) ويتحقق من نتائج API بقيمة null مع رسائل معالج الضيف الفعلية. يستخدم `pluginsdk/python/tests/test_driver_seh_integration.py` المتغيرات `NEVERD_TEST_LIBNEVERD` و`NEVERD_TEST_WDM_SEH_FIXTURE` و`NEVERD_TEST_WDM_SEH_CFG_FIXTURE`. تُتخطى الصور الخارجية المفقودة صراحةً؛ وتظل الأدلة خاصة بـLinux ولا تثبت دعم مخازن المستخدم أو SEH العام.

تتحقق `KernelDMAChannelTests.cpp` و`KernelDMAChannelBridgeTests.cpp` و`SchedulerDMATests.cpp` المشترك من FIFO للتخصيصات المختلطة، وعروض قيم العودة، وفحوص القبول والتحرير الخالية من الآثار، وإعادة استعمال السجلات، والأجزاء المتجاورة، وتفريغ كامل العملية، ولقطات CurrentIrp، وأعمار الحزم وMDL والأجهزة. تستخدم العينة الأصلية المبنية بـWDK الحقيقي `driver_wdm_dma_channel.c` المسارين الاختياريين `NEVERD_WDM_DMA_CHANNEL_FIXTURE` / `NEVERD_WDM_DMA_CHANNEL_CFG_FIXTURE`؛ ويشغّل `DriverWDMDMAChannelTests.cpp` الصور العادية وCFG النشط والمعاد تموضعها باستدعاءات MapTransfer/FlushAdapterBuffers فعلية وحصة common/SG/قناة مشتركة، ومعاملات صريحة وإكمال IRQ/DPC وعمليات متتابعة وPDO اثنين وحالات فشل. يشغّل C API/CLI الطلبات السبعة في [driver-dma-channel-scenario.json](../examples/driver-dma-channel-scenario.json)، ومنها معاملة واحدة تعبر الجزأين المعيّنين. يستخدم `pluginsdk/python/tests/test_driver_dma_channel_integration.py` المتغيرات `NEVERD_TEST_LIBNEVERD` و`NEVERD_TEST_WDM_DMA_CHANNEL_FIXTURE` و`NEVERD_TEST_WDM_DMA_CHANNEL_CFG_FIXTURE` للواجهة العامة JSON نفسها. تُتخطى المنتجات الغائبة صراحةً؛ ولا تثبت أدلة Linux دعم متحكمات DMA النظام أو أنماط HAL العشوائية للتعيين والتفريغ.

يغطي `DriverGuardTests.cpp` وأربعة متغيرات أصلية من `driver_guard.c` حالات CFG النشط وغير النشط ونقل عنوان التحميل وABI استدعاءات check/dispatch والأهداف المشوهة. تغطي `KernelFrameworkTests.cpp` و`KernelFrameworkControlTests.cpp` و`KernelFrameworkQueueTests.cpp` و`KernelFrameworkRequestTests.cpp` الربط وإنشاء الأجهزة مع التراجع الكامل عند الفشل وتوجيه الطوابير والأطوال المنطقية للمخازن المؤقتة وترتيب التنظيف وأعمار IRP والسياقات. يُترجم الملفان الأصليان `driver_kmdf_lifecycle.c` و`driver_kmdf_control.c` اختياريًا باستخدام ترويسات WDK 1.33 الحقيقية ويرتبطان عبر مكتبة `FxDriverEntry` الفعلية. اضبط مسارات CMake المخزنة `NEVERD_KMDF_FIXTURE` / `NEVERD_KMDF_CFG_FIXTURE` لصور دورة الحياة، و`NEVERD_KMDF_CONTROL_FIXTURE` / `NEVERD_KMDF_CONTROL_CFG_FIXTURE` لصور أجهزة التحكم العادية وذات CFG النشط. يُعلن التخطي صراحة عند غياب المنتجات الخارجية. يغطي `DriverKMDFLifecycleTests.cpp` و`DriverKMDFControlTests.cpp` وحالات C API/CLI في `DriverScenarioPublicTests.cpp` الاستدعاءات الفعلية والإدخال والإخراج بالمخازن المؤقتة وبالوصول المباشر وإكمال الطلبات المعلقة عبر عناصر العمل وحالات الفشل وإلغاء التحميل وتنفيذ CFG بعد نقل عنوان التحميل. تظل الأدلة محصورة في Linux ولا تثبت دعم KMDF الكامل أو PnP وإدارة الطاقة.

تحتفظ اختبارات واجهة الإلغاء القديمة باستكمال الواجهة خلال الإلغاء والتنظيف المتداخل والتدمير النهائي؛ وتظل Ex تعيد حالة الإلغاء دون استدعاء راجع للطلب الملغى مسبقًا. تغطي `KernelFrameworkRequestAccessorTests.cpp` و`KernelRequestMDLTests.cpp` Information المشترك ذي 64 بت وفحص الطول عند الإكمال وهوية الطابور/IRP الأصلية ومقبض ملف WDF بقيمة NULL ونتائج getter للمقابض المحتفظ بها وMDL المخزن مع ByteCount للاتجاه الأول والواصف المباشر والتعيين المؤجل والإبطال ومنع تجاوز WDM. تنفذ أوضاع L وM وD وC لعينة التحكم الحقيقية واجهة الإلغاء القديمة وMDL/المعلومات المخزنة وMDL READ/WRITE المباشر والوصول بعد الإكمال في الصور العادية وذات CFG النشط.

تغطي اختبارات الإلغاء المهل الافتراضية المسموحة للنقل فقط وحقول التقارير، والإكمال السابق والطلبات الملغاة مسبقًا، ووضع العلامة وإزالتها، وسلطة الإكمال بحسب الاصطفاف أو التسليم، وانتظار الاستدعاءات والمراجع الداخلية. تتحقق اختبارات المجدول بصورة مستقلة من ترتيب DPC/الإلغاء/العمل والسعة وفصل الهويات والإيقاف والاستئناف. يبقى إلغاء WDM خطأ صريحًا في النموذج.


## توزيع الاختبارات

ينشئ `add_neverd_unittest` ملف GoogleTest تنفيذيًا واحدًا، ويمنح كل حالة مكتشفة
وسم CTest يساوي اسم ذلك الهدف التنفيذي.

| منطقة المصدر | الهدف ووسم CTest | التغطية |
|--------------|------------------|---------|
| `unittests/TestProcessTests.cpp` | `NeverDTestProcessTests` | استدعاء العمليات الفرعية عابر المنصات، وquoting، وإعادة التوجيه، ورموز الخروج |
| `unittests/libc` | `NeverDLibCTests` | أسماء libc المعروفة وتصنيفها |
| `unittests/safety` | `NeverDSafetyTests`، `NeverDSafetyIntegrationTests` | كتالوج المصارف، وأولوية الهوية، ومرشح الوسائط المسبق، وصيد فيضان النسخ، وتدقيق عمر الكومة، ومصفوفة إلزامية من ست خلايا PE/ELF/Mach-O × x86-64/AArch64 |
| `unittests/lift` | `NeverDLiftTests` | أشكال LowIR لـ decoder/lifter، ومراحل IR، وloader، وrelocation، وfixtures الصيغ، وإعادة التجميع، ومسارات patch الممثلة |
| معظم ملفات `unittests/semantic` | `NeverDSemanticTests` | دلالات تفاضلية للتعليمات وABI والتحكم وتعابير C وlift/recompile |
| `unittests/evm` | `NeverDEVMOpcodeTests` و`NeverDEVMBytecodeTests` و`NeverDEVMLoaderTests` و`NeverDEVMABITests` و`NeverDEVMAnalyzerTests` و`NeverDEVMDecoderPropertyTests` و`NeverDEVMProxyTests` و`NeverDEVMCallTests` و`NeverDEVMSemanticTests` و`NeverDEVMEmitterTests` و`NeverDEVMIntegrationTests` | metadata للـhardfork وتطبيع الإدخال وغموض ABI/signature وCFG/SSA والاستعادة واستنفاد حدود decoder والمدخلات العدائية وحقائق proxy/call ودلالات interpreter وتنفيذ LLVM/C/Solidity التفاضلي وتوجيه API العامة |
| `unittests/sbf` | `NeverDSBFMetadataTests`، `NeverDSBFProgramImageTests`، `NeverDSBFLoaderTests`، `NeverDSBFAnalyzerTests`، `NeverDSBFVerifierTests`، `NeverDSBFISAConformanceTests`، `NeverDSBFAgaveConformanceTests`، `NeverDSBFSemanticTests`، `NeverDSBFEmitterTests`، `NeverDSBFLLVMEmitterTests`، `NeverDSBFLLVMDifferentialTests`، `NeverDSBFSourceDifferentialTests`، `NeverDSBFMalformedCorpusTests`، `NeverDSBFUpstreamConformanceTests`، `NeverDSBFExternalOracleTests`، `NeverDSBFSolanaModelTests`، `NeverDSBFIntegrationTests` | بيانات v0-v4 الوصفية وتخطيطات ELF، وسلوك التحقق والتحميل الصارم، و23 من عناصر ELF المثبتة، وoracle الرسمي المنفصل، وتغطية opcode الشاملة، والمدخلات العدائية، وCFG/الاستعادة، وفروق LLVM/C/Rust المنفّذة |
| `PatchFullSubstRTTests.cpp` | `NeverDPatchFullTests` | تكافؤ إعادة الكتابة/التشويش عبر أربع ISA وثلاث صيغ كائنات |
| ملفات التحويل المحددة في `unittests/semantic` | `NeverDSwitchXformTests` و`NeverDIndCallXformTests` و`NeverDCFGLoopXformTests` و`NeverDTwoTableXformTests` و`NeverDAvxUpperXformTests` | مجسات سريعة الربط منفصلة عن الثنائي الدلالي الكبير |
| `unittests/corpus` (وحدة فرعية) | `NeverDWindowsEHCorpusTests` و`NeverDRustEHCorpusTests` و`NeverDGoEHCorpusTests` و`NeverDCxxItaniumEHCorpusTests` و`NeverDObjCEHCorpusTests` و`NeverDAdaDEHCorpusTests` | metadata الاستثناءات ووقت التشغيل المقروءة من 545 ثنائيًا حقيقيًا مثبّتًا، كل واحد منها معلن في manifest يذكر الحدود الدنيا التي يجب أن يتجاوزها استرجاعه |

مصادر التسجيل الموثوقة هي
[`unittests/CMakeLists.txt`](../../unittests/CMakeLists.txt) و
[`unittests/lift/CMakeLists.txt`](../../unittests/lift/CMakeLists.txt) و
[`unittests/semantic/CMakeLists.txt`](../../unittests/semantic/CMakeLists.txt) و
[`unittests/evm/CMakeLists.txt`](../../unittests/evm/CMakeLists.txt) و
[`unittests/sbf/CMakeLists.txt`](../../unittests/sbf/CMakeLists.txt) و
[`unittests/safety/CMakeLists.txt`](../../unittests/safety/CMakeLists.txt).

### الـcorpus الثنائي المثبّت

كل مجموعة اختبارات أخرى تبني ما تختبره، أما الـcorpus فلا: إنه وحدة فرعية من
ثنائيات أنتجتها سلاسل أدوات حقيقية، على مضيفات ولأهداف لا يستطيع هذا المستودع
بلوغها، وكل ملف مثبّت بالبصمة وإلى جانبه manifest يذكر الحدود الدنيا التي يجب أن
يتجاوزها استرجاعه. هذا هو المكان الوحيد الذي يصير فيه ادعاء عن ما تقرأه NeverD
من — مثلًا — كائن مشترك `armv7` مبني بـ`-O2` ومجرّد من الرموز قابلًا للإجابة بدل
أن يكون محل جدل.

لا تُبنى هذه المجموعات إلا حين يُطلب من خطوة الإعداد البحث عنها، فهذا الخيار هو
كل ما يبقيها تحت الاختبار:

```bash
cmake -S . -B build-corpus -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON \
  -DNEVERD_ENABLE_BINARY_CORPUS_TESTS=ON
cmake --build build-corpus --target check-neverd-corpus --parallel 4
```

يشغّل `check-neverd-corpus` كل الخطوط، بينما يشغّل
`check-neverd-windows-eh-corpus` و`check-neverd-rust-eh-corpus` و
`check-neverd-go-eh-corpus` و`check-neverd-cxx-itanium-eh-corpus` و
`check-neverd-objc-eh-corpus` و`check-neverd-ada-d-eh-corpus` خطًا واحدًا لكل منها. تُعدّ مضيفات الـCI الثلاثة جميعها
بهذا الخيار وتشغّل الخطوط الستة: البايتات واحدة في كل مكان، أما ما يقرؤها فليس
كذلك، وتشغيل الـcorpus على مضيف واحد لا يثبت شيئًا عن المضيفين الآخرين. يرفض
`scripts/audit_ci_test_inventory.py` أي inventory ينقصه أحد الـlabels الستة، لأن
بناءً توقف بصمت عن قراءة الـcorpus هو انحدار لا يستطيع أي اختبار التقاطه —
فالاختبار نفسه هو ما اختفى.

يُشغّل تدقيق opcodes ‏EVM الحي بالأمر التالي:

```bash
python3 scripts/audit_evm_opcode_metadata.py
```

يفرض المسار القياسي محليًا وفي CI تنفيذ
`git fetch --depth=1 --force` من URL الرسمي
`https://github.com/ethereum/go-ethereum.git`، ولا يفحص في worktree مؤقت detached
إلا SHA الدقيق الذي جُلب للتو من remote `HEAD` للـdefault branch. كل تشغيل يستخدم
bare repository خاصًا مؤقتًا باسم غير متوقع، ويحفظ authority ref
الذي أعاده fetch والـSHA الدقيق المحلول منه طوال عمر worktree الـdetached، ثم
يدمرهما معًا. لا يوجد Git repository أو cache دائم مشترك. ليست
`local_docs` ولا checkout موجود ولا submodule مسارات تدقيق؛ إذ يصبح pin
الـsubmodule قديمًا تحديدًا عند الحاجة إلى كشف live drift.

تحذف كل أوامر Git أولًا جميع `GIT_*` الموروثة، ومنها `GIT_CONFIG_*`، ثم تثبت
القيم المدققة فقط. يعطل `GIT_CONFIG_NOSYSTEM` و`GIT_CONFIG_GLOBAL` إعدادات
system/global؛ ويعطل `GIT_ATTR_NOSYSTEM` و`core.attributesFile` على مستوى الأمر
attributes النظام والعامة، كما يعطل `core.hooksPath` hooks. يفشل الفحص عند إعداد غير متوقع في
الـrepository الخاص أو
grafts أو `objects/info/alternates` أو `refs/replace`، ويعطل
`GIT_NO_REPLACE_OBJECTS` replacement lookup.

يعكس probe كل حقول bool المصدرة في `params.Rules`،
ويستدعي `LookupInstructionSet(params.Rules)` ويفحص كل 256 slots. يملك
`EVMUpstreamOpcodePolicy.def` aliases وtyped exclusions التاريخية/EOF غير
المجدولة؛ ويملك `EVMUpstreamSemanticsPolicy.def` inventory الـRules المغلق وfork
mappings وbase-stack exceptions وعائلات dynamic-immediate.

يشغل CI التدقيق الحي نفسه فقط عند push إلى `dev` وpull request والتشغيل اليدوي
والجدول اليومي. يستدعي Go probe الواجهة العامة
`LookupInstructionSet(params.Rules)` لكل fork مربوط. يملك
لا تعرض CLI العامة سوى `--manifest-output`؛ ويستخدم manifest المغلق `schema 3`
ولا يسمح باختيار source أوref أوcheckout أوtoolchain.
`EVMUpstreamOpcodePolicy.def` aliases والاستثناءات التاريخية/EOF غير المجدولة المراجعة،
بينما يملك `EVMUpstreamSemanticsPolicy.def` المستقل قواعد forks واستثناءات stack
semantics. يفحص manifest المغلق revision الدقيقة وactivation وbyte/name و
`base_min_stack` و`net_stack_delta`، ويرفض fields وforks والأسماء والbytes المجهولة
أو المكررة. يحدد probe allocation من `operation.undefined` وحده؛ و`HasCost` مجرد
فحص متقاطع للكلفة لأن العملية المعرّفة صفرية الكلفة تعيد false أيضًا. يجب أن
يطابق كل `defined && !HasCost` ‏slot تصريح `EVM_GETH_ACTIVE_WITHOUT_COST` تمامًا
من fork التفعيل المحدد؛ ويفشل undefined slot ذو كلفة أو defined slot غير مراجع
أو اختفاء marker بشكل مغلق. تفشل كذلك declarations المفقودة أوخارج النطاق أوغير
المستهلكة نحويًا؛ فكل `.def parser` يرفض policy ‏`partial`. عند فشل CI تُرفع
revision وmanifest وlog كـartifact. للـparser وتشخيص الانحراف تغطية unit مستقلة:

يصنّف `EVMUpstreamSemanticsPolicy.def` كل boolean field مصدّر في `params.Rules`
بسجل `EVM_GETH_RULE_FIELD` واحد: `MappedForkSelector` أو `NoOpcodeAllocation` أو
`ExcludedSelectorExpectedError`. يفعّل probe كل field منفردًا عبر
`LookupInstructionSet`؛ يجب أن يعيد الصنفان الأولان nil error والثالث error، وأن
تطابق fingerprint الكاملة لـ256 opcode/stack slots قيمة `ExpectedFork`.
الحقول `IsEIP155` و`IsEIP2929` و`IsEIP4762` و`IsPetersburg` بلا allocation وتطابق
Frontier؛ أما `IsUBT` فيجب أن يفشل ويطابق Cancun.

يصرح `EVMUpstreamSemanticsPolicy.def` بعائلات opcodes الديناميكية EIP-8024 ونوع
العملية وstack delta الصالح، بينما يملك `EVMEIP8024Immediates.def` فك immediate
المنفصل ويصنف 256 byte من single/pair. عبر `go -overlay` يحصل التدقيق على private
handlers الحقيقية `operation.execute`، ويمر على `canonical fork jump tables` و
`mainnet active/scheduled jump tables` جدولًا بجدول. يسجل العائلة `inactive`
ويرفض `partial`. يختبر كل جدول نشط `DUPN` و`SWAPN` و`EXCHANGE` مع كل immediate (`3x256`)
و`3 missing-operand cases` مقابل المصادر التصريحية نفسها.

لـ`EVM_HARDFORK_LATEST` target canonical واحد. يربط
`EVMUpstreamForkAliases.def` المغلق Prague→Pectra وOsaka وBPO1–BPO5→Fusaka، وتعود
Paris/Shanghai/Cancun/Amsterdam/Bogota إلى نفسها؛ وتفشل الأسماء المجهولة مغلقًا.
تقود قيمة `audit_unix_time` المسجلة فحص `MainnetChainConfig.LatestFork(time)`
(يجب أن يساوي NeverD latest) وفحص alias/probe لـ`LatestFork(max uint64)`؛ وتقارن
مجموعتا التعليمات كاملتين. يثبت manifest ‏`authority=official-fresh-fetch` وURL الرسمي
و`HEAD` وSHA. يستخدم probe ‏`GOTOOLCHAIN=local`.

يفرض Go probe وPython controller ‏`input/collection/string hard limits`؛ فتفشل
المدخلات أوالمجموعات أوالنصوص الضخمة مغلقًا. أما
`bounded diagnostic output` فيرفق بالعرض الطويل `digest` كامل المحتوى و
`explicit truncated marker`. يطبق على كل child خرج وdeadline محدودان؛ وعند
التجاوز تُقتل `process group` كاملة/process tree وتُصرّف pipes.

تسجل وصلة schema 3 الحالية `schema_version=3` و
`audit_unix_time=1787534659` و`authority=official-fresh-fetch` و
`remote=https://github.com/ethereum/go-ethereum.git` و`ref=HEAD` وrevision
`02b73d4ea7181464175e0a6cbecc0a3a2655a562` و`Go 1.24.0` محلية و
`stack_limit=1024` و`diagnostics=[]`. تغطي `21 fork tables` و`20 Rules probes`
بتصنيف `15 mapped/4 no-op/1 expected-error`. يسمي سجلا
`mainnet active/scheduled` ‏`upstream BPO2` المربوط مغلقًا بـ`NeverD Fusaka`.
ومن `23 table targets` لا ينشط إلا `Amsterdam/Bogota`، بنتيجة
`1536 candidate executions` و`6 missing-operand cases`. وتتطابق
`three handler symbols` بين الهدفين النشطين. نجح Python audit ‏`67/67` و
`C++ Opcode 10/10`. نجح macOS تحت `sandbox-exec` مع `go run` نهائي بلا شبكة،
ويفرض Linux ‏`bubblewrap`.

تمر كل مراحل Go، أي `go env` و`go mod init` و`go mod edit` و`go mod tidy` و
`go mod download` و`go run`، عبر filesystem sandbox من نوع `capability-root`.
تسمح القراءة فقط لـprivate probe وfresh geth و`resolved GOROOT` المتحقق منه وجذور
system runtime المطلوبة بدقة، وتسمح الكتابة فقط في isolated environment roots.
تمنح الشبكة لمراحل dependencies التي تحتاجها وحدها ويبقى run النهائي offline.
تثبت الاختبارات أن sentinels في `host HOME/workspace` مرفوضة وأن محتواها لا يظهر
في output. ويطابق Linux هذه السياسة بـ`bubblewrap` من دون `/` broad bind.

```bash
python3 -m unittest -v scripts.tests.test_audit_evm_opcode_metadata
```

أهداف اختبار EVM الأحد عشر المسجلة حاليًا في CMake هي:

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

يستنفد `NeverDEVMDecoderPropertyTests` كل مدخلات البايتين في كل fork يغيّر
decoder، ويقارن decode كاملًا وحدود `JUMPDEST` الدقيقة، ثم يمرر مدخلات عدائية
حتمية محدودة الطول عبر كل forks.

لتغييرات control flow في EVM، شغّل أولًا عقد fixed point وheight domain:

```bash
cmake --build build --target NeverDEVMAnalyzerTests --parallel 4
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.StackHeightDomain*:EVMAnalyzer.WholeProgram*'
```

تغطي هذه الحالات returns عابرة للـblocks وmerges محدودة متعددة الأهداف وتقارب
loops وترتيب edges الحتمي وwhole-stack lanes الحساسة للمسار وحفظ correlation و
unknown jumps والأهداف غير الصالحة الدقيقة وfail-loud budgets وstack faults.
لا يمثل `MayReachable` إلا مرشح CFG ولا ينتج حقيقة يقينية. بعدها شغّل أهداف EVM
الأحد عشر كلها مع live upstream audit.

ولتغييرات dataflow في MedIR/HighIR، شغّل أيضًا عقود constant-phi وselector
وtyped-operand وmalformed-graph وdeep-chain:

```bash
build/bin/NeverDEVMAnalyzerTests \
  --gtest_filter='EVMAnalyzer.MediumIR*:EVMAnalyzer.HighIR*:EVMAnalyzer.*Selector*:EVMAnalyzer.*MedIR*:EVMAnalyzer.RecoversStorageAndEventFactsFromTypedOperands:EVMAnalyzer.RecoversComputedCalldataArgumentOffset:EVMAnalyzer.*Return*:EVMAnalyzer.*Receive*'
```

تثبت هذه الحالات phis الدورية المتساوية والمتعارضة، وتعابير selector غير
المتجاورة والعابرة للـblocks، وترتيبي operands للمساواة، وفحوص ABI width الدقيقة،
وoperands النوعية لـstorage/event/calldata، والتعامل الحتمي مع MedIR malformed،
وproducer walk تكرارية من 16,384 قيمة.

## كيفية إنتاج fixtures

### Fixtures الرفع والصيغ

يجمع `unittests/lift/CMakeLists.txt` مصادر C وassembly لأهداف متعددة أثناء
البناء. تنتج triple ‏Clang كائنات ELF لـ x86-64 وi386 وAArch64 وARM32، وكائنات
PE/COFF وصورًا مرتبطة، وكائنات Mach-O i386 ‏PIC/no-PIC. عند توفر LLD، تُربط
كائنات مختارة أيضًا كملفات تنفيذية لاختبارات patch. يعتمد `NeverDLiftTests` على
هدف `lift-test-objects`، لذلك يجدّد البناء العادي لذلك الثنائي fixtures المولدة.

تستخدم معظم اختبارات lift ‏`NeverDLiftFixture.h` لاستدعاء CLI المبنية `neverd`
وفحص LowIR وMedIR وHighIR وLLVM IR وC المولدة أو ثنائي معاد كتابته. يمكن لمتغير
البيئة `NEVERD` تجاوز مسار CLI في تجربة يدوية محددة؛ وتستخدم عمليات CTest العادية
الملف التنفيذي الذي يضمنه CMake.

### Fixtures سلامة الذاكرة

يحتوي `unittests/safety/fixtures/binaries` على صور PE وELF وMach-O مُودعة
لمعماريتَي x86-64 وAArch64، مع ملف PDB أو dSYM المرافق الذي توفّره كل صيغة،
إضافةً إلى ملف MAP من الرابط لكل صورة. الـMAP هو ما تبقى البُنية المجرّدة من
الرموز تُصدره، لذلك تُحلَّل كل خانة أيضًا مع تسمية الـMAP صراحةً، وهو ما يثبّت
ما يحق للنتيجة أن تدّعيه حين لا تبقى أنواع ولا أسطر مصدرية. يشغّل
`NeverDSafetyIntegrationTests` الخانات الست كلها على كل مضيف؛ وتفشل التهيئة إذا
غابت أي صورة أو ملف مرافق مطلوب، ولا يملك الطقم أي مسار تخطٍّ يعتمد على سلسلة
أدوات المضيف.

تأتي الثنائيات المتكافئة من ملف مصدري واحد. أعد بناء fixture الدخان الأصلية
للمضيف بـ `make`، أو أعد توليد المصفوفة المُودعة كاملةً بـ:

```bash
make -C unittests/safety/fixtures matrix
```

تحتاج وصفة المصفوفة إلى أهداف Clang المتقاطعة للينكس وويندوز، وأدوات COFF من
LLD، ومعماريتَي Darwin كلتيهما، و`dsymutil`. تُعاد خرائط مسارات التنقيح ويُعطَّل
تسجيل سطر أوامر CodeView حتى لا تلتقط الملفات المرافقة المُودعة المسار المطلق
لمساحة عمل المطوّر.

### إعادة بناء استثناءات Windows

تحتاج تغييرات الاستثناءات الجدولية في Windows إلى اختبارات للتمثيل واختبار patch
لملف PE مرتبط. يغطي مرشح مجموعة lift المركّز النموذج الموحّد لـ
unwind/SEH/C++، ومعالجة المدخلات التالفة، وحواف CFG الاستثنائية، وHighIR،
وتوليد LLVM WinEH، واستبدال دليل الاستثناءات، وإعادة بناء Guard CF/EH
continuation:

```bash
cmake --build build --target NeverDLiftTests --parallel 4
build/bin/NeverDLiftTests \
  --gtest_filter='COFFException*:*PatchCOFF_X64.ReconstructsGuardedSEHAndContinuationTable:*PatchCOFF_X64.ReconstructsNativeFH3StateGraph:*PatchCOFF_X64.RejectsInteriorExceptionDirectoryPadding:*PatchCOFF_X64.RebuildsSortedExceptionDirectoryInAppendedSection'
```

تتطلب fixture التجميع x64 المحمية هدف Windows في Clang و`lld-link`؛ ويستخدم
ربط CMake الخيارين `/guard:cf` و`/guard:ehcont`. التخطي بسبب غياب cross-linker
ليس دليلاً على مسار الصورة النهائية. تثبت حالة التكامل الناجحة أن PE المعاد
كتابته يمكن تحميله مجددًا وأن جداول runtime-function وunwind وload-config وGuard
CF وGuard EH continuation مرتبة، ومدعومة ببيانات الملف، وتشير إلى أهداف قابلة
للتنفيذ.

تغطي fixture FH3 المرتبطة إغلاق C++ الأصلي بصورة مستقلة: جداول الحالة الثابتة،
وتعليقات HighC، والحفاظ على personality، وأهداف catch المولدة، ومخطط IP-to-state
بعد إعادة التحميل.

راجع [إعادة بناء استثناءات Windows](windows-exception-reconstruction.md)
لمصفوفة دعم التحليل/التوليد الأصلي وعقد patch الذي يفشل بأمان.

### نماذج الاستثناءات حسب اللغة

كل ما ليس نموذج جداول Windows يقع في هدف واحد مركّز. يغطي
`NeverDLanguageEHTests` سلسلة إطارات DWARF، ومنطقة البيانات الخاصة باللغة في
Itanium، وARM EHABI، وcompact unwind الخاص بـ Darwin، وبيانات إطارات زمن تشغيل
Go، وآلية panic في Rust، وأزمنة تشغيل Objective-C الثلاثة:

```bash
cmake --build build --target NeverDLanguageEHTests --parallel 4
build/bin/NeverDLanguageEHTests --gtest_filter='ObjC*'
```

تُبنى جداول هذه المجموعة بايتًا بايت بدل تصريفها، لأن معظم التوليفات المقصودة لا
تُصدِرها أي سلسلة أدوات واحدة معًا. وObjective-C أوضح مثال: أزمنة التشغيل الثلاثة
تُصدِر جميعها LSDA بصيغة Itanium ولا تختلف إلا فيما تحمله خانة جدول الأنواع، وهذا
الاختلاف كلي لا تدريجي. خانة Apple تُعنون `objc_typeinfo` الذي صُمِّم حقلاه الأولان
عمدًا ليحاكيا `std::type_info`؛ وخانة Objective-C++ في GNUstep تُعنون صنفًا مشتقًا
حقيقيًا من `std::type_info`؛ أما خانة زمن تشغيل GNU فليست مؤشرًا أصلًا بل سلسلة اسم
الصنف نفسها. وتطبيق عرف زمن تشغيل على جدول زمن تشغيل آخر لا يفشل، بل يُبلِّغ عن اسم
صنف قُرئ من وسط شيء آخر تمامًا؛ ولهذا يُحدَّد زمن التشغيل من personality الإطار قبل
قراءة أي خانة.

وتثبِّت المجموعة نفسها تمييزين يسهل دمجهما ويكون دمجهما خطأً. فـ `@catch(id)` و
`@catch(...)` معالِجان مختلفان — الأول يستقبل أي كائن Objective-C ويدع الاستثناء
الغريب يمر بجانبه — وكل زمن تشغيل يكتبهما بصورة مختلفة، فالمفكِّك الذي يُبلِّغ عنهما
معًا كـ catch-all يضع معالجًا على استثناءات كانت في الواقع ستمر دون توقف. كما أن
جدول مواقع الاستدعاء في نموذج setjmp/longjmp يفهرس مواقع الاستدعاء لا العناوين،
فالقارئ الذي لا يتعرف على إحدى personalities الخاصة بـ SJLJ لا يُخفق، بل يخترع
نطاقات محمية وlanding pads لم يسمِّها البرنامج قط.

والتعرّف على هذا الشكل ليس كرفض فكّه. فالمدخل الواحد في SJLJ زوج من قيم ULEB128 —
مُحدِّد إرسال وإزاحة action — وهذه الإزاحة تعني هنا ما تعنيه تمامًا في الشكل العنواني،
ولذلك تُقرأ سلسلة الـ action وأنواع الـ catch ومواصفات الاستثناء كلها من جدول لا يسمّي
أي شيفرة على الإطلاق. ولا يبقى مجهولًا سوى النطاق الذي يحرسه كل مدخل، لأن ما ينصّ عليه
هو ما تكتبه الدالة نفسها في خانة call-site الخاصة بها، لا أي شيء في الجدول. كما تثبّت
المجموعة البايت الوحيد الذي لا يجوز الوثوق به هنا: يكتب GCC `DW_EH_PE_uleb128` ترميزًا
لـ call-site ويكتب LLVM `DW_EH_PE_udata4`، ثم يُصدر كلاهما ULEB128 على أي حال، ولا
تقرأه أي personality قط — فلا يجوز لمُفكِّك الترميز أن يقرأه أيضًا.

وتُثبَّت إلى جانب ذلك هوية الـ personality، لأنها هي ما يقرّر كيف يُقرأ كل جدول أعلاه.
فـ GNAT يسمّي إجراءه بالطرق الثلاث التي يسمّي بها GCC إجراء كل واجهة أمامية — `_v0`
و`_sj0` و`_seh0` — ويسجّل على Windows رمزًا بينما يحيل إلى آخر، فوجب أن تؤول التهجئات
الأربع جميعها إلى Ada. أما D فهي الصورة المعكوسة: ثلاثة مترجمات، وثلاثة أسماء لإجراء
واحد، ووراءها مجموعة جداول واحدة.

### دورات Unicorn التفاضلية

تختبر fixture الدلالية السلوك بدل الشكل النصي:

1. اكتب حالة C/assembly صغيرة أو أنشئ LLVM IR.
2. اجمعها بـ Clang/LLVM للهدف المطلوب.
3. نفّذ شفرة الآلة الأصلية في Unicorn والتقط قيمة العودة المتوقعة أو حالة أخرى تعرفها fixture.
4. حمّلها وارفعها عبر NeverD، وأصدر LLVM IR، ثم أعد تجميع النتيجة إلى شفرة آلة.
5. نفّذ الشفرة المعاد توليدها بنفس ABI والمدخلات وتخطيط الذاكرة ونموذج CPU.
6. قارن النتائج الملحوظة.

التنفيذ الأساسي هو
[`SemanticRoundTripFixture.h`](../../unittests/semantic/SemanticRoundTripFixture.h).
تستخدم fixture ‏patch-full ‏`Codegen::compileForRewrite`، وهو backend إعادة
الكتابة نفسه لعمليات patch، ثم تقارن الشفرة الأساس والمحوّلة عبر شبكة ISA/صيغة
الكاملة 4×3.

يجب أن يكون فشل NeverD الدلالي الحتمي اختبارًا فاشلًا. احصر skips في حدود قدرة
خارجية صريحة واقرأ سببها: لا يثبت ملخص أخضر بلا cross-linker أن مسار الصيغة نُفذ.

### واجهات EVM الخلفية التفاضلية

توفر اختبارات interpreter لـEVM oracle حتميًا بعرض 256 بت. تبني suite الـemitter
وتشغل LLVM، وتحوّل C23 عبر Clang إلى harness host نفسه، وعند توفر `solc` و`anvil`
و`cast` و`jq` تنشر Solidity مولدًا إلى Anvil محلي. تقارن status وstorage وعدد
تعليمات trace. ويشغل corpus raw-bytecode مستقل ALU قبل Fusaka ونسخ calldata/
memory و`MCOPY` المتداخل وKeccak وreturn data في EVM الأصلي لـAnvil.

تحفظ اختبارات Low/Med ‏execution lanes لكامل stack الحساسة للـpath وهوية lane في
phi؛ ويكون نفاد أي budget، ومنها `MaxAbstractInstructionTransfers`، hard error.
لا يرفض strict opcode مجهولة أوfork-inactive إلا على lane ثبت أنها `Reachable`،
ولا تنتج `MayReachable` حقائق مؤكدة. يقيد HighIR ‏selector/receive/fallback بالـroot
lane والـterminals الناجحة. لا يمثل selector مشترك دليل standard مستقلًا؛ ولا
تُختار variant وreturn list إلا من `KnownFunctionVariantInfo` الخاصة بالـstandard
وبعد اتفاق كل terminals الناجحة على return shape الدقيقة.

يجري interpreter ‏typed stack preflight قبل أي أثر خاص بالـopcode. يعرّف
`EVMForkSemantics.def` البايت `0x44` بأنه `DIFFICULTY` قبل Paris و`PREVRANDAO`
بدءًا من Paris. تعيد `REVERT` وfaults وstep limit ونفاد الموارد حالة transaction
إلى snapshot. يكون فشل allocation من النوع
`ExecutionFaultKind::ResourceExhausted`؛ وإذا تعذر حتى snapshot الدخول تكون
`HasPersistentStateSnapshot` بالقيمة false ولا يمكن commit للنتيجة.

### اختبارات انحدار حدود EVM العامة وميزانياتها

تعبث اختبارات الـAPI العامة كلًا على حدة بالـ
`Code`/`Fork`/`Instructions`/`JumpDestinations` القانونية وبكل table وrange وID
وlane وedge reference في LowIR. يجب أن يعيد `execute` ‏`llvm::Error` قبل lookup
التعليمات، وأن يرفض `lowerToMedIR` كامل LowIR الـmalformed أو المتجاوز للميزانية
قبل بناء index أو تخصيص output متناسب مع input. وتفرض tests في `lowerToMedIR`
ترتيب validation: options ثم resources ثم structure، وبعدها مقارنة field-by-field
عبر `canonical decode replay` وقبل `lowerCanonicalLowToMedIR`. يعيد public HighIR
recovery التحقق من LowIR/MedIR الخارجيين؛ ولا يستخدم
`lowerCanonicalLowToMedIR` و`recoverCanonicalHighIR` على IR القانوني الخاص إلا
`analyze`، بلا replay عودي أو مكرر ومع إبقاء HighIR option/resource budgets.
ثم تختبر حالات الـinterpreter
الحد الدقيق و+1 لكل حدود `EVMInterpreterLimits.def`: يحتفظ `MaxSteps` بـ
`StepLimit` المخصص، بينما يعيد نفاد `MaxMemoryBytes` أو `MaxTraceEntries` أو
`MaxLogEntries` أو aggregate ‏`MaxLogDataBytes` أو runtime
`MaxPersistentStateEntries` القيمة `ResourceExhausted` مع rollback لتأثيرات
المعاملة. يعد تجاوز aggregate أولي `MaxHostReturnDataBytes` أو persistent state
خطأ API. كما تعد مجاوزة `MaxCalldataBytes` أو aggregate
`MaxHostEnvironmentEntries` عبر `BlockHashes` و`Balances` و`CodeHashes` و
`ExternalCode` و`BlobHashes`، أو aggregate `MaxExternalCodeBytes`، خطأ API.
يرفضها `const execute preflight` قبل نسخ environment أو snapshot أو result. وتغطي
الاختبارات views ‏return-data من `ArrayRef` وlookup ‏`lower_bound`
في الجدول المرتب بلا نسخ buffer أو PC map.

تغطي اختبارات LowIR المنفصلة عند الحد الدقيق ميزانيتي diagnostic الـaggregate
`MaxLowDiagnostics` و`MaxLowDiagnosticBytes`: يحاسب linear decode وبناء CFG العدد
الدقيق والبايتات النهائية مسبقًا ويرفضان الصفر.
تغطي اختبارات أمان HighIR مجال `Any/Exact/Excluded` المرتب لكل lane، ومطابقة/
استبعاد equality، ومطابقة false edge وmismatch ‏true edge في
`XOR(selector, constant)` الخام، وتنقيح zero word/calldata size/call value،
والإغلاق عند unknown condition. كما تختبر الحدود الدقيقة و-1 في
`EVMAnalysisLimits.def`
`MaxHighDispatchCandidates` والـaggregate `MaxHighRecoveredArguments` و
`MaxHighDiagnostics` و`MaxHighDiagnosticBytes` و`MaxHighReferenceVisits` و
`MaxHighMemoryTransferCells` و`MaxHighMemoryValueVisits` من
`EVMAnalysisLimits.def`. ويجب أن يحاسب كل output diagnostic، بما فيه malformed
diagnostic الثابت، العدد والبايتات النهائية قبل allocation.
وتُختبر ميزانيتا diagnostic في LowIR وHighIR كل على حدة، ويجب أن تحاسب root CFG
region الافتراضية `MaxHighRegionBlockReferences` قبل reserve أو نسخ block PC.
تغطي regressions نطاق function كلا back-jump عبر `EQ` و`raw XOR` إلى dispatcher
مشترك، وتثبت أن function أخرى لا تلوث `arguments` أو`mutability` أو
`return shape` أو`region`، مع إبقاء shared bodies وtail calls قابلة للوصول.
تختبر نتائج CALL/CREATE الخارجية كـhost outcomes غير حتمية على حافتي CFG الدقيقتين،
فتبقى استعادة fallback في ERC-1167. ويظل selector condition غير المقروء Unknown ولا
يمكنه اختلاق facts لـfallback أو function.

تستمد اختبارات CFG ‏`InvalidJumpDestination` من `EVMLowFaultKinds.def` لحالة
`end-of-code JUMPI`: true المؤكد إلى هدف غير صالح definite fault بلا successful
tail، وfalse المؤكد ناجح، وunknown يحتفظ فقط بمسار false محتمل النجاح من دون وسم
lane كاملة كـdefinite fault.

تطبق اختبارات ABI حدود grammar من `EVMABIParserLimits.def` وحدود cardinality/text
للجداول العامة من `EVMABITableLimits.def` عند الحد الدقيق و+1. كما ترفض
kind/standard/evidence enums غير الصالحة وmetadata غير المتطابقة وsignature/return
list غير القانونية وshared selector الموسوم independent خطأً وvariant المعلق أو
المكرر وevent-topic ‏`APInt` غير بعرض word، قبل indexed selector أو sorted topic
lookup.

يفرض `NeverDEVMOpcodeTests` بنية metadata أيضًا: تدور كل opcode مخصصة بين encoding
وtyped value، وتُختبر حدود العائلات وaliases للـhardfork، وتبقى maxima لعقد stack
وhost arguments مشتقة بدل تكرارها في backends.

### واجهات Solana SBF الخلفية التفاضلية

تتحقق اختبارات بيانات SBF الوصفية من كل ميزة مرتبطة بالإصدار، وحدود تصادم opcodes، وhash ‏Murmur3 لـ syscall، وrelocations، وثوابت ELF machine والسجلات وعناوين VM. تولّد fixtures الخاصة بالـ loader، من دون ثنائيات مضمّنة، تخطيطات sections القديمة لـ v0-v2 وتخطيطات v3/v4 الصارمة الخالية من sections والمعتمدة على program headers.

يفحص `NeverDSBFISAConformanceTests` كل byte encoding في كل version من v0 إلى
v4 مقابل typed manifest مدقّق بصورة مستقلة. ثم يقارن
`NeverDSBFExternalOracleTests` قرارات activation وboundary مع official Anza
process مبني بصورة منفصلة. ويعيّن `NeverDSBFUpstreamConformanceTests` نتيجة
صريحة لكل ملفات ELF الثلاثة والعشرين عند Anza revision المثبتة.

ينفّذ `NeverDSBFSemanticTests` بايتات التعليمات المتحقق منها مباشرة ولا يستهلك MedIR، لذلك لا يمكن لتغيير IR المطبّع أو إفساده أن يجعل oracle المصدر يتفق مصادفةً مع backend. ويغطي دلالات v2 غير الرتيبة، والذاكرة، وsyscalls، وإطارات الاستدعاء الداخلية، وfaults، وtraces، وحدود الموارد. يجري التحقق من وحدات LLVM؛ ويُجمّع C المولد مع اعتبار warnings أخطاء، وRust مع `-D warnings`. تمر اختبارات API العامة عبر جميع مراحل IR والتفكيك وCFG والبيانات الوصفية وLLVM وC وRust بدءًا من ملف SBF ELF صارم مولد.

## أهداف بأمر واحد

تبني الأهداف المخصصة تبعياتها ثم تشغّل CTest بتوازٍ مشتق من CPU المضيف:

| هدف CMake | الاختيار |
|-----------|----------|
| `check-neverd` | كل الاختبارات المسجلة |
| `check-neverd-semantic` | `NeverDSemanticTests` فقط |
| `check-neverd-sbf` | جميع أهداف/حالات `NeverDSBF*Tests` |
| `check-neverd-patch-full` | `NeverDPatchFullTests` فقط |
| `check-neverd-switch-xform` | `NeverDSwitchXformTests` فقط |
| `check-neverd-cfgloop-xform` | `NeverDCFGLoopXformTests` فقط |
| `check-neverd-twotable-xform` | `NeverDTwoTableXformTests` فقط |

```bash
cmake --build build-release --target check-neverd
cmake --build build-release --target check-neverd-semantic
cmake --build build-release --target check-neverd-sbf
```

لا يملك `NeverDIndCallXformTests` و`NeverDAvxUpperXformTests` حاليًا هدف راحة
`check-neverd-*`. ابنِهما واخترهما بالوسم كما أدناه. كذلك لا يتضمن
`check-neverd-semantic` ثنائيات التحويل أو patch-full المنفصلة؛ استخدم
`check-neverd` للتجميع الكامل.

## سير CTest التزايدي

ابنِ الملف التنفيذي المالك أولًا ثم اختر وسمه. يتجنب ذلك إعادة ربط أهداف
دلالية كبيرة غير مرتبطة.

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

# جميع أهداف/حالات EVM المحددة
cmake --build build-release --target \
  NeverDEVMOpcodeTests NeverDEVMBytecodeTests NeverDEVMLoaderTests \
  NeverDEVMABITests NeverDEVMAnalyzerTests NeverDEVMDecoderPropertyTests \
  NeverDEVMProxyTests NeverDEVMCallTests NeverDEVMSemanticTests \
  NeverDEVMEmitterTests \
  NeverDEVMIntegrationTests --parallel 4
ctest --test-dir build-release --build-config Release \
  -R 'EVM' --output-on-failure --parallel 4

# جميع أهداف/حالات Solana SBF المحددة
cmake --build build-release --target check-neverd-sbf --parallel 4
```

استخدم اسم CTest مشتقًا من GoogleTest لانحدار واحد:

```bash
ctest --test-dir build-release --build-config Release -N \
  -L '^NeverDLiftTests$'
ctest --test-dir build-release --build-config Release \
  -R '^COFFARMPipeline\.ARM32ThumbLiftAndDecompile$' \
  --output-on-failure
```

محددات مفيدة:

| الأمر | الغرض |
|-------|-------|
| `ctest --test-dir build-release -N` | سرد الحالات المكتشفة دون تشغيلها |
| `ctest --test-dir build-release -L '<regex>'` | اختيار وسم ثنائي اختبار |
| `ctest --test-dir build-release -R '<regex>'` | اختيار أسماء الحالات |
| `ctest --test-dir build-release --output-on-failure` | عرض التشخيص عند الفشل فقط |
| `ctest --test-dir build-release --stop-on-failure` | التوقف بعد أول فشل |
| `ctest --test-dir build-release --parallel 4` | تشغيل أربع حالات بالتوازي كحد أقصى |

يستخدم اكتشاف GoogleTest ‏`DISCOVERY_MODE PRE_TEST`، لذلك يجب أن يوجد ثنائي
الاختبار المطابق قبل تعداد CTest. تُعرّف مهلات الحالة والاكتشاف المنفصل في
`cmake/AddNeverD.cmake`، ولا تُوسّع إلا لمجموعات ذات حالات ثقيلة مقاسة.

## أي اختبارات يجب أن تتغير مع الشفرة؟

| منطقة التغيير | ابدأ بـ | ثم فكّر في |
|---------------|---------|------------|
| lifter العمارة أو decode | الحالة المسماة في `NeverDLiftTests` | دورة دلالية لـ ISA المطابقة |
| LowIR CFG واكتشاف الدوال وجداول القفز | حالات lift ‏CFG/switch | `NeverDSwitchXformTests` أو `NeverDCFGLoopXformTests` أو `NeverDTwoTableXformTests` |
| MedIR وABI والأعلام والأنواع وSSA | حالات lift ‏MedIR/أعراف الاستدعاء | حالات `NeverDSemanticTests` العابرة لـ ISA |
| HighIR أو C المنظمة | حالات HighIR/decompile | `NeverDCFGLoopXformTests` وفحوص تجميع C المولدة |
| loader ‏PE/ELF/Mach-O أو relocation الإدخال | fixture الصيغة المطابقة في `unittests/lift` | اختبار تحميل/إعادة تجميع كل المراحل للخلية |
| Rewrite codegen أو relocation الإخراج | حالات `RewriteCodegenRTTests` | `NeverDPatchFullTests` وfixture ‏patch مرتبطة عند توفرها |
| تحويل LLVM IR يستخدمه patch | ثنائي التحويل المحدد | شبكة pass المركبة لـ `NeverDPatchFullTests` |
| C API أو CLI | اختبار SDK/query مباشر و`unittests/semantic/CLIEndToEndTests.cpp` | مجموعة pipeline/صيغة ذات الصلة |
| ‏EVM loader أو opcode أو IR أو backend | أصغر هدف مالك من `NeverDEVM*Tests` | جميع أهداف EVM مع فحوص تجميع C/Solidity المولدين |
| ‏SBF loader أو ISA أو IR أو backend | أصغر هدف مالك من `NeverDSBF*Tests` | جميع أهداف SBF مع فحوص تجميع C/Rust المولدين |
| تعرف libc | `NeverDLibCTests` | حالات call/ABI دلالية إذا تغير السلوك |
| تدقيق عمر الكومة أو صيد فيضان النسخ | `NeverDSafetyTests` | الخلايا الست كلها في `NeverDSafetyIntegrationTests` |
| تنفيذ العمليات أو quoting | `NeverDTestProcessTests` | حالة CLI/دلالية متأثرة على كل مضيف مدعوم |

يجب أن تعبر الاختبارات عن العقد عند أدنى حد مستقر. يفيد اختبار شكل LowIR في
نسب السلوك إلى lifter؛ وتلزم دورة دلالية إذا كان لشكلين IR معقولين سلوك مختلف.
تجنب golden dump لدوال كاملة عندما يكفي assertion صغير على opcode أو CFG أو
حالة ملحوظة.

## العلاقة مع CI

تبني CI ‏Release مع الاختبارات على Linux وmacOS وWindows، ثم تدقق المخزون
المكتشف قبل تطبيق استثناءات الوسوم الخاصة بالمنصة. تُعرّف الملفات في
`.github/workflows/ci.yml` و`scripts/audit_ci_test_inventory.py`. يجب أن يضم كل
مضيف في المصفوفة `NeverDSafetyTests` و`NeverDSafetyIntegrationTests`؛ وتقرأ كل
عملية تشغيل fixtures نفسها المثبتة لـ PE وELF وMach-O على x86-64 وAArch64.
ولأن لا shard واحدًا من المصفوفة يمثل كل المجموعات المكلفة، يبقى
`check-neverd` المحلي أوضح إشارة كاملة قبل الدمج عندما تملك الآلة كل الأدوات
العابرة اللازمة.

## ملف مطابقة وتعقيم Solana SBF الحالي

تحل هذه القائمة الحالية محل قائمة SBF المختصرة أعلاه. تحتاج اختبارات source
differential إلى `rustc` بالإضافة إلى clang؛ تخطي compiler يعني coverage ناقصة.
يشمل التجميع الكامل `NeverDSBFProgramImageTests` و
`NeverDSBFMalformedCorpusTests` و`NeverDSBFISAConformanceTests` و
`NeverDSBFUpstreamConformanceTests` و`NeverDSBFLLVMDifferentialTests` و
`NeverDSBFSourceDifferentialTests` مع targets metadata/loader/analyzer/semantic/
emitter/integration. يسجل profile المتكامل الأهداف المسماة ونتائجها ولا يثبت
عدداً تجميعياً سريع التغيّر.

يجب بناء profile sanitizer في `build-sbf-asan-ubsan` منفصلًا. تحتوي package
الجاهزة المثبتة ذات الإصدار المحدد الآن fork-only header المطلوب، لذلك يعمل
integration في profile نفسه بإعدادات ASan وUBSan بنمط fail-fast.

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

### لقطة أدلة SBF المثبتة (2026-08-24)

تثبت البوابة Anza `sbpf` عند
`2510663bb8d894e8e3094be351e4bb4b604f1f84` وAgave عند
`ef210d67f2fabeee1730498188fa78854260c679` وSolana SDK عند
`122f32e571ce39face4beffaccea733e37c207fd`. يمر ELF manifest الرسمي 23/23؛
ويقارن `NeverDSBFExternalOracleTests` عدد 1,411 من حالات opcode/boundary عبر
`SBFOfficialOracleProtocol.def` و`SBFOfficialVerifierCases.def` و`SBFOfficialExecutionConstants.def`.
`SBFOfficialELFMutations.def` هو عقد malformed ELF الجدولي، ولا يثبت المستند
عدداً إجمالياً متغيراً.
وبشكل مستقل يشغّل `41-case strict ELF differential` كامل مصفوفة strict-v3 عبر
`verify-elf-batch` الرسمي وNeverD؛ ولا تدخل الحالات الـ41 في مجموع 1,411.
ويصادق `NeverDSBFAgaveConformanceTests` على corpus ‏Firedancer test-vectors عند
`68bb4af40235562e8852fa23d5727e49c2a0b862` ويطابق كل fixtures الـloader البالغ
عددها 1,955 `sol_compat_elf_loader_v1` (قبول 1,399 ورفض 556)، ويطابق لكل ELF مقبول
`entry_pc` و`text_off` و`text_cnt` و`rodata_hash` و`calldests_hash`. ولا تشغّل هذه gate
الـinstruction verifier اللاحق.

مصفوفة التنفيذ الرسمية الإضافية مستقلة: تضم بالضبط 508 حالات فعالة من
`(Version,Opcode)` و58 حالة boundary، أي 566 حالة تنفيذ دقيقة. وهي لا تستبدل
ولا تدخل ضمن 1,411 من اختبارات verifier أو `41-case strict ELF differential`.
تستخدم Linux Release CI الخيارات `--print-pinned-revision` و
`--print-test-vectors-revision` و`--print-toolchain`، وتصدر
`NEVERD_SBPF_ORACLE` و`NEVERD_AGAVE_CONFORMANCE_ROOT`، فتكون البوابتان
الخارجيتان إلزاميتين؛ محلياً، غياب env الصريح للـoracle/corpus يسمح باكتشاف
الحالات ثم skip.

تجعل rows باسم `SBF_RUNTIME_VERSION` نطاق
`RuntimeVersionPolicy::ChainProfile` تاريخياً حسب cluster/slot: ينتقل maximum
ISA من V0 إلى V1 ثم V2 ثم V3 مع تفعيل feature accounts الرسمية، والحالي V3.
أما v4 الصريح فيستخدم `RuntimeVersionPolicy::UpstreamToolchain` للتحليل offline. حد 10 MiB
الحالي هو بالضبط `10'485'760` byte، و65,536 provenance/test تاريخي غير منفذ.
يثبت `SBFFaultCodes.def` قيم execution fault؛ أما `SBFSourceStatuses.def` فيثبت
generated-source ABI المستقل.

تحرس fixtures بحجم 10,000 خصائص worklist/function ownership/multi-latch من دون
تثبيت زمن جهاز. وتتيح rows الخاصة بالـcluster/account/slot تنفيذ
`RPC activation audit` مع بقاء الاختبارات العادية deterministic وoffline.

## أدلة تصدير SDK للأجهزة المحمولة

يشغّل سير العمل اليدوي `Mobile SDK Export Evidence` الأمر `collect_mobile_ios_sdk_declarations.py --exports-only` على إصدارات Xcode SDK المثبتة. ويحتفظ بخرائط الرابط الأصلية لكل من Foundation وCoreFoundation وUIKit من حزم iOS للأجهزة والمحاكي، مع الهدف وإصدار SDK وبصمة إعداداته وحجم الملف وSHA-256. ويحتفظ جامع التصريحات المعتاد بهذه الخرائط أيضًا. يفشل الجمع عند غياب الملفات أو فراغها أو تجاوز حجمها الحد أو وجودها خارج SDK، مع الاحتفاظ بالأدلة المكتملة. تثبت خرائط الرابط تصدير الرموز، لكنها لا تثبت ABI الاستدعاء أو نجاح استعادة الطريقة.

## أدلة ABI لسلاسل Swift على الأجهزة المحمولة

يُصرّف سير العمل اليدوي `Mobile Swift String ABI Evidence` مسبارات Swift ثابتة للمساواة والترتيب ومسبار C يستخدم `swiftcall` بواسطة Xcode 26.5 لأجهزة iOS ومحاكياتها بمعمارية arm64. يحتفظ `collect_mobile_swift_string_abi.py` بالمصدر وLLVM IR والتجميع وهوية المصرّف وإعدادات SDK و`libswiftCore.tbd` مع بصماتها. يجب أن تُظهر اللغتان استيراد المقارنة الدقيق بخمسة معاملات ونتيجة `i1`؛ ويجب أن يوسّع C هذه النتيجة صراحةً إلى بايت. تؤدي الأهداف الخاطئة والتواقيع المتغيرة وفشل الأوامر والمهل المنتهية إلى فشل الجمع مع حفظ الأدلة الجزئية. لا تثبّت هذه الأدلة تصريحًا لوقت التشغيل ولا تثبت استعادة طريقة. اختبر المجمّع دون SDK باستخدام `python3 -m unittest scripts.tests.test_mobile_swift_string_abi`.

## تبسيط MBA المعياري

تغطي `SymReadability.*` كتابة الطرح والمتمّم، وكلفة العوامل التجميعية، والثوابت ذات البت الواحد والعريضة، وتشبع الأشجار المشتركة، واختيار المرشّحات ضمن ميزانية، والتكافؤ الشامل لثلاثة بتات مع تعطيل أخذ العينات. تقارن `SymMBASample.*` التحقق الضيق وذا الدقة غير المحدودة بمقيّم AP، بما يشمل كل العوامل والإسنادات الحتمية والمدخلات العريضة غير المستخدمة. عند مقارنة جودة المرشّحات بين إصدارات التقييم، يجب إعادة حساب المخرجين بالمقياس نفسه؛ عدادات الحجم الخاصة بإصدار SDK للتشخيص فقط.

## مصفوفة اختبارات ARM32 وتمرير الإطار

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

تشمل المصفوفة أيضًا x86-32 بصيغ ELF/COFF/Mach-O، وARM32 بنمطي ARM وThumb في ELF، وAArch64 بصيغ ELF/COFF/Mach-O، مع خلفيتي C. يجب اختزال إعادة تحميل الإطار الخاص المتكررة إلى جمع أو طرح وأن تنفذ بصورة صحيحة لأزواج البايتات وحدود الكلمات والكلمات العشوائية المحددة عند مستويَي التحسين. تفحص اختبارات Clang AST الدوال الكاملة بحثًا عن عمليات MBA المتبقية مع التمييز بين العناوين الصحيحة. يغطي HighFrameStoreForwarding عرض الوصول وتغير المتغيرات المحلية وكتابات الذاكرة والتداخل والذاكرة المرتبة والرسوم غير الصحيحة وحدود التوسع. تحفظ اختبارات HighCStoreForwarding تعريفات قيم التخزين الحية عبر المعماريات الأربع، وتغطي SymSimplifyGuard هوية التحميل وترتيبه وحالات volatile/atomic وحدود poison. تتحقق اختبارات ELFARM32ModeTest من اختيار ARM/Thumb وتطبيع العناوين وحفظ بيانات الصور المختلطة ورفض الأدلة المتناقضة؛ وتتحقق ELFARM32ModeCAPITest من رفض SDK الصريح ثم استعادة المفكك بعد إعادة تحميل Thumb. تغطي InstructionMode حدود المفكك والمؤشرات والفروع وتوليد الكود. غياب Clang للهدف الآخر يعني تخطي الاختبار، وليس إثبات نجاح الصيغة.

## الاستثناءات المتزامنة الأصلية في x64

تستخدم تعليمات `DIV`/`IDIV` في checked x64 نتائج المعالج الحقيقية و`#DE`. يعتمد KVM على IDT/IST خاصة بالمشرف، ويستخدم WHP خريطة اعتراض صريحة؛ يُحتفظ بالسياق الأصلي وأكواد الخطأ المتاحة بصورة مستقلة عن أخطاء النقل. يستهلك نموذج نظام التشغيل الحدث القابل للاستعادة قبل تثبيت سياق المتابعة. يحوّل نموذج برامج تشغيل Windows القسمة على صفر وفيض خارج القسمة إلى `STATUS_INTEGER_DIVIDE_BY_ZERO` مع تنفيذ مرشحات SEH و`__finally` وإعادة المحاولة فعليًا. يُبنى `NeverDX64ExceptionTests` دون Unicorn، ويتحقق `DriverWDMCPUException` من أمثلة WDK الأصلية. تُتخطى مضيفات WHP/ARM64 غير المتاحة صراحةً.

## آثار RAM المرحلية

يحتفظ `RAMTransaction` فقط باتحاد نطاقات الكتابة الفيزيائية المعلنة للتعليمة وتحت قفل التنفيذ. تُستعاد الذاكرة الأصلية قبل مراقبي النتائج؛ الإلغاء وفشل النقل واستثناءات المراقب لا تنشر RAM أو سجلات جزئية. تحتفظ أخطاء المعالج بحالة الاستثناء المعمارية بعد تراجع RAM. تستخدم كتابات ARM64 المفردة والمزدوجة نفس السلطة. تنفذ x64 تعليمات `XCHG` و`XADD` و`CMPXCHG` بعروض 8/16/32/64 بت، مع محاذاة طبيعية للأشكال المقفلة أو ذات القفل الضمني. تقارن اختبارات `NeverDRAMTransactionTests` النتائج بمعالج المضيف وتختبر التراجع والمرادفات والصلاحيات؛ تُتخطى المنصات غير المتاحة صراحةً. تبقى الأجهزة وSMP المتوازي خارج هذا العقد، ولا تستعيد لقطات CPU ذاكرة سبق اعتمادها.

## حالة x87 الكاملة

تملك `NeverDEmulationArch` عقود ISA وجداول الصفحات وتنسيق FP المشترك بين النقل الأصلي وUnicorn. تحفظ سياقات x64 التحكم والحالة وTOP والوسوم الفيزيائية ورمز العملية ومؤشرات التعليمات/البيانات وثمانية سجلات بطول 80 بت. تستخدم `FP0`–`FP7` نوع `RegisterValue` ويرفض الوصول العددي الاقتطاع. يمثل `FPTag` قناع السجلات الفيزيائية غير الفارغة. تتحقق `NeverDX64FPTests` من كل قيم TOP والعمليات الدقيقة بمقارنتها مع FXSAVE/FXRSTOR للمضيف واستعادة السياق. لا يتيح ذلك تعليمات x87 ضمن checked ولا يثبت جميع حالات التقريب. يجري تجاوز المضيفات الأصلية غير المتاحة صراحةً.
