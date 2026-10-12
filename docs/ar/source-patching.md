**Languages**: [English](../source-patching.md) | [简体中文](../zh-CN/source-patching.md) | [繁體中文](../zh-TW/source-patching.md) | [日本語](../ja/source-patching.md) | [한국어](../ko/source-patching.md) | [Français](../fr/source-patching.md) | [Deutsch](../de/source-patching.md) | [Español](../es/source-patching.md) | [Italiano](../it/source-patching.md) | [Русский](../ru/source-patching.md) | [العربية](source-patching.md)

<!-- i18n-source: b504f377f29d587519311580a92c9cdaed9118057832a8afd81551fbd34802df -->

[← Documentation Index](README.md)

# تصحيحات المصدر والكائنات الأصلية

يترجم NeverD الدالة البديلة بالمترجم المختار ويثبتها عند مدخل الدالة الأصلية المحدد صراحةً. يتولى المترجم دلالات C/C++، ويتولى NeverD ربط العناوين وإعادة التموضع والتخزين وسجلات فك المكدس والنشر. يحتفظ إصلاح الثنائيات وإعادة كتابة الشيفرة المرفوعة بمساراتهما. لا يعاد بناء بقية البرنامج.

<!-- i18n-section: compiler -->
## اختيار المترجم

اختر `--compiler=clang` أو `--compiler=gcc` أو `--compiler=neverc`. يستطيع Clang وNeverC في الوضع العادي إنتاج LLVM IR؛ وتحافظ صيغة `object` على التعليمات الأصلية المنتجة. تختار CLI الكائنات افتراضياً مع GCC. يتطلب Python SDK تحديد `artifact="object"` صراحةً مع GCC. لا يحدث انتقال صامت إلى مترجم آخر.

استبدل عناوين الأمثلة بمداخل أصلية مؤكدة. يجب أن يطابق تصريح البديل ABI الأصلي بما فيه اتفاقية الاستدعاء وتخطيط البنى؛ لا يثبت العنوان ذلك. تتطلب C++ الخيار `--language=c++` ومسار Clang++/G++ الصحيح. يفضل مدخل `extern "C"` أو الاسم المزخرف الدقيق. يسمح بدوال مساعدة إضافية. يحدد `--entry` و`--func` البديل صراحةً، ويحترم `--from-c` القديم العنوان أيضاً.

```sh
# Clang C -> LLVM IR
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=clang --compiler-path=/path/to/clang \
  --patch-report -o app.clang

# NeverC ordinary C -> native object
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=neverc --compiler-path=/path/to/neverc \
  --artifact=object --patch-report -o app.neverc

# GCC -> native object (CLI default for GCC)
neverd patch app --from-source=replacement.c --entry=replacement \
  --func=0x401140 --compiler=gcc --compiler-path=/path/to/gcc \
  --patch-report -o app.gcc

# Independently compiled ELF x64 object
clang --target=x86_64-unknown-linux-gnu -fPIC -fno-lto -c replacement.c -o replacement.o
neverd patch app --from-object=replacement.o --entry=replacement \
  --func=0x401140 --patch-report -o app.object
```

<!-- i18n-section: profiles -->
## الملفات المتحققة والقيود

| المترجم | الناتج | ملف تنفيذ نظام التشغيل المتحقق |
|---|---|---|
| Clang 22.1.6 | LLVM IR | ELF x64 C/C++؛ Mach-O x64/ARM64 C/C++؛ PE x64 C/C++ FH3 |
| Clang 22.1.6 | كائن أصلي | ELF x64 C/C++ |
| GCC/G++ 13.3.0 | كائن أصلي | ELF x64 C/C++ مع بيئة Itanium متوافقة |
| NeverC 3389.1.5 الوضع العادي | LLVM IR | ELF x64 C؛ Mach-O x64/ARM64 C |
| NeverC 3389.1.5 الوضع العادي | كائن أصلي | ELF x64 C |

هذه ملفات جرى التحقق منها، وليست ضماناً لكل إصدار أو خيار مترجم. لا يدعم `dyncode` ولا NeverC C++. لا ينتج GCC صيغة LLVM IR. لا تقبل كائنات COFF/Mach-O الأصلية؛ يتطلب تثبيت الكائنات ELF x64. تؤدي عمليات إعادة التموضع المجهولة وTLS والمنشئات وLTO ورموز common/IFUNC والاستيرادات غير المحلولة إلى فشل واضح. يسجل إيصال JSON هوية المترجم والوسائط والروابط والعناوين المثبتة، ويسجل SHA-256 أيضاً للكائنات الأصلية.

تغطي مصفوفة CPU المنفصلة أربع معماريات (`x86` و`x64` و`ARM32` و`ARM64`) وثلاث صيغ (`ELF` و`PE` و`Mach-O`)، وتتحقق من تثبيت المصدر أو الرفض الصريح. لا يثبت تنفيذ CPU المعزول صحة محمل النظام أو فك مكدس C++ أو أي ABI عشوائي. تعتمد هذه الاستنتاجات على ملفات التنفيذ أعلاه.

| Format | `x86` | `x64` | `ARM32` | `ARM64` |
|---|---|---|---|---|
| `ELF` | `CPU` | `CPU` | `REFUSED` | `CPU` |
| `PE` | `REFUSED` | `CPU` | `REFUSED` | `REFUSED` |
| `Mach-O` | `REFUSED` | `CPU` | `REFUSED` | `CPU` |

تعني `CPU` تثبيت المصدر وتنفيذ الدالة المعزولة، وتعني `REFUSED` رفضاً صريحاً لغياب عقد التثبيت. تحتاج ARM32 ELF إلى تسجيل EHABI؛ وتحتاج Mach-O ذات 32 بت إلى عقود محمل وواجهة ترجمة وفك مكدس متحققة؛ ويتطلب تثبيت مصدر PE حالياً x64.

تستخدم مصفوفة CPU حسابات وفروعاً واستدعاءات مساعدة مستقلة. لا تتحقق من مراجع بيانات ELF x86/ARM64 أو استيرادات C++ ARM64؛ وقد رفضت الاختبارات الإضافية عقود إعادة التموضع غير المدعومة لهذه الحالات.

<!-- i18n-section: references -->
## مراجع البرنامج الأصلي والتخزين

تطابق الروابط تصريحات خارجية: يتطلب `function` دالة أصلية أو مدخل استيراد مؤكداً؛ ويتطلب `data` تخزيناً قابلاً للكتابة؛ ويتطلب `const` تصريح IR ثابتاً. تحتاج روابط بيانات الكائن إلى امتداد غير صفري. لا يجوز تجاوز حدود التخزين أو منح الكتابة إلى RELRO. يرفض استدعاء الجسم الأصلي للدالة المستبدلة؛ لا يحتوي هذا الملف على قفزة رد تحفظ مقدمة الدالة.

تبقى الشيفرة الجديدة RX والحالة القابلة للتعديل RW. يعاد استخدام الاستيرادات الموجودة فقط، مع التمييز بين خانات GOT/IAT ومداخل الاستدعاء. يحتاج Mach-O إلى مساحة لأوامر التحميل مثل `-Wl,-headerpad,0x400`؛ ويحتاج PE إلى مساحة لرؤوس الأقسام. يفشل نقص المساحة. تتطلب ELF `.data.rel.ro` وحماية CET/IBT الأصلية عقوداً إضافية وترفض حالياً.

```sh
neverd patch app --from-source=replacement.c --entry=replacement --func=0x401140 \
  --bind=function:original_helper=0x401190 \
  --bind=data:shared_counter=0x404020:4 \
  --bind=const:lookup_table=0x402040:64 \
  --patch-report -o app.bound
```

يتطلب نمو جداول فك المكدس Mach-O أيضاً مساحة ملف ومساحة افتراضية مجاورتين غير مستخدمتين؛ لا تحجزها مساحة الرأس وحدها. تميز العينات المتحققة بين الشرطين.

<!-- i18n-section: exceptions -->
## استثناءات C++

تستخدم ELF بيانات FDE/LSDA للمترجم، وتستخدم Mach-O صيغة compact unwind/DWARF. تقبل PE x64 جداول FH3 للمترجم مع بيئة MSVC أصلية متوافقة وهويات أنواع متطابقة. قد تحتاج بيانات type-info إلى روابط صريحة. ترفض FH4 وpersonality المخصصة ومصدر SEH وأوضاع CFG/EHCont/XFG النشطة مع الحفاظ على أعلام الحماية الأصلية.

تغطي حالات التنفيذ العودة العادية وcatch الداخلي واستثناءً جديداً يمسكه المستدعي الأصلي واستثناءً يمر من الدالة الأصلية عبر البديل وتنظيف RAII مرة واحدة وإعادة الرمي وإنهاء noexcept والدوال غير المعدلة. يجب أن يسمح موضع الاستدعاء الأصلي بانتشار الاستثناء. لا يعيد التصحيح مسارات catch التي أزالها المترجم حول دالة ثبت أنها لا ترمي.

<!-- i18n-section: publication -->
## SDK والنشر

واجهة C الإضافية هي `neverd_patch_source` مع `neverd_source_patch_options_v1`؛ وتعيد `neverd_patch_source_report` الإيصال. يقدم إدخال الكائن طوله بالبايت. تبقى `neverd_patch_from_c` القديمة متاحة.

تؤخذ لقطة للمدخلات، ويجهز المرشح بجوار الوجهة، ويعاد تحميله وتدقيق كل مدخل مطلوب قبل النشر الحصري. لا تستبدل الملفات الموجودة ولا أسماء الروابط الرمزية. يؤدي غياب تعيين واحد إلى فشل الطلب كله. يرفض `--mode=inplace`. لا تطبق تحويلات IR للجلسة على الكائنات الأصلية. تتحقق Mach-O من التوقيع وتجدد توقيعه بدقة؛ ولا يحذف توقيع PE بصمت.

```python
report = session.patch_source(
    replacement_source, "app.patched",
    mappings={"replacement": original_address},
    compiler="gcc", compiler_path="/path/to/gcc", artifact="object",
    bindings={"shared_counter": ("data", original_counter_address, 4)},
)
```

<!-- i18n-section: verification -->
## البناء والتحقق

حدد مترجماً متوافقاً عبر `NEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE` (جرى اختبار Clang 22.1.6). يصحح البناء المتكامل نسخة LLVM داخل دليل البناء فقط، دون تعديل الوحدة الفرعية أو ذاكرة الحزم المشتركة. قد يرفض خطأ x64 PLT في حزم r3/r4 تصحيح IR؛ تتجنب الكائنات الأصلية هذا المسار MC. يشترط `NEVERD_TEST_REQUIRE_FIXED_LLVM=1` إنتاج PLT الدقيق المصحح.

تحتوي `NeverDSourcePatchTests` على حالات C/API/الكائنات/الاستثناءات الأصلية؛ وتفحص `NeverDSourcePatchMatrixTests` كل التركيبات باستخدام Unicorn. تنفذ `SourcePatchPE` سبعة سيناريوهات C++ عند تحديد `NEVERD_TEST_WINDOWS_CRT_LIB` و`NEVERD_TEST_WINDOWS_UCRT_LIB` و`NEVERD_TEST_WINDOWS_SDK_LIB`. يحتاج المضيف غير Windows أيضاً إلى `NEVERD_TEST_WINDOWS_RUNNER`. يجعل `NEVERD_TEST_REQUIRE_WINDOWS_RUNTIME=1` المدخلات الناقصة فشلاً. يشترط سير عمل Windows المخصص التنفيذ الأصلي ويرفض الحالات المتخطاة.

تستخدم أدلة Linux على Mac التطوير Docker ومحمل Linux صريحاً؛ وتستخدم PE بيئة Wine 11. تنفذ Mach-O ARM64 على Mac نفسه. يجب فحص نتائج CI الأصلية لـWindows بشكل منفصل؛ فإعداد المهمة لا يعني نجاح التنفيذ. توجد الأعداد التاريخية وإخفاقات الأساس وحدود المحاكيات في [العقود التفصيلية وسجل التحقق](../source-patching-details.md).

```sh
cmake -S . -B build-source -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_TESTING=ON -DNEVERD_LLVM_PREBUILT=OFF \
  -DNEVERD_ENABLE_SEMANTIC_TESTS=ON \
  -DNEVERD_SOURCE_PATCH_TEST_CLANG_EXECUTABLE=/path/to/clang
cmake --build build-source --target NeverDSourcePatchTests \
  NeverDSourcePatchMatrixTests --parallel 4
ctest --test-dir build-source -L \
  '^(NeverDSourcePatchTests|NeverDSourcePatchMatrixTests)$' \
  --output-on-failure --parallel 4
```

تصحح نسخة LLVM أيضاً مراجع بيانات GOT في Mach-O x64؛ تقاس جداول catch ذات النوع في Clang من نهاية حقل إعادة التموضع ذي أربعة بايتات. تزحزحه r3/r4 أربعة بايتات، وترفضه مراجعة ما قبل النشر. تنفذ حالات x64 على Mac عبر Rosetta 2، وحالات ARM64 مباشرةً.

اجتاز التحقق في بيئة Windows x64 الأصلية [96cf7cd59](https://github.com/NeverSight/NeverD/actions/runs/38200205814) جميع الاختبارات المتخصصة الخمسة عشر لمسارات PE والكائنات وtrampoline، وجميع اختبارات مصفوفة الأهداف الثمانية والأربعين، دون تخطي أي حالة. نفّذ اختبار FH3 سبعة سيناريوهات تشغيل C++. استُخدم Clang 22.1.6 مع MSVC وWindows SDK الأصليين.
