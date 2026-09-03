# سجل التغييرات — Round 31 (تطوير وإصلاح القيود)

**التاريخ:** 3 سبتمبر 2026  
**الإصدار:** v19.0.0 → v19.1.0 (Round 31)  
**النتيجة:** 57 اختبار ناجح / 0 فشل / ~4,333 فحصة

---

## ملخص التنفيذ

تم تطوير وإصلاح قيود مشروع ProsperoLayer RDNA2 Core عبر ثلاث جبهات رئيسية: CPU و HLE و GPU، مع إصلاح أخطاء بناء وإضافة اختبارات جديدة.

---

## 1. إصلاح أخطاء البناء (Critical Fix)

### المشكلة
ملف `src/cpu/x86_64_x87.cpp` استخدم دوال `long double` الرياضية مع بادئة `std::` (مثل `std::floorl`، `std::ceill`، `std::fabsl`)، لكن g++ 12 لا يوفّرها في namespace `std::`.

### الإصلاح
استبدال جميع استدعاءات `std::floorl/ceill/fabsl/tanl/sinl/cosl/sqrtl/fmodl/atan2l/frexpl/nearbyintl/truncl` بنسخها العامة (C globals) بدون بادئة `std::`.

**الملف:** `src/cpu/x86_64_x87.cpp`  
**الأثر:** البناء يعمل بنجاح على g++ 12 (Debian 12)

---

## 2. إصلاح ذرية `lock` prefix في CPU

### المشكلة
كان `lock` prefix (0xF0) يُتجاهل صمتاً: `if (byte == 0xF0) { continue; }` — مما يعني أن `lock cmpxchg` و `lock xadd` و `lock add` كانت تعمل بشكل مماثل لنظيراتها بدون lock. صحيح للمفسّس أحادي الخيط، لكن البادئة كانت تُفقد لأي مسار تنفيذ مباشر أو متعدد الخيوط مستقبلاً.

### الإصلاح
- **`include/cpu/x86_64_interpreter.hpp`:** إضافة حقل `bool lock{false}` إلى بنية `Prefixes`
- **`src/cpu/x86_64_interpreter.cpp`:** تتبّع البادئة: `if (byte == 0xF0) { p.lock = true; continue; }`
- **`src/cpu/x86_64_isa_ext.cpp`:** توثيق أن `cmpxchg` و `xadd` ذرية بطبيعتها في المفسّس أحادي الخطوة (read-compare-write غير قابل للمقاطعة)

**الملفات:** `include/cpu/x86_64_interpreter.hpp`، `src/cpu/x86_64_interpreter.cpp`، `src/cpu/x86_64_isa_ext.cpp`  
**الأثر:** `lock` prefix يُتتبّع الآن بدلاً من تجاهله، مما يهيّأ المشروع لدعم多-threaded execution

---

## 3. تحسين Software Rasterizer — Texture Sampling

### المشكلة
الـ software rasterizer كان يدعم clipping و viewport transform و depth test و color interpolation و per-format pixel encoding، لكن **لم يدعم texture sampling** — مما يعني أن الرسوميات المنظّمة كانت مقتصرة على ألوان الرؤوس فقط.

### الإصلاح
- **`include/gpu/software_rasterizer.hpp`:** إضافة بنية `RasterTexture` (width, height, format, data pointer) وحقل `texture` إلى `RasterTarget`
- **`src/gpu/software_rasterizer.cpp`:**
  - إضافة حقول `u`, `v` إلى `ClipVertex` و `ScreenVertex`
  - توسعة clipper لاستيفاء UV
  - إضافة دالة `SampleTextureNearest()`: nearest-neighbor sampling مع CLAMP_TO_EDGE (NaN-safe)
  - **Fail-closed:** texture null أو حجم صفر → لون أسود افتراضي `(0,0,0,1)`
  - Texel يضرب (modulate) في لون الرأس عند وجودهما معاً
- **`tests/software_rasterizer_texture_test.cpp`:** اختبار جديد (25 فحصة) يتحقق من:
  - A: 2×2 nearest-neighbour sampling
  - B: vertex colour modulation
  - C: fail-closed null texture
  - D: fail-closed zero-size texture
- **`Makefile`:** إضافة `SOFTWARE_RASTER_TEX_TEST` إلى `UNIT_TESTS`

**الملفات:** `include/gpu/software_rasterizer.hpp`، `src/gpu/software_rasterizer.cpp`، `tests/software_rasterizer_texture_test.cpp`، `Makefile`  
**الأثر:** الـ rasterizer يدعم الآن texture sampling أساسي (nearest-neighbor)

---

## 4. اختبار Lock Prefix الجديد

### الإضافة
- **`tests/lock_prefix_test.cpp`:** اختبار جديد (6 فحصات) يتحقق من:
  - A: `lock cmpxchg` — atomic compare-exchange (RAX == [mem] → [mem] = RBX)
  - B: `lock xadd` — atomic exchange-add ([mem] += RAX, RAX = old [mem])
  - C: `lock add` — atomic add ([mem] += RAX)
  - D: `cmpxchg` بدون lock — لا انحدار (RAX ≠ [mem] → RAX = [mem], [mem] unchanged)
- **`Makefile`:** إضافة `LOCK_PREFIX_TEST` إلى `UNIT_TESTS`

---

## 5. نتائج الاختبار النهائية

| المعيار | قبل | بعد |
|---------|-----|-----|
| **البناء** | ❌ فشل (x87 cmath) | ✅ نجح |
| **الاختبارات** | غير قابل للتشغيل | **57 PASS / 0 FAIL** |
| **الفحوصات** | — | **~4,333 فحصة** |
| **lock prefix** | مُتجاهل | مُتتبّع + مُختبَر |
| **Texture sampling** | غير مدعوم | مدعوم (nearest-neighbor) |
| **اختبارات جديدة** | 0 | 2 (lock_prefix + texture) |

---

## القيود المتبقية (غير المغلقة في هذه الجولة)

1. **libSceAgc (568 NID):** المانع الأكبر لا يزال — مكتبة مشغل رسوميات PS5 غير مسجّلة. اكتشفنا أن Ngs2/Ajm/AvPlayer لها تطبيقات موجودة في `libAudio.cpp`/`ajm.cpp`/`avPlayer.cpp` (تُستثنى بدون FFmpeg).
2. **GPU rendering كامل:** لا swapchain، لا fragment shaders حقيقية، لا إدارة ذاكرة مستمرة.
3. **JIT codegen:** محرك JIT يسجّل metadata فقط بدون codegen فعلي.
4. **تغطية RDNA2:** مجموعة فرعية فقط من التعليمات مدعومة في المترجم.

---

## الملفات المعدّلة/المضافة

| الملف | نوع التغيير |
|------|-------------|
| `src/cpu/x86_64_x87.cpp` | إصلاح دوال `long double` |
| `include/cpu/x86_64_interpreter.hpp` | إضافة `lock` إلى `Prefixes` |
| `src/cpu/x86_64_interpreter.cpp` | تتبّع `lock` prefix |
| `src/cpu/x86_64_isa_ext.cpp` | توثيق ذرية cmpxchg/xadd |
| `include/gpu/software_rasterizer.hpp` | بنية `RasterTexture` |
| `src/gpu/software_rasterizer.cpp` | texture sampling nearest-neighbor |
| `tests/software_rasterizer_texture_test.cpp` | اختبار جديد (25 فحصة) |
| `tests/lock_prefix_test.cpp` | اختبار جديد (6 فحصات) |
| `Makefile` | إضافة اختبارين جديدين |
| `libs/libs.cpp` | إزالة تسجيلات مكررة |
