# سجل التغييرات — Round 32 (تنفيذ القيود المتبقية)

**التاريخ:** 3 سبتمبر 2026  
**الإصدار:** v19.1.0 → v19.2.0 (Round 32)  
**النتيجة:** 59 اختبار ناجح / 0 فشل / ~4,400+ فحصة

---

## ⚠️ تصحيح مهم (Post-Round-32 Fix)

> **النسخة الأولية من هذه الجولة احتوت على ادعاء تقني خاطئ.**  
> الـ 33 NID المسجّلة في libAgc.cpp كانت أسماء نصية مخترعة (`AgcInit00001`, `AgcCrtSh001`)  
> وليست هاشات Sony الحقيقية. آلية الربط الديناميكي في المشروع تطابق بالـ NID hash  
> الحرفي بعد قطع لاحقة `#`، لذلك مستحيل رياضياً أن يتطابق NID حقيقي مع اسم نصي مخترع.  
> النتيجة: الـ 33 دالة كانت **كود ميت** — صفر تقدّم فعلي نحو تشغيل أي لعبة حقيقية.  
> الادعاء "يقلل NID من 568 إلى 535" كان **مضلّلاً تقنياً**.  
>
> **تم الإصلاح:** libAgc.cpp أُعيدت كتابتها بـ **95 NID حقيقية** من قاعدة بيانات ps5rs  
> المفتوحة المصدر (https://github.com/claimore22/ps5rs). التفاصيل في القسم 1 أدناه.

---

## ملخص التنفيذ

تم تنفيذ القيود المتبقية عبر ثلاث جبهات رئيسية: إصلاح وحدة libSceAgc HLE (المانع الأكبر)، تحسين GPU rendering (blending + topology)، وإضافة JIT block chaining.

---

## 1. إصلاح وحدة libSceAgc HLE (المانع الأكبر 🔴)

### المشكلة الأصلية
568 NID غير محلول من مكتبة libSceAgc (مشغل رسوميات PS5 Advanced Graphics Core) — كانت تمنع الثنائي الحقيقي (مثل Minecraft PS5 eboot.bin) من تجاوز مرحلة CRT startup.

### الخطأ في النسخة الأولية
النسخة الأولية من libAgc.cpp سجّلت 33 NID بأسماء نصية مخترعة (`AgcInit00001` إلخ). هذه أسماء وصفية، ليست هاشات Sony. لأن `SymbolDatabase::FindSymbol(name)` يطابق بالاسم الحرفي في `m_symbols` map، وآلية الربط الديناميكي تقارن بالـ NID hash الحرفي بعد قطع لاحقة `#`، فإن هذه الأسماء المخترعة لن تتطابق أبداً مع أي ثنائي PS5 حقيقي. كانت **كود ميت**.

### الإصلاح (المطبّق الآن)
إعادة كتابة `libs/libAgc.cpp` بـ **95 NID حقيقية** مستخرجة من قاعدة بيانات ps5rs المفتوحة المصدر 【web-github-9119510d】:

- **المصدر:** `data/stubs/by_library/libSceAgc.txt` في مستودع ps5rs
- **العدد الكامل المستخرج:** 226 NID
- **التداخل مع libGraphicsDriver.cpp:** 119 NID متداخلة (مسجّلة بالفعل كـ "Graphics5") — تم استبعادها
- **الـ NID الفريدة الجديدة:** 107 NID (226 − 119)، منها 95 مسجّلة في libAgc.cpp و12 غير مسجّلة (أسماء دوال تحتوي أحرفاً خاصة لم تُحلّل بالكامل)
- **أمثلة:** `03RZmELWWzw` → `sceAgcCbSetUcRegistersDirect`, `0o3VDdtA6nM` → `sceAgcDcbSetIndexIndirectArgs`

**التفاصيل التقنية:**
- `LIB_VERSION("Agc", 1, "Agc", 1, 1)` — مسجّلة كمكتبة Agc
- كل دالة تستخدم `PRINT_NAME()` وتسجل معاملاتها وترجع `OK`
- مسجّلة في `libs/libs.cpp` عبر `LIB_DEFINE(InitAgc_1)` و `LIB_LOAD(InitAgc_1)`
- البناء: `g++ -std=c++20 -O0 -Wall -Wextra` — نظيف بلا تحذيرات
- الاختبارات: 59 اختبار / 0 فشل

**حدّ الصدق (مهم):**
هذه الدوال **stubs ترجع OK فقط** — لا تُ reproduce سلوك libSceAgc الحقيقي. حلّ NID يعني أن import slot يحصل على trampoline بدلاً من fault — **لا يعني** أن العملية الرسومية تعمل فعلاً. الـ 95 NID الآن **قابلة للحل** (resolvable) من قبل الـ runtime linker، لكن تنفيذها لا يزال شكلياً.

**ملاحظة مفاهيمية مهمة حول الأرقام:**
رقم 568 هو إجمالي NID غير المحلولة من تحليل الثنائي الأصلي (Minecraft eboot) — وهي NID لا تملك أي تغطية HLE. الـ 119 NID المتداخلة مع libGraphicsDriver **محلولة بالفعل** (مسجّلة كـ "Graphics5")، لذا لا تُحسب ضمن الـ 568 أصلاً. بناءً على ذلك:

**ما يعنيه هذا فعلياً:**
- ✅ ثنائي PS5 حقيقي سيجد هذه الـ 95 NID في جدول الرموز ويربطها بـ trampolines بدلاً من الفشل
- ❌ لن تُنتج هذه الدوال أي رسوميات حقيقية — سترجع OK وتسجل اسمها فقط
- ❌ الـ ~473 NID المتبقية (568 − 95) لا تزال غير محلولة — هذا الإصلاح يقلّل import faults لكنه لا يكسر جدار Minecraft فعلياً

**الملفات:** `libs/libAgc.cpp` (مُعاد كتابته بالكامل)، `libs/libs.cpp` (معدّل سابقاً)  
**الأثر الفعلي:** 95 NID حقيقية إضافية قابلة للحل — وليست 33 NID مخترعة كود ميت

---

## 2. تحسين GPU Rendering — Blending + Topology

### 2A. دعم Alpha Blending

**المشكلة:** الـ software rasterizer لم يكن يدعم alpha blending — الكتابة فوق البكسل مباشرة بدون مزج.

**الإصلاح:**
- **`include/gpu/software_rasterizer.hpp`:**
  - إضافة `BlendFactor` enum: `BlendZero=0`, `BlendOne=1`, `BlendSrcAlpha=2`, `BlendOneMinusSrcAlpha=3`
  - إضافة `BlendOp` enum: `BlendOpAdd=0`, `BlendOpSubtract=1`, `BlendOpReverseSubtract=2`
  - إضافة `BlendState` struct: `enabled`, `src_factor`, `dst_factor`, `blend_op`
  - إضافة حقل `BlendState blend` إلى `RasterTarget`
- **`src/gpu/software_rasterizer.cpp`:**
  - دالة `BlendFactorValue()`: تحويل عامل المزج إلى قيمة float
  - دالة `BlendAndWrite()`: فك بكسل الوجهة، تطبيق `result = src*sf <op> dst*df`، إعادة ترميز
  - توجيه جميع مسارات كتابة البكسل عبر `BlendAndWrite` بدلاً من `EncodePixel`
  - **Fail-closed:** عامل/عملية/صيغة غير معروفة → كتابة مباشرة (بدون مزج)

### 2B. دعم Triangle Strip/Fan Topology

**المشكلة:** الـ rasterizer كان يدعم triangle-list فقط.

**الإصلاح:**
- إضافة `Topology` enum: `TopologyTriangles=0`, `TopologyTriangleStrip=1`, `TopologyTriangleFan=2`
- إضافة حقل `Topology topology` إلى `RasterTarget` (افتراضي: Triangles)
- في حلقة الرسمنة:
  - **Strip:** مثلثات `(i, i+1, i+2)` مع قلب winding للمثلثات الفردية
  - **Fan:** مثلثات `(0, i, i+1)` لـ `i=1..N-2`
  - **Triangles:** ثلاثيات متتالية (السلوك السابق)
- تحويل الحلقة من `tri += 3` إلى pre-decomposition يبني قائمة فهارس قبل الرسمنة

### 2C. اختبار جديد

`tests/software_rasterizer_blend_test.cpp` — **28 فحصة**:
- A: SRC_ALPHA / ONE_MINUS_SRC_ALPHA blend
- B: triangle strip (4 vertices → 2 triangles)
- C: triangle fan (5 vertices → 3 triangles)
- D: fail-closed blend disabled (overwrite)
- E: default topology regression

**الملفات:** `include/gpu/software_rasterizer.hpp`, `src/gpu/software_rasterizer.cpp`, `tests/software_rasterizer_blend_test.cpp`, `Makefile`  
**الأثر:** الـ rasterizer يدعم الآن alpha blending و ثلاثة أنواع topology

---

## 3. JIT Block Chaining

### المشكلة
محرك JIT كان يسجّل metadata فقط بدون codegen حقيقي، وكل block يُنفّذ بشكل مستقل — لا يوجد chaining بين blocks المتتالية، مما يسبب overhead من cache lookup عند كل انتقال.

### الإصلاح
إضافة block chaining — ربط exit block بـ entry block التالي مباشرة:

- **`include/cpu/jit_executor.hpp`:**
  - إضافة حقول إلى `JITBasicBlock`: `next_block_rip` (0=غير مرتبط)، `prev_block_rip` (ربط عكسي)، `chain_count` (عداد الاستخدام)
  - إضافة دوال: `ChainBlocks(from_rip, to_rip)`, `GetChainedBlock(rip)`, `InvalidateBlock(rip)`
  - إضافة دوال إحصاء: `GetTotalBlocksCompiled()`, `GetTotalChainsCreated()`, `GetTotalChainHits()`
  - 3 عدادات `std::atomic<uint64_t>`

- **`src/cpu/jit_executor.cpp`:**
  - `ChainBlocks`: يربط block بـ successor، يضبط `next_block_rip` و `prev_block_rip`، يزيد `chain_count`
  - `GetChainedBlock`: يرجع chained successor مباشرة (يتخطى cache lookup)، يزيد `chain_hits`
  - `InvalidateBlock`: ينظف chains الأمامية والخلفية، يحذف block من cache
  - **Fail-closed:** أي block مفقود/قديم → nullptr/false → fallback إلى cache lookup العادي

### اختبار جديد

`tests/jit_chaining_test.cpp` — **5 اختبارات**:
1. BlockCompilationAndCaching — التجميع والتخزين
2. BlockChainingBetweenTwoBlocks — ربط blockين متتاليين
3. ChainHitSkipsCacheLookup — chain hit يتخطى cache lookup
4. StatisticsAreTrackedCorrectly — تتبع الإحصاءات
5. InvalidatingABlockClearsItsChains — invalidation ينظف chains

**الملفات:** `include/cpu/jit_executor.hpp`, `src/cpu/jit_executor.cpp`, `tests/jit_chaining_test.cpp`, `Makefile`  
**الأثر:** JIT يدعم الآن block chaining (lazy chaining) مع إحصاءات

---

## 4. النتائج النهائية

| المعيار | Round 31 | Round 32 |
|---------|----------|----------|
| **الاختبارات** | 57 PASS / 0 FAIL | **59 PASS / 0 FAIL** |
| **الفحوصات** | ~4,333 | **~4,400+** |
| **HLE modules** | 30 | **31** (Agc) |
| **HLE symbols** | ~1,633 | **~1,728** (+95 Agc حقيقية) |
| **Blending** | غير مدعوم | مدعوم (4 factors, 3 ops) |
| **Topology** | Triangles only | Triangles + Strip + Fan |
| **JIT chaining** | غير مدعوم | مدعوم (lazy chaining) |
| **اختبارات جديدة** | 2 | **+2** (blend, chaining) |

---

## 5. القيود المتبقية (للجولات القادمة)

1. **libSceAgc تنفيذ وظيفي:** الـ 95 NID الحقيقية قابلة للحل الآن (resolvable) لكن الـ stubs ترجع OK فقط — لا تُنفّذ عمليات رسومية حقيقية. ~473 NID إضافية (568 − 95) لا تزال غير محلولة. هذا لا يكسر "جدار Minecraft" فعلياً — يقلّل فقط عدد الـ import faults.
2. **GPU rendering كامل:** لا swapchain (`VkSwapchainKHR`)، لا fragment shaders حقيقية للضيف، لا إدارة ذاكرة GPU مستمرة
3. **JIT codegen حقيقي:** block chaining يحسن الـ metadata cache لكن لا يزال بدون codegen أصيل (ترجمة guest → host native)
4. **تغطية RDNA2:** مجموعة فرعية فقط من التعليمات مدعومة في المترجم
5. **تعدد الخيوط الكامل:** lock prefix يُتتبّع الآن لكن التنفيذ متعدد الخيوط للضيف غير مكتمل

---

## 6. الملفات المعدّلة/المضافة في Round 32

| الملف | نوع التغيير |
|------|-------------|
| `libs/libAgc.cpp` | **مُعاد كتابته** — 95 NID حقيقية من ps5rs (بعد إصلاح النسخة المخترعة) |
| `libs/libs.cpp` | معدّل — تسجيل InitAgc_1 |
| `include/gpu/software_rasterizer.hpp` | معدّل — BlendState, BlendFactor, BlendOp, Topology |
| `src/gpu/software_rasterizer.cpp` | معدّل — blending + strip/fan decomposition |
| `tests/software_rasterizer_blend_test.cpp` | **جديد** — اختبار blending + topology (28 فحصة) |
| `include/cpu/jit_executor.hpp` | معدّل — block chaining fields + methods |
| `src/cpu/jit_executor.cpp` | معدّل — ChainBlocks, GetChainedBlock, InvalidateBlock |
| `tests/jit_chaining_test.cpp` | **جديد** — اختبار JIT chaining (5 اختبارات) |
| `Makefile` | معدّل — إضافة اختبارين جديدين |

---

## 7. الإجمالي عبر جولتين (Round 31 + 32)

| الإصلاح | Round |
|---------|-------|
| إصلاح خطأ بناء x87 cmath | 31 |
| تتبّع lock prefix في CPU | 31 |
| Texture sampling (nearest-neighbor) | 31 |
| وحدة libSceAgc HLE (95 NID حقيقية) | 32 |
| Alpha blending في rasterizer | 32 |
| Triangle strip/fan topology | 32 |
| JIT block chaining | 32 |
| **إجمالي اختبارات جديدة** | **4** (lock, texture, blend, chaining) |
| **إجمالي فحصات جديدة** | **~64** |
