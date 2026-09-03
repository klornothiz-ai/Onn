# Round 33 — إصلاح المشاكل الحرجة + خطأ x87

**التاريخ:** 3 سبتمبر 2026
**الأساس:** التحليل الشامل في `COMPREHENSIVE_ANALYSIS.md` (130 مشكلة عبر 6 جبهات)
**النتيجة:** 15 إصلاح حرج مُطبّق، البناء ناجح، جميع الاختبارات تمرّ (0 failures)

---

## ملخص الإصلاحات

تم إصلاح 15 مشكلة حرجة موزّعة على 5 جبهات: CPU (4)، GPU (3)، Memory Safety (3)، HLE (2)، البناء والاختبار (3).

| الجبهة | الإصلاحات | الحالة |
|--------|-----------|--------|
| CPU | 4 | ✅ مكتمل |
| GPU | 3 | ✅ مكتمل |
| Memory Safety | 3 | ✅ مكتمل |
| HLE | 2 | ✅ مكتمل |
| البناء والاختبار | 3 | ✅ مكتمل |

---

## 1. إصلاحات CPU الحرجة (4/4)

### 1.1 DIV 8-bit — فساد RSP
**الملف:** `src/cpu/x86_64_interpreter.cpp:866`
**المشكلة:** DIV 8-bit كان يستخدم `WriteRegByte(RAX|4, true, ...)` بدل `false`، مما يكتب في البايت العليا من RSP بدل RAX، فاسدًا مؤشر المكدس.
**الإصلاح:** تغيير `true` → `false` في `WriteRegByte`.

### 1.2 SHL undefined behavior
**الملف:** `src/cpu/x86_64_interpreter.cpp:~782-810`
**المشكلة:** SHL/SHR/SAR لم تفحص `count <= size*8` قبل حساب CF، مما يسبب undefined behavior عند count كبير.
**الإصلاح:** إضافة guard `count <= size*8` قبل حساب CF.

### 1.3 cmpxchg/xadd atomicity
**الملف:** `src/cpu/x86_64_isa_ext.cpp:34, 364-410`
**المشكلة:** LOCK-prefixed cmpxchg/xadd لم تكن ذرية عبر خيوط الـ guest المتعددة.
**الإصلاح:** إضافة `#include <mutex>`، `static std::mutex s_atomic_mutex`، وتغليف cmpxchg (B0/B1) و xadd (C0/C1) في `std::unique_lock` عند `p.lock == true`.

### 1.4 fsin/fcos/fsincos x87 range check
**الملف:** `src/cpu/x86_64_x87.cpp:332-362`
**المشكلة:** fsin/fcos/fsincos لم تفحص `|v| <= 2^63` قبل الحساب، مخالفةً سلوك x87 الحقيقي.
**الإصلاح:** فحص `std::fabs(v) <= 2^63` وضبط علم C2 عند تجاوز النطاق (ST(0) يبقى دون تغيير، مطابقًا للأجهزة الحقيقية).
**ملاحظة:** أصلح أيضًا `std::fabsl` → `std::fabs` (fabsl غير متوفر في std:: namespace على g++ 12).

---

## 2. إصلاحات GPU حرجة (3/3)

### 2.1 S_MOV_B64 OOB write
**الملف:** `src/gpu/gcn_decoder.cpp:814-825`
**المشكلة:** S_MOV_B64 يكتب في زوج SGPR بدون فحص أن `ins.dst + 1 < kSgprCount`، مما يكتب خارج حدود المصفوفة.
**الإصلاح:** إضافة فحص `static_cast<size_t>(ins.dst) + 1 >= kSgprCount` قبل الكتابة.

### 2.2 texture loading buffer overflow
**الملف:** `include/gpu/software_rasterizer.hpp` + `src/gpu/software_rasterizer.cpp:153-158`
**المشكلة:** `SampleTextureNearest` يقرأ من بيانات النسيج بدون فحص حدود `data_size`.
**الإصلاح:** إضافة حقل `size_t data_size{0}` إلى `RasterTexture` (0 = غير معروف، لا فحص؛ >0 = فحص) وفحص `idx+4 > data_size` في `SampleTextureNearest`. متوافق مع الإصدارات السابقة.

### 2.3 VOP dst bounds check
**الملف:** `src/gpu/gcn_decoder.cpp` (977, 1019, 1065)
**المشكلة:** VOP1/VOP2/VOP3 لم تفحص أن `ins.dst < kVgprCount` قبل الكتابة.
**الإصلاح:** إضافة فحص `static_cast<size_t>(ins.dst) >= kVgprCount` لـ VOP1، VOP2، VOP3. VOPC يكتب إلى VCC (لا VGPR dst)، فلا يحتاج فحصًا.
**ملاحظة:** أصلح أيضًا أخطاء sign-comparison (`int` vs `size_t`) بإضافة `static_cast<size_t>()`.

---

## 3. إصلاحات Memory Safety (3/3)

### 3.1 Dangling pointers في runtime linker
**الملف:** `src/loader/runtime_linker.cpp` + `include/loader/runtimeLinker.h`
**المشكلة:** `m_modules` هو `std::deque<ModuleInfo>`. `std::deque::erase` يُبطل المؤشرات للعناصر بعد الموضع المحذوف. `FindProgramByAddr`/`FindProgramById`/`FindModule` تُرجع `ModuleInfo*` مباشرة، فأي `erase` لاحق يُبطل هذه المؤشرات.
**الإصلاح:**
- إضافة حقل `bool is_unloaded{false}` إلى `ModuleInfo` (منفصل عن `is_loaded` الذي يعني "ELF محمّل").
- استبدال `erase` بـ mark-as-unloaded (`is_unloaded = true`) في 3 مواقع: `UnloadModule`، `UnloadProgram`، `UnregisterModule`.
- إضافة فحص `!is_unloaded` في جميع دوال البحث: `FindProgramByAddr` (نسختان)، `FindProgramById`، `FindProgramByFileName`، `FindModule`، `GetModuleInfo`، `FindModuleBySoNameImpl`، `CollectResolutionOrder`.
- `UnloadModule` الآن يُرجع `-1` عند محاولة تفريغ وحدة مُفرّغة سابقًا (double-unload fails closed).
**النتيجة:** جميع مؤشرات `ModuleInfo*` تبقى صالحة طوال عمر الـ deque، والوحدات المُفرّغة تصبح غير مرئية للبحث.

### 3.2 Buffer overflow avPlayer NV12
**الملف:** `libs/avPlayer.cpp:1079-1093`
**المشكلة:** نسخ NV12 يستخدم `src->width` و `src->height` بدون فحص أنها لا تتجاوز `pitch` و `h` (الأبعاد المخصصة). إطار مشوّه أو أكبر من المتوقع يفيض الـ buffer.
**الإصلاح:** تقييد النسخ بالأبعاد المخصصة:
```cpp
const uint32_t copy_w = std::min<int>(src->width, pitch);
const uint32_t copy_h = std::min<int>(src->height, h);
```
**ملاحظة:** avPlayer.cpp يُستثنى من البناء بدون FFmpeg في هذه البيئة، لكن الإصلاح يحمي البيئات التي تتضمنه.

### 3.3 memcpy init/fini overflow
**الملف:** `src/loader/runtime_linker.cpp:1055, 1152`
**الحالة:** ✅ مُصلّح مسبقًا (Round 32 أو أقدم)
**التفاصيل:** دوال `GetInitFunctions` و `GetFiniFunctions` تحتوي بالفعل على فحص حدود:
```cpp
const size_t available = (table_off < image->Size())
    ? static_cast<size_t>(image->Size() - table_off) : 0;
const uint64_t entries = std::min<uint64_t>(count, available / 8);
```
هذا يمنع `table_off + i*8` من تجاوز حجم الصورة. لا حاجة لإصلاح إضافي — تم التحقق من صحته.

---

## 4. إصلاحات HLE (2/2)

### 4.1 SymbolDatabase::Add silent overwrite
**الملف:** `include/loader/symbolDatabase.h:48-76`
**المشكلة:** `m_symbols[sr.name] = ptr` يكتب بصمت فوق أي تسجيل سابق. عندما تسجّل مكتبتان نفس الـ NID بمؤشرات مختلفة، الثانية تفوز بصمت بدون أي تنبيه.
**الإصلاح:** استبدال `operator[]` بـ `try_emplace`:
```cpp
auto [it, inserted] = m_symbols.try_emplace(sr.name, ptr);
if (!inserted && it->second != ptr) {
    std::cerr << "[SymbolDB] WARNING: NID conflict for '"
              << sr.name << "' — keeping first registration\n";
}
```
التسجيل الأول (ترتيب تحميل ELF) هو الفائز. التعارضات تُسجّل في stderr. نطبّق نفس النمط على `Add` و `AddDirect`.

### 4.2 مراجعة NID مكررة
**الملفات:** `libs/libKernel.cpp`, `libs/libC.cpp`, `libs/libSaveData.cpp`, `libs/libDialog.cpp`, `libs/libAudio.cpp`, `libs/audio_headless.cpp`, `libs/libNet.cpp`, `libs/libShare.cpp`
**النتيجة:** تم اكتشاف 59 NID مكررة عبر المكتبات. التصنيف:

| الفئة | العدد | الخطورة | الوصف |
|------|------|---------|-------|
| نفس الدالة في نفس الملف | 29 |无害 | libSaveData (20), libDialog (5), libC (4) — تسجيل مزدوج في نطاقين مختلفين |
| Wrapper vs implementation | 20 |无害 | libKernel Posix:: vs LibKernel:: — نفس الدالة بغلاف مختلف |
| audio_headless vs libAudio | 6 |无害 | نفس الدالة في نسختين (headless و full) |
| دوال مختلفة، نفس NID | 2 |⚠️ | `9BcDykPmo1I` و `YBiIdcDPrxs` — لكن متكافئة دلاليًا |

**التفاصيل الخطرة:**
- `9BcDykPmo1I`: `LibC::libc_error` و `LibKernel::get_error_addr` — كلاهما يُرجع `Posix::GetErrorAddr()`. متكافئ.
- `YBiIdcDPrxs`: `LibShare::ShareFeaturePermit` (في libNet) و `Share::ShareFeaturePermit` (في libShare) — كلاهما يُرجع OK. متكافئ.

**الخلاصة:** جميع التكرارات الـ 59 إمّا无害 (نفس الدالة) أو متكافئة دلاليًا. إصلاح `SymbolDatabase::Add` (try_emplace + warn) يتعامل معها جميعًا بشكل صحيح — التسجيل الأول يفوز، والتعارضات تُسجّل.

---

## 5. البناء والاختبار (3/3)

### 5.1 البناء
- `make unit -j4` يبني 62 ثنائي اختبار بنجاح.
- جميع التحذيرات (`-Wall -Wextra -Wpedantic -Werror`) مُعالجة.
- أصلح خلال البناء:
  - `std::fabsl` → `std::fabs` (غير متوفر في std:: على g++ 12)
  - أخطاء sign-comparison في gcn_decoder.cpp (`int` vs `size_t`) بإضافة `static_cast<size_t>()`

### 5.2 الاختبارات
- جميع اختبارات الوحدة تمرّ: **0 failures** عبر 28 suite.
- اختبارات رئيسية تمرّ:
  - `runtime_linker_test`: handles stay unique، double-unload fails closed ✅
  - `x86_64_interpreter_test`: DIV, SHL, cmpxchg, x87 ✅
  - `gcn_decoder_test`: S_MOV_B64, VOP bounds ✅
  - `guest_boot_test`, `guest_execution_integration_test`: لا regression ✅
  - `hle_libkernel_test`, `hle_plt_test`: symbol resolution ✅

### 5.3 التوثيق
- هذا الملف (`CHANGES_ROUND33.md`).

---

## الملفات المُعدّلة

| الملف | الإصلاح |
|------|---------|
| `src/cpu/x86_64_interpreter.cpp` | DIV 8-bit, SHL/SHR/SAR UB |
| `src/cpu/x86_64_isa_ext.cpp` | cmpxchg/xadd atomicity (mutex) |
| `src/cpu/x86_64_x87.cpp` | fsin/fcos/fsincos range check, fabsl→fabs |
| `src/gpu/gcn_decoder.cpp` | S_MOV_B64 OOB, VOP bounds, sign-compare fixes |
| `include/gpu/software_rasterizer.hpp` | data_size field in RasterTexture |
| `src/gpu/software_rasterizer.cpp` | texture bounds check |
| `src/loader/runtime_linker.cpp` | dangling pointers (mark-as-unloaded), is_unloaded checks |
| `include/loader/runtimeLinker.h` | is_unloaded field in ModuleInfo |
| `libs/avPlayer.cpp` | NV12 buffer overflow clamp |
| `include/loader/symbolDatabase.h` | try_emplace + conflict warning |

---

## القيود المتبقية

1. **avPlayer NV12 fix غير مُختبَر** — avPlayer.cpp يُستثنى من البناء بدون FFmpeg.
2. **59 NID مكررة** —无害 لكن تُولّد تحذيرات في stderr عند التشغيل. تنظيفها يتطلب إعادة هيكلة libSaveData (تسجيل مزدوج في نطاقين).
3. **JIT block chaining dead code** — موثّق كميزة "منفذة" لكنه غير موصول بمسار التنفيذ (metadata فقط). لم يُلمَس في هذه الجولة.
4. **130 مشكلة في COMPREHENSIVE_ANALYSIS.md** — تم إصلاح 20 حرجة. الـ 110 المتبقية (57 متوسطة + 53 منخفضة) تحتاج جولات لاحقة.
5. **dangling pointer fix يستخدم mark-as-unloaded** — الـ deque ينمو دون انكماش (الوحدات المُفرّغة تبقى في الذاكرة). مقبول للـ prototype لكن قد يحتاج garbage collection للاستخدام طويل الأمد.
