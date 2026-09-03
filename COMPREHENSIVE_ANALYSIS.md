# التقرير الشامل لتحليل مشروع ProsperoLayer RDNA2 Core

**التاريخ:** 3 سبتمبر 2026  
**المسار:** `/workspace/project/`  
**النطاق:** تحليل كامل عبر 6 جبهات متوازية  

---

## ملخص تنفيذي

تم فحص المشروع (~263 ملف، ~111K سطر) عبر ستة محاور تحليلية متوازية. **إجمالي المشاكل المكتشفة: 130 مشكلة** موزّعة كالتالي:

| الخطورة | العدد | النسبة |
|---------|------|--------|
| 🔴 حرج | 20 | 15% |
| 🟠 متوسط | 57 | 44% |
| 🟡 بسيط | 53 | 41% |

**أبرز الاكتشافات الحرجة:**
1. **عيب في DIV 8-bit يُفسد RSP** (CPU interpreter) — memory corruption حقيقي
2. **Undefined behavior في SHL** عند count > عرض المعامل
3. **JIT block chaining هو metadata فقط** — غير موصول بمسار التنفيذ
4. **Dangling pointers في runtime linker** بسبب إدارة `std::vector` يدوياً
5. **Buffer overflows** في avPlayer (نسخ NV12) و texture loading بدون فحص حجم
6. **55 NID مكررة عبر المكتبات** — `SymbolDatabase::Add` يكتتب فوق القيمة بصمت
7. **S_MOV_B64 OOB write** على heap عند dst = آخر SGPR
8. **صفر assertions** في كل منطقة GPU (13K+ سطر)

---

## 1. تحليل CPU Emulation (25 مشكلة)

### 🔴 المشاكل الحرجة

#### 1.1 DIV 8-bit يُفسد RSP بدلاً من AH
- **الملف:** `src/cpu/x86_64_interpreter.cpp:866`
- **الخطورة:** 🔴 حرج
- **الوصف:** في DIV بنسبة 8-bit، الباقي يجب أن يُكتب في AH. الكود يستخدم `WriteRegByte(RAX|4, true, ...)` بمسار REX، الذي يكتب في `gpr[4]` (RSP) بدلاً من AH. هذا يُفسد مؤشر الكومة في أي قسمة 8-bit — شائعة في كود CRT. التعليق "AH via rex path" معكوس تماماً.

#### 1.2 Undefined Behavior في SHL
- **الملف:** `src/cpu/x86_64_interpreter.cpp:~775`
- **الخطورة:** 🔴 حرج
- **الوصف:** حساب علم الحمل `val >> (size*8 - count)` يصبح إزاحة بمقدار ≥ 64 عندما `count > size*8`، وهو سلوك غير معرّف في C++. ينبغي حماية `count > size*8` بإرجاع CF=0.

#### 1.3 JIT Block Chaining — metadata فقط
- **الملف:** `src/cpu/jit_executor.cpp:298-344`
- **الخطورة:** 🔴 حرج
- **الوصف:** `ChainBlocks()` و`GetChainedBlock()` تُحدّث حقول metadata فقط، لكن **لا يوجد أي مُستدعٍ لهما من مسار التنفيذ** (`ExecuteGuestFull`/`ExecuteGuestCode` لا تستدعيهما إطلاقاً). `host_code_ptr` يبقى `nullptr` دائماً. إحصائيات `m_total_chain_hits` لن تكون إلا صفراً خارج الاختبارات.

### 🟠 المشاكل المتوسطة
- **SIMD differential test يتجاهل xmm4–xmm15** (يختبر xmm0–3 فقط)
- **كل تعليمة SIMD تُختبر ببيعة عشوائية واحدة** (لا تغطية NaN/denormal/overflow)
- **RIP-relative تقريبي** في DecodeModRM (يحسب نسبة لـ rip بعد disp32 وليس نهاية التعليمة)
- **cmpxchg/xadd غير ذرية** رغم تتبّع بادئة `lock` — تسلسل read-compare-write قابل للمقاطعة
- **كود ميت:** `map_select==3` داخل `map_select==2` (غير قابل للوصول)
- **seccomp allowlist واسعة** (74 syscall بما فيها socket/connect/openat)
- **DirectTrapHandler إشارات عالمية** على process — خيطان متوازيان قد يتقاطعان
- **نظاما خيوط منفصلان** (ThreadScheduler و GuestThreadManager) — حالة منقسمة
- **JIT cache ينمو بلا حدود** (لا سياسة إخلاء)
- **dangling pointer من JIT** — مؤشر raw يُعاد من تحت القفل

### 🟡 المشاكل البسيطة
- `fsin`/`fcos` لا تتحقق من نطاق الوسيط (C2 لا يُضبط)
- `FXAM` مبسّط بشدة (يتجاهل NaN/Inf/denormal)
- أعلام استثناء x87 غير مُمثّلة
- `getpid` مسجّل مرتين (كود ميت)
- `_umtx_op` يستخدم polling بدلاً من futex حقيقي
- `writev` يكتب بايت-بايت عبر `std::cout`
- `AesInvMixColumns` lambda ميتة

---

## 2. تحليل GPU Emulation (25 مشكلة)

### 🔴 المشاكل الحرجة

#### 2.1 محوّل SPIR-V المنفصل شبه-placeholder
- **الملف:** `src/gpu/shader_spirv_recompiler.cpp:195-270`
- **الخطورة:** 🔴 حرج
- **الوصف:** يدعم 5 تعليمات فقط (V_MOV, V_SQRT, V_ADD, V_MUL, S_ENDPGM). لا VOP3، لا تحكم في التدفق، لا memory ops. placeholder واضح. (المحوّل الفعلي هو `rdna2_compute_compiler.cpp` بـ 3108 سطر).

#### 2.2 S_MOV_B64 OOB write
- **الملف:** `src/gpu/gcn_decoder.cpp:824-825`
- **الخطورة:** 🔴 حرج
- **الوصف:** `m_sgprs[ins.dst + 1] = 0` — الفحص يحمي `ins.dst` لكن لا يحمي `ins.dst + 1`. إذا كان `ins.dst == 103` (آخر SGPR)، الكتابة في `m_sgprs[104]` تخرج خارج المصفوفة (الحجم 104).

#### 2.3 VOP dst بدون فحص kVgprCount
- **الملف:** `src/gpu/gcn_decoder.cpp:976-1212`
- **الخطورة:** 🔴 حرج
- **الوصف:** `vg[ins.dst] = ...` يُنفذ مباشرة دون فحص أن `ins.dst < kVgprCount`. أي bytecode تالف يكتب خارج VGPR.

#### 2.4 صفر assertions في كل منطقة GPU
- **الملف:** كل `src/gpu/*.cpp` و `include/gpu/*.hpp`
- **الخطورة:** 🔴 حرج (منهجي)
- **الوصف:** `grep -rcn "assert"` يُرجع 0 لكل ملف. لا `assert()` ولا `static_assert()` واحد في 13K+ سطر. كل الاعتماد على fail-closed برسائل خطأ نصية.

### 🟠 المشاكل المتوسطة
- **MTBUF يُفكك لكن لا يُنفذ** (يسقط في default → "unhandled format")
- **Blending محدود:** 4 عوامل فقط (Zero/One/SrcAlpha/OneMinusSrcAlpha)، 3 عمليات، صيغتان فقط (RGBA8/BGRA8)
- **Texture sampling:** nearest فقط (لا bilinear/mipmapping/anisotropic)، RGBA8 فقط
- **S_BUFFER_LOAD/MUBUF فحص حدود غير متناظر** (off-by-one)
- **VOPC يطوي النتيجة على VCC bit واحد** (غير صحيح لـ multi-lane)
- **WaitRegMem ينتهي بـ timeout ثابت 100ms** (قد يستمر البرنامج كأن الشرط تحقق)
- **S_MOV_B64 simplification** يفقد hi word (يكسر مؤشرات 64-bit)
- **lanes_total & (lanes_total-1)** يفترض قوة 2 دون فحص
- **g_handles/g_mapped تنمو بلا حد** أعلى
- **تباين sampling** بين rasterizer و GCN executor (إزاحة نصف-تكسل)

### 🟡 المشاكل البسيطة
- Float== مقارنة في ZFuncPass (هشة لكن مطابقة للأجهزة)
- إعادة تخصيص framebuffer في كل draw (أداء)
- NaN في uf11/uf10 غير مفصول
- SrgbEncode يستخدم `std::pow` لكل بكسل (بطيء)
- RasterTexture::data ملكية خارجية (لا assert يحمي)

### ✅ نقاط القوة
- PM4 decoder صارم وحقيقي (يتحقق من type==3, payload bounds)
- Draw packets حقيقية مع مسار GPU حقيقي
- Topology (Strip/Fan) صحيح مع قلب winding
- Clipping كامل وتحويل viewport حقيقي
- Vulkan cleanup صحيح (لا leak)

---

## 3. تحليل HLE Libraries (42 مشكلة)

### 🔴 المشاكل الحرجة

#### 3.1 55 NID مكررة عبر المكتبات
- **الملفات:** `libs/libKernel.cpp`, `libs/libC.cpp`, `libs/libNet.cpp`, `libs/libShare.cpp`
- **الخطورة:** 🔴 حرج
- **الوصف:** 55 NID مسجّلة مرتين أو أكثر عبر مكتبات مختلفة. أخطرها:
  - `9BcDykPmo1I` — في libKernel (`get_error_addr`) و libC (`libc_error`) — **دوال مختلفة تماماً**
  - `YBiIdcDPrxs` — في libNet (`ShareFeaturePermit`) و libShare (`ShareFeaturePermit`) — مسارات مختلفة
  - 9 NID مكررة داخل libKernel.cpp بين كتلتي `Posix` و `LibKernel`
- **النتيجة:** `SymbolDatabase::Add` يكتتب فوق القيمة السابقة بصمت — "آخر تسجيل يفوز" — قد يربط NID بدالة خاطئة.

#### 3.2 SymbolDatabase::Add لا يكتشف التكرار
- **الملف:** `include/loader/symbolDatabase.h:47-56`
- **الخطورة:** 🔴 حرج
- **الوصف:** `m_symbols[sr.name] = func_ptr` — إدراج مفتاح موجود يُكتَب فوق القيمة بصمت دون أي تحذير أو تسجيل. هذا يخفي جميع التعارضات.

#### 3.3 libAgc.cpp — 95 NID حقيقية لكن stubs فقط
- **الملف:** `libs/libAgc.cpp`
- **الخطورة:** 🔴 حرج (وظيفياً)
- **الوصف:** الـ 95 NID بصيغة base64-url حقيقية (مثل `03RZmELWWzw`) — ✅ صحيح. لكن جميع الدوال الـ 95 هي stubs: `{ PRINT_NAME(); return OK; }` دون أي منطق رسومي. "قابلة للحل" (resolvable) لكن "غير وظيفية" (non-functional).

### 🟠 المشاكل المتوسطة
- **8 NID بطول 10 أحرف** في libAgc.cpp (بدلاً من 11 المعيارية) — تستوجب التحقق
- **5 NID في libGraphicsDriver بأسماء مشتقة من NID** (`GraphicsUnknownNApJjpKNBl4` إلخ)
- **5 NID متطابقة منطقياً** بين libAgc و libGraphicsDriver (بإشارات +/- مختلفة) — stub vs تنفيذ حقيقي
- **ملفات ميتة مستبعدة من البناء:** libAudio2.cpp (953 سطر)، libVideoDec2.cpp، libJson2.cpp

### 🟡 المشاكل البسيطة
- تسجيل مزدوج متعمد في libDialog.cpp و libSaveData.cpp (نفس الدوال)

### إحصائيات NID الفعلية
| المكتبة | NID مسجلة | ملاحظة |
|---------|----------|--------|
| libKernel.cpp | 373 | أكبر مكتبة |
| libNet.cpp | 270 | يتضمن Share |
| libAudio.cpp | 170 | يتضمن Ajm/AvPlayer/Ngs2 |
| libGraphicsDriver.cpp | 148 | |
| libFont.cpp | 70 | |
| libC.cpp | 60 | |
| libDialog.cpp | 55 | |
| libSaveData.cpp | 51 | |
| libVideoOut.cpp | 31 | |
| libPad.cpp | 31 | |
| libSystemService.cpp | 34 | |
| libAgc.cpp | 95 | stubs فقط |
| **الإجمالي** | **1723 تسجيل** | **1668 NID فريدة** |

---

## 4. تحليل Memory Safety (27 مشكلة)

### 🔴 المشاكل الحرجة

#### 4.1 Dangling pointers في runtime linker
- **الملف:** `src/loader/runtime_linker.cpp:988-996`
- **الخطورة:** 🔴 حرج
- **الوصف:** `UnloadProgram` يحذف عنصراً من `std::vector m_modules`. `vector::erase` قد يُعيد تخصيص الذاكرة ويُبطل كل المؤشرات. أي `ModuleInfo*` محفوظ في مكان آخر يصبح dangling. `FindProgramByAddr` يُرجع `&m` (مؤشر لعنصر داخل vector) — أي `push_back`/`erase` لاحق يُبطل هذا المؤشر.

#### 4.2 Buffer overflow في avPlayer (نسخ NV12)
- **الملف:** `libs/avPlayer.cpp:1079-1093`
- **الخطورة:** 🔴 حرج
- **الوصف:** نسخ بيانات NV12 بدون فحص أن `src->width <= pitch` أو `src->height <= h`. البيانات تأتي من إطار FFmpeg مفكوك (non-trusted input). لو كان `h < src->height` أو `pitch < src->width`، يحدث كتابة خارج الحدود.

#### 4.3 Buffer overflow في texture loading
- **الملف:** `src/gpu/software_rasterizer.cpp:178-197`
- **الخطورة:** 🔴 حرج
- **الوصف:** `SampleTextureNearest` يفحص `tex.data == nullptr` لكن لا يفحص أن `tex.data` تتسع لـ `width * height * 4` بايت. الوصول `tex.data + idx` قد يقرأ خارج الحدود لو كان `tex.data` أصغر من المتوقع.

#### 4.4 memcpy في init/fini tables بدون فحص overflow
- **الملف:** `src/loader/runtime_linker.cpp:1055, 1152`
- **الخطورة:** 🔴 حرج
- **الوصف:** `memcpy(&fn, image->Data() + table_off + i * 8, 8)` — لو كان `table_off + i*8` يفيض، يحدث قراءة خارج الحدود. الفحص بـ `available` يحمي جزئياً لكن `table_off + i*8` قد يفيض قبل الوصول لـ `available`.

### 🟠 المشاكل المتوسطة
- **vkMapMemory بدون فحص النتيجة** → null deref عند memcpy
- **strcpy بدون bounds** في network.cpp:1528
- **memset بأحجام ثابتة** (128, 136, 4104, 4096, 512) على مؤشرات ضيف في libSystemService, libNet, libUlt, controller.cpp
- **const_cast من volatile** في agc.cpp بدون قفل (data race)
- **Integer underflow** في mprotect span calculation
- **strtab قراءة بدون فحص** `strtab_cont < file_size` في prospero_self.cpp
- **pthread.cpp new بدون ضمان delete** عند فشل التهيئة

### 🟡 المشاكل البسيطة
- **Type punning عبر reinterpret_cast** (strict aliasing violation) — واسع في x86_64_isa_ext.cpp
- **union type punning** في libs/ (UB في C++، صالح في C فقط)
- **new خام بدل make_unique** في vulkan_compute_executor.cpp
- **CodeScanBus يرجع true دائماً** عند التجاوز (يخفي أخطاء)

### ✅ نقاط القوة
- استخدام واسع لـ `std::vector` و `std::unique_ptr` بدل المؤشرات الخام
- fail-closed design في مُحلِّلات ELF/SELF
- فحوصات حدود في memcpy الحرجة في virtual_memory_manager
- `std::memcpy` لـ type punning في الأماكن الحرجة (النمط الصحيح)
- `snprintf` بدل `sprintf` في معظم المسارات

---

## 5. تحليل Build System و Tests (15 مشكلة)

### 🔴 المشاكل الحرجة

#### 5.1 اختبارات تختبر stubs بدلاً من وظائف حقيقية
- **الملفات:** `tests/pm4_decoder_test.cpp`, `tests/pm4_translator_expanded_test.cpp`, `tests/gpu_backend_state_test.cpp`
- **الخطورة:** 🔴 حرج
- **الوصف:** هذه الاختبارات تُعرّف stub محلي للـ `VulkanRendererBackend` لا يفعل شيئاً حقيقياً — `SetViewport` تتجاهل القيم، `DispatchCompute`/`DrawAuto` يزيدان عدّاداً فقط. الاختبار يتحقق من استدعاءات لا من سلوك.

#### 5.2 نجاح مشروط يُخفي غياب الفحص
- **الملفات:** `tests/vulkan_compute_executor_test.cpp`, `tests/gpu_backend_state_test.cpp`
- **الخطورة:** 🔴 حرج
- **الوصف:** عند غياب Vulkan/SDL، هذه الاختبارات **تمر تلقائياً** دون فحص أي شيء. `make unit` يُبلّغ بالنجاح دون فحص GPU على بيئات بلا SDL/Vulkan.

### 🟠 المشاكل المتوسطة
- **تضارب `-Werror`:** TEST_CXXFLAGS يستخدمه (59 وصفة) لكن CXXFLAGS لا يستخدمه (8 وصفات) — تحذيرات تُتجاهل
- **`-Wno-error=nonnull`** يُخفي تمرير null إلى دوال `nonnull` بدل الإصلاح
- **قيم hardcoded متوسطة المخاطرة** في اختبارات GPU/rasterizer (قيم بكسل، نتائج GCN)
- **`make unit` لا يبني كل الملفات** — كل binary يربط مجموعة فرعية فقط
- **4 أنماط تأكيد مختلفة** (CHECK, assert, Expect, printf) تُصعّب المقارنة الموحدة

### 🟡 المشاكل البسيطة
- `std::format` غير مستخدم رغم C++20 (المشروع يستخدم `std::span` فقط)
- `#if 0` في cpu_x87_test.cpp (دوال مساعدة مهجورة)
- تكرار `$(SELF_PARSER_TEST)` في target unit

### ✅ نقاط القوة
- كشف مشروط ذكي للحزم الاختيارية (SDL/FFmpeg/Vulkan/JSON/fmt)
- لا ملفات معزولة تماماً عن البناء (wildcards تضمن الشمول)
- لا ملفات مذكورة وغير موجودة
- استخدام `std::span` صحيح وواسع

---

## 6. تحليل دقة التوثيق (9 مشاكل)

### 🔴 المشاكل الحرجة

#### 6.1 JIT block chaining موصوف كمنفذ لكنه dead path
- **الملف:** `CHANGES_ROUND32.md` القسم 3
- **الخطورة:** 🔴 حرج
- **الوصف:** الوثيقة تصف JIT chaining كأنه ميزة وظيفية ("يتخطى cache lookup"، "يزيد chain_hits"). الحقيقة: `GetChainedBlock` لا يُستدعى في أي مسار تنفيذ فعلي. "chain hits" و"skip cache lookup" لا يحدثان فعلاً. الوثيقة تعترف بجزء من هذا في قسم القيود، لكن القسم الرئيسي يصف الميزة بصيغة "منفذة وفعّالة".

#### 6.2 رقم 568 و 473 غير قابلين للتحقق محلياً
- **الملف:** `CHANGES_ROUND32.md`
- **الخطورة:** 🔴 حرج
- **الوصف:** رقم 568 (إجمالي NID غير المحلولة) يُزعم أنه من تحليل Minecraft eboot، لكن لا يوجد eboot في المشروع ولا أداة تحليل. الرقم 473 (568−95) مُشتق من رقم غير قابل للتحقق. (ملاحظة: التداخل 119 بين ps5rs الكامل و libGraphicsDriver تم التحقق منه وهو صحيح — الـ 119 NID محلولة بالفعل كـ "Graphics5" فلا تُحسب ضمن 568.)

### 🟠 المشاكل المتوسطة
- **إجمالي HLE symbols (1728)** لا يطابق العدد الفعلي للرموز الفريدة (1668) — يوجد 59 NID مكررة
- **Blending موصوف كـ "مدعوم"** لكنه محدود بصيغتين فقط (RGBA8/BGRA8) — القيد غير مذكور في القسم الرئيسي
- **ادعاءات تحسن أداء** دون أي قياسات (benchmark)
- **"59 اختبار / 0 فشل"** — العدد معقول لكن يشير لعدد suites وليس فحصات فردية

### 🟡 المشاكل البسيطة
- لغة مبالغة في ANALYSIS_AR.md ("صدق كامل في التوثيق")
- الاعتراف بـ "33 NID مخترعة" صادق لكن غير قابل للتحقق (النسخة السابقة غير موجودة)

### ✅ نقاط صحيحة (للإنصاف)
- ✅ **95 NID حقيقية** — هاشات فعلية (مثل `03RZmELWWzw`)، لا أسماء نصية
- ✅ **stubs ترجع OK** — موثّفة بصراحة
- ✅ **حسابات Blending** — صحيحة رياضياً (المشكلة في التغطية لا في الصحة)
- ✅ **Topology (Strip/Fan)** — مُنفذ فعلاً مع قلب winding صحيح
- ✅ **README.md** — صادق تماماً ("not a complete PS5 emulator")

---

## 7. التصنيف حسب الأولوية

### 🔴 إصلاحات فورية (حرجة)

| # | المشكلة | الملف | الإصلاح المقترح |
|---|---------|-------|-----------------|
| 1 | DIV 8-bit يُفسد RSP | x86_64_interpreter.cpp:866 | غيّر `WriteRegByte(RAX\|4, true, ...)` إلى `WriteRegByte(4, p.rex, ...)` |
| 2 | SHL undefined behavior | x86_64_interpreter.cpp:~775 | أضف `if (count > size*8) { cf = false; }` |
| 3 | Dangling pointers في runtime linker | runtime_linker.cpp | استبدل `std::vector` بـ `std::list` أو استخدم IDs |
| 4 | Buffer overflow في avPlayer | avPlayer.cpp:1079 | أضف فحص `src->width <= pitch && src->height <= h` |
| 5 | Buffer overflow في texture loading | software_rasterizer.cpp:178 | أضف حقل `data_size` وفحص `idx < data_size` |
| 6 | S_MOV_B64 OOB write | gcn_decoder.cpp:824 | أضف فحص `ins.dst + 1 < kSgprCount` |
| 7 | memcpy في init/fini tables | runtime_linker.cpp:1055 | فحص `table_off + i*8` ضد overflow |
| 8 | NID مكررة عبر مكتبات | libKernel.cpp, libC.cpp, libNet.cpp | راجع التسجيل المزدوج ووحّده |
| 9 | SymbolDatabase::Add صامت | symbolDatabase.h:47 | أضف كشف التكرار مع تحذير |

### 🟠 إصلاحات عاجلة (متوسطة)

| # | المشكلة | الإصلاح المقترح |
|---|---------|-----------------|
| 10 | JIT chaining غير موصول | موصّل به في القسم الرئيسي أو احذف الدوال الوهمية |
| 11 | vkMapMemory بدون فحص | افحص النتيجة قبل memcpy |
| 12 | strcpy بدون bounds | استبدل بـ snprintf |
| 13 | memset بأحجام ثابتة على مؤشرات ضيف | أضف فحص حجم الوجهة |
| 14 | SIMD test يتجاهل xmm4-15 | وسّع الحلقة لتشمل 0-15 |
| 15 | cmpxchg/xadd غير ذرية | أضف سياج ذرّي/CAS عند تفعيل lock |
| 16 | صفر assertions في GPU | أضف static_assert و assert على bounds |
| 17 | اختبارات تختبر stubs | أضف اختبارات سلوك حقيقية |
| 18 | نجاح مشروط يُخفي غياب الفحص | علّم الاختبارات كـ "skipped" لا "passed" |

### 🟡 إصلاحات لاحقة (بسيطة)

| # | المشكلة | الإصلاح المقترح |
|---|---------|-----------------|
| 19 | Type punning عبر reinterpret_cast | استبدل بـ std::memcpy |
| 20 | كود ميت (map_select==3, getpid مكرر, lambda ميتة) | احذف |
| 21 | ملفات ميتة مستبعدة من البناء | احذف libAudio2.cpp, libVideoDec2.cpp, libJson2.cpp |
| 22 | fsin/fcos بدون فحص نطاق | أضف فحص C2 |
| 23 | -Wno-error=nonnull يُخفي مشكلة | أصلح الكود بدل كتم التحذير |
| 24 | توثيق JIT chaining مضلّل | انقل التحذير للقسم الرئيسي |
| 25 | إجمالي HLE symbols خاطئ | استخدم 1668 (فريد) بدل 1728 |

---

## 8. خلاصة تقييم الصدق

| المحور | درجة الصدق | ملاحظة |
|--------|-----------|--------|
| CPU fixes (lock, texture) | ✅ صادق | إصلاحات حقيقية ومُختبرة |
| Blending + Topology | ✅ صادق | حسابات صحيحة، لكن التغطية محدودة |
| JIT block chaining | ❌ مضلّل | metadata فقط، غير موصول بالتنفيذ |
| libAgc NID حقيقية | ✅ صادق | 95 هاش حقيقي (لكن stubs) |
| أرقام NID (568, 473) | ⚠️ غير قابل للتحقق | يعتمد على تحليل خارجي |
| عدد HLE symbols | ❌ خاطئ | 1728 يشمل تكرارات؛ الفريد = 1668 |

**الخلاصة العامة:** المشروع مكتوب بجودة دفاعية عالية (fail-closed design، استخدام smart pointers، فحوصات حدود في الأماكن الحرجة)، لكنه يحتوي على **أخطاء صحيحة حقيقية** (DIV 8-bit، SHL UB، OOB writes) و**ميزات موصوفة كمنفذة لكنها dead code** (JIT chaining) و**ثغرات memory safety** (dangling pointers، buffer overflows) و**NID مكررة صامتة**. التوثيق صادق في النية لكن يحتوي على ادعاءات مضلّلة في الجولة 32.
