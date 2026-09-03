# تحليل شامل لمشروع ProsperoLayer RDNA2 Core

**تاريخ التحليل:** 3 سبتمبر 2026  
**الإصدار المحلل:** v19.0.0 (Round 30 - expanded)  
**حجم الأرشيف:** 2.8 ميجابايت (292 ملفاً)

---

## 1. نظرة عامة

**ProsperoLayer RDNA2 Core** هو مشروع بحثي أولي لمحاكاة PlayStation 4/5، مبني على هندسة C++20 ويستهدف Linux x86-64. المشروع مبني على أساس مشروع **Kyty** كمحاكي هجين (HLE). الهدف البحثي هو بناء نواة قابلة للاختبار والتحقق، وليس محاكياً تجارياً كاملاً.

> ⚠️ المشروع يصرّح بوضوح أنه **ليس محاكياً كاملاً لـ PS5** ولا يدّعي توافق أي لعبة تجارية، ولا يتضمن ألعاباً أو فيرموير أو مفاتيح أو برمجيات نظام مملوكة.

---

## 2. الإحصائيات

| المعيار | القيمة |
|---------|--------|
| **عدد الملفات الإجمالي** | 292 |
| **إجمالي أسطر الكود** (src + include + libs) | ~111,041 |
| **أسطر src/** | ~26,094 |
| **أسطر include/** | ~22,432 |
| **أسطر libs/** | ~62,515 |
| **أسطر tests/** | ~51,268 |
| **ملفات الاختبار** | 57 |
| **رموز HLE مسجّلة (LIB_FUNC)** | 1,633 |
| **وحدات HLE** | 30 وحدة |
| **Binary مبنية مسبقاً** | 2 (ps5_native_vulkan_emulator + prospero-run) |

---

## 3. بنية المشروع

```
prosperolayer-rdna2-core/
├── include/          # الهيدرز (التعريفات)
│   ├── common/       # أدوات مشتركة (logging, file, threads, ...)
│   ├── cpu/          # معالج x86-64 (مفسر، JIT، SIMD، syscalls)
│   ├── gpu/          # واجهة GPU (PM4, Vulkan, GCN, RDNA2, SPIR-V)
│   ├── graphics/     # رسوميات الضيف والمضيف
│   ├── kernel/       # نواة المحاكاة (memory, sync, pthread, ...)
│   ├── loader/       # محمّل ELF / SELF / runtime linker
│   ├── memory/       # مدير الذاكرة الافتراضية (VMM)
│   └── 3rdparty/stb/ # مكتبات طرف ثالث (stb_image, stb_truetype)
├── src/              # الكود المصدري
│   ├── cpu/          # تنفيذ المعالج (مفسر + JIT + syscalls)
│   ├── gpu/          # محرك الرسوميات (Vulkan, PM4, GCN, rasterizer)
│   ├── kernel/       # تنفيذات النواة (events, semaphores, fs, posix)
│   ├── loader/       # ELF loader, SELF parser, runtime linker
│   ├── memory/       # VMM
│   ├── common/       # أدوات مشتركة
│   └── audio/        # audio sink headless
├── libs/             # مكتبات HLE (واجهات نظام Sony)
│   ├── libKernel.cpp, libNet.cpp, libAudio.cpp, libFont.cpp, ...
│   └── ajm/          # فك تشفير الصوت (AAC, ATRAC9, MP3)
├── tests/            # 57 ملف اختبار
├── scripts/          # سكريبتات Python (توليد SIMD، ربط الاختبارات)
├── docs/             # توثيق (SCOPE, HLE_COVERAGE, GPU_COMPUTE_PIPELINE, ...)
├── Makefile          # نظام البناء الرئيسي
├── CMakeLists.txt    # نظام البناء العابر للمنصات
├── project_config.json
└── CHANGES.md        # سجل التغييرات (119 KB - 30 جولة تطوير)
```

---

## 4. المكوّنات الرئيسية

### 4.1 وحدة المعالجة (CPU)
- **مفسر x86-64 محدود (fail-closed):** يدعم مجموعة صغيرة من التعليمات المسجّلية فقط: `MOV`, `ADD`, immediate moves, `NOP`, `SYSCALL`, `RET`. أي تعليمة غير مدعومة تُرفض ولا تُنفّذ.
- **مفسر كامل (x86_64_interpreter.cpp):** 1,648 سطر — توسعة تدعم مجموعة تعليمات أوسع.
- **JIT Executor:** محرك تنفيذ مباشر يستخدم التنفيذ الأصلي (direct execution) مع معالجة syscalls عبر SIGTRAP.
- **دعم SIMD/AVX:** ملفات `simd_full_map1.inc` (46K)، `simd_full_map23.inc` (29K)، `simd_full_map3.inc` (22K) — خرائط تعليمات SIMD مولّدة آلياً.
- **مدير FPU/x87:** حفظ واستعادة حالة FPU و AVX2 عبر FXSAVE64.
- **syscalls Prospero/FreeBSD:** 35+ دالة فعّالة (mmap, kqueue, cpuset, ...).
- **HLE Trampolines:** كل استيراد محلول يحصل على stub تنفيذي `mov eax,<magic+id>; syscall; ret` — يوجّه استدعاءات HLE إلى الدوال المضيفة بسرعة أصيلة.

### 4.2 وحدة الرسوميات (GPU)
- **PM4 Decoder:** فك حزم Type-3 مع التحقق من كامل التدفق قبل أي تأثير جانبي. مفصول عن التنفيذ.
- **PM4 Translator:** ترجمة حزم PM4 إلى أوامر Vulkan (1,335 سطر).
- **GCN Decoder:** فك تعليمات Graphics Core Next (1,992 سطر).
- **RDNA2 Compute Compiler:** مترجم مجموعة فرعية من تعليمات RDNA2 إلى SPIR-V 1.0 (3,108 سطر — أكبر ملف في src/). يرفض التعليمات غير المدعومة بدلاً من معاملتها كعملية أخرى.
- **Vulkan Backend:** اكتشاف جهاز وطابور Vulkan، تتبع حالة PM4، مسار compute executor حقيقي.
- **Software Rasterizer:** مسار ربّج برمجي بديل (582 سطر).
- **نموذج LDS:** 64 KiB برمجياً مع فحوصات حدود لنطاق لعمليات DS.
- **SMEM/MUBUF:** عمليات تحميل buffer مع فحوصات حدود صارمة.

### 4.3 وحدة الذاكرة (VMM)
- مدير ذاكرة افتراضية موحّد (16 GB) مع ترجمة GVA↔HVA.
- نسخ ذاكرة الضيف محميّة — ترفض النطاقات غير الم mapped ولا تعيد مؤشر مضيف خام لقيمة ضيف عشوائية.
- فحوصات صلاحيات كاملة للنسخ.

### 4.4 محمّل ELF / SELF
- **ELF Loader:** يتحقق من حدود program-header، يحمّل عبر صلاحية كتابة مؤقتة، يمسح BSS صراحةً، ثم يطبّق صلاحيات الضيف النهائية.
- **Prospero SELF Parser:** يحلل تنسيق SELF الأصلي لـ PS5 (magic `54 14 F5 EE`) — يقرأ بنية الحاوية، يطابق المقاطع مع ELF المضمّن، وينتج ELF قابلاً للإقلاع.
- **Runtime Linker:** ربط ديناميكي HLE — يحل رموز NID عبر DT_HASH، يدعم الأسماء المتنسخة (`NID#D#E`)، ينشئ trampolines للضيف.
- **GameFolderScanner:** تصنيف واكتشاف مجلدات الألعاب.

### 4.5 مكتبات HLE (30 وحدة)
إجمالي **1,633 رمز HLE** مصدّرة عبر `LIB_FUNC`:

| المستوى | الوحدة | الرموز | الدعم |
|---------|--------|:------:|-------|
| **A (كبير)** | libKernel | 373 | حقيقي (pthread/VMM/time) + stubs |
| | libNet | 271 | **POSIX sockets حقيقية** (TCP loopback, DNS) |
| | libAudio | 170 | SDL2 backend; stub بدون SDL2 |
| | libGraphicsDriver | 148 | stubs (PM4 في src/gpu) |
| | libAmpr | 106 | stubs |
| **B (متوسط)** | libFont | 70 | stb_truetype |
| | libJson2 | 61 | nlohmann-json (اختياري) |
| | libC | 60 | pass-through libc المضيف |
| | libDialog | 55 | stubs |
| | libSaveData | 51 | **استمرار حقيقي** (PARAM.bin + icon0.png) |
| | libSystemService | 34 | stubs + system_services.cpp |
| | libVideoOut | 31 | video_out_impl.cpp (مسار مرجعي) |
| | libPad | 31 | controller.cpp (SDL2) |
| | libRtc | 26 | وقت المضيف الحقيقي |
| | libUlt | 23 | stubs |
| **C (صغير)** | libPsml, libPlayGo, libUserService, libShare, libVideoDec2, libAppContent, libDbgAsan, libSysmodule, libPngDec, libRudp, libTextToSpeech2 | 2-19 | معظمها stubs |

> **حدّ الصدق:** رمز HLE مصدّر يعني أن المحاكي يمكنه *حلّ واستدعاء* دالة الضيف دون تحطم — لا يعني أنها تعيد سلوك PS4/PS5. معظمها stubs تسجيل ترجع نجاحاً (OK) أو رمز خطأ موثّق.

---

## 5. نظام البناء والاختبار

### أنظمة البناء
- **Makefile:** النظام الرئيسي (38 KB) — يكتشف تلقائياً SDL2, FFmpeg, Vulkan, JSON, fmt ويبني وفقاً لما هو متاح.
- **CMakeLists.txt:** بديل عابر للمنصات (Windows/MinGW/MSVC).
- **معايير الترجمة:** C++20، `-O2 -Wall -Wextra -Wpedantic`.
- **التبعيات:** لا توجد تبعات إلزامية؛ SDL2, FFmpeg, nlohmann-json, fmt, Vulkan اختيارية.

### الاختبارات
- **57 ملف اختبار** تغطي: CPU (مفسر، JIT، SIMD، AVX256، x87)، GPU (PM4، GCN، RDNA2، Vulkan، rasterizer)، kernel (events، semaphores، syscalls، fork)، loader (ELF، SELF، runtime linker)، HLE (audio، pad، entropy، graphics submit)، تكامل (guest boot، elf execution، et_dyn boot).
- **النتيجة الموثّقة:** 4,433 فحصة عبر 55 suite في Round 30 (EXIT=0).
- **إطار الاختبار:** مخصّص (custom)، بدون GoogleTest.

---

## 6. الجولات التطويرية (CHANGES.md - 119 KB)

سجل التغييرات يوثّق **30 جولة تطوير** متتالية، أبرزها:

| الجولة | الموضوع |
|--------|---------|
| **30** | الثنائي الحقيقي: تحليل SELF حقيقي لـ PS5 (Minecraft eboot.bin 254 MB)، ربط ديناميكي HLE، تشغيل CRT اللعبة على المفسر |
| **29** | جولة التدقيق: إصلاح كل الملاحظات، تشغيل ألعاب حقيقية، توسعة CPU/GPU/PM4/HLE |
| **28** | POSIX sockets حقيقية، استمرار save data |
| **26** | LDS برمجي 64 KiB، عمليات DS/SMEM/MUBUF، توسعة I/O |
| **19** | البنية الأساسية v19، مسار Vulkan حقيقي، إصلاح segfault |

---

## 7. الإنجاز الحقيقي (Round 30)

أبرز إنجاز موثّق هو **تشغيل Minecraft PS5 CRT startup** على المفسر:

```
prospero-run eboot.bin --interpreter
→ SELF مُحلّل (12 entry، 6 pairing، 50 DT_NEEDED)
→ ELF مُسطّح
→ 632,633 relocation مطبّقة (306 imports عبر trampolines)
→ 4 PT_LOAD م mapped (240 MB)
→ TLS → DT_INIT → entry point
→ libc حقيقي: MtxInitWithName, __cxa_atexit, PthreadGetthreadid, init_env, atexit
→ CRT startup لـ Minecraft ينفّذ على المفسر مع syscalls حية
```

**الجدران الحالية المعلنة بصدق:**
- 568 NID استيراد تنتمي لمكتبات بدون تغطية HLE بعد (libSceAgc — مشغل رسوميات PS5 — وغيرها).
- أول استدعاء PLT غير محلول يفشل عند عنوان link-time الصريح.
- لا يوجد rendering حقيقي للموارد الضيف (buffers, images, descriptors, pipelines) بعد.

---

## 8. تقييم الجودة

### نقاط القوة ✅
1. **منهجية fail-closed صارمة:** كل مكوّن يرفض ما لا يدعمه بدلاً من محاكاة سلوك غير مؤكد.
2. **صدق كامل في التوثيق:** الوثائق تصرّح بوضوح بما هو منفّذ وما هو غير منفّذ، وتنفي أي ادعاء توافق غير مثبت.
3. **اختبارات شاملة:** 4,433 فحصة عبر 55 suite، مع اختبارات تكامل حقيقية (boot، ELF execution، runtime linking).
4. **بنية نظيفة:** فصل واضح بين الفك (decoding) والتنفيذ (execution)، بنية معيارية، C++20.
5. **معالجة حقيقية لثنائي PS5 فعلي:** تحليل SELF حقيقي، ربط ديناميكي HLE، تشغيل CRT.
6. **لا تبعيات إلزامية:** يبني ويعمل بدون أي مكتبات خارجية (headless mode).
7. **سلامة الذاكرة:** نسخ محميّة، فحوصات صلاحيات، رفض المؤشرات الخام.
8. **30 جولة تطوير متدرّجة:** تطوير متزايد موثّق بعناية.

### نقاط الضعف / القيود ⚠️
1. **تغطية HLE جزئية:** 1,633 رمز من أصل سطح Sony الأكبر — معظمها stubs ترجع OK.
2. **GPU غير مكتمل:** لا يوجد rendering حقيقي للموارد الضيف؛ المسار الرسومي أدنى (compute فقط).
3. **مفسر CPU محدود:** المفسر الأساسي يدعم مجموعة تعليمات صغيرة جداً؛ الأداء بطيء (interpreter).
4. **لا توافق ألعاب:** المشروع بحثي — لا يوجد لعبة تجارية تعمل بالكامل.
5. **568 NID غير محلول:** استيرادات libSceAgc وغيرها بدون تغطية.
6. **الأداء:** التنفيذ بالمفسر بطيء بطبيعته؛ JIT محدود.
7. **منصة واحدة:** Linux x86-64 فقط بشكل فعلي (CMake يدعم Windows نظرياً).

### ملاحظات أمنية 🔒
- لا يستخدم RWX للضيف ولا يعتمد على signal handlers على مستوى العملية.
- يرفض صور ciphertext retail (fail-closed).
- فحوصات حدود صارمة لـ MUBUF/SMEM/LDS.
- مسار الإقلاع يتحقق من bounds و permissions.

---

## 9. التقنيات المستخدمة

| المجال | التقنية |
|--------|---------|
| **اللغة** | C++20 |
| **GUI/نوافذ** | SDL2 (اختياري) |
| **الرسوميات** | Vulkan 1.3 |
| **فك الفيديو** | FFmpeg (اختياري) |
| **JSON** | nlohmann-json (اختياري) |
| **التنسيق** | fmt (اختياري) |
| **الخطوط** | stb_truetype |
| **الصور** | stb_image |
| **البناء** | Make / CMake |
| **التحليل الثابت** | cppcheck, clang-tidy |
| **التنسيق** | clang-format |
| **التوثيق** | Doxygen |

---

## 10. الخلاصة

**ProsperoLayer RDNA2 Core** هو مشروع بحثي طموح ومنهجي لمحاكاة PS5، يتميّز بـ:

- **نواة تدقيق صارمة** (fail-closed) بدلاً من محاكاة هشة.
- **توثيق صادق** يفصل بوضوح بين المُنفّذ والمُدّعى.
- **اختبارات حقيقية** تمرّ عبر pipeline الإقلاع الفعلي.
- **30 جولة تطوير** تظهر تقدّماً متدرّجاً موثّقاً.

المشروع في مرحلة **بحث متقدّم** — نجح في تحليل ثنائي PS5 حقيقي وتشغيل CRT اللعبة على المفسر، لكنه لم يصل بعد إلى تشغيل لعبة كاملة. الفجوة الأكبر هي في تغطية HLE لمكتبات الرسوميات (libSceAgc) والـ rendering pipeline الكامل.

التقييم العام: **مشروع بحثي عالي الجودة بمنهجية سليمة وصدق توثيقي نادر في هذا المجال.**
