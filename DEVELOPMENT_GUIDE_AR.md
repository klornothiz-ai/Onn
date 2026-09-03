# دليل التطوير الشامل - مشروع ProsperoLayer RDNA2 Core

## 📖 نظرة عامة

هذا دليل التطوير الشامل لمشروع محاكاة PS5 (ProsperoLayer). يهدف هذا الدليل إلى توفير كل المعلومات اللازمة للمطورين للمساهمة في تطوير المشروع.

---

## 🎯 أهداف المشروع

1. **محاكاة PS5/PS4** - إنشاء محاكاة كاملة لأجهزة PlayStation
2. **الأمان** - ضمان أمان التنفيذ وحماية النظام المضيف
3. **الأداء** - تحقيق أداء عالي يقترب من الأجهزة الحقيقية
4. **التوافق** - دعم أكبر قدر ممكن من الألعاب

---

## 🏗️ بنية المشروع

```
prosperolayer-rdna2-core/
├── include/                  # ملفات الهيدر (التعريفات)
│   ├── common/              # أدوات مشتركة
│   ├── kernel/              # نواة المحاكاة
│   ├── graphics/            # مكونات الرسوميات
│   └── loader/              # محمّل ELF
├── src/                     # الكود المصدري
│   ├── cpu/                 # معالج المحاكاة
│   ├── gpu/                 # محرك الرسوميات
│   ├── kernel/              # تنفيذات النواة
│   ├── loader/              # محمّل ELF
│   ├── memory/              # مدير الذاكرة
│   └── common/              # أدوات مشتركة
├── libs/                    # مكتبات HLE
├── tests/                   # اختبارات
├── docs/                    # التوثيق
├── Makefile                 # نظام البناء الرئيسي
├── CMakeLists.txt           # نظام البناء العابر للمنصات
└── README.md                # وصف المشروع
```

---

## 🛠️ المتطلبات الأساسية

### للبناء:
- **Compilateur:** g++ أو clang++ مع دعم C++20
- **نظام التشغيل:** Linux x86-64
- **أدوات البناء:** make أو cmake

### للمكتبات الاختيارية:
- **SDL2:** للرسوميات النافذية
- **FFmpeg:** لفك ترميز الفيديو
- **nlohmann/json:** للتعامل مع JSON
- **fmt:** لتنسيق النصوص

### تثبيت المتطلبات على Ubuntu/Debian:
```bash
# المتطلبات الأساسية
sudo apt update
sudo apt install build-essential cmake git

# المكتبات الاختيارية
sudo apt install libsdl2-dev libavcodec-dev libavformat-dev libavutil-dev \
                 libswresample-dev libswscale-dev libfmt-dev nlohmann-json3-dev
```

---

## 🔨 نظام البناء

### البناء الأساسي (Make):
```bash
# بناء المحرك الكامل
make all

# بناء الاختبارات فقط
make unit

# تشغيل الاختبارات
make test

# تنظيف ملفات البناء
make clean
```

### البناء العابر للمنصات (CMake):
```bash
# إنشاء مجلد البناء
cmake -S . -B build/cmake -DPROSPERO_BUILD_TESTS=ON

# بناء المشروع
cmake --build build/cmake

# تشغيل الاختبارات
ctest --test-dir build/cmake --output-on-failure
```

### خيارات البناء المتقدمة:
```bash
# بناء مع تحسينات الأداء
make CXXFLAGS="-O3 -march=native"

# بناء للتنقيب (للتطوير)
make CXXFLAGS="-g -O0"

# بناء مع تحذيرات صارمة
make CXXFLAGS="-Wall -Wextra -Wpedantic -Werror"
```

---

## 🧪 نظام الاختبارات

### الاختبارات المتاحة:
1. **cpu_interpreter_test** - اختبار مترجم التعليمات
2. **jit_executor_test** - اختبار منفذ JIT
3. **vmm_elf_loader_test** - اختبار مدير الذاكرة ومحمل ELF
4. **pm4_decoder_test** - اختبار فك ترميز PM4
5. **rdna2_spirv_recompiler_test** - اختبار مترجم RDNA2
6. **gpu_backend_state_test** - اختبار حالة GPU
7. **game_folder_test** - اختبار مجلد الألعاب

### تشغيل الاختبارات:
```bash
# تشغيل جميع الاختبارات
make test

# تشغيل اختبار محدد
./build/tests/cpu_interpreter_test

# تشغيل الاختبارات مع تفاصيل
./build/tests/cpu_interpreter_test --verbose
```

### إضافة اختبار جديد:
1. أنشئ ملف اختبار جديد في مجلد `tests/`
2. أضف قاعدة بناء في Makefile
3. أضف الاختبار إلى قائمة الاختبارات

---

## 📝 المعايير البرمجية

### أسلوب الكتابة:
- استخدام **C++20** والميزات الحديثة
- اتّباع أسلوب **Google C++ Style Guide**
- استخدام **RAII** لإدارة الموارد
- استخدام **Smart Pointers** بدلاً من المؤشرات العادية

### التسمية:
- **الكلاسات:** PascalCase (مثال: `VirtualMemoryManager`)
- **الدوال:** camelCase (مثال: `translateInstruction`)
- **المتغيرات:** snake_case (مثال: `guest_address`)
- **الثوابت:** UPPER_SNAKE_CASE (مثال: `MAX_MEMORY_SIZE`)
- **ملفات الهيدر:** snake_case.h (مثال: `virtual_memory_manager.h`)
- **الملفات المصدرية:** snake_case.cpp (مثال: `virtual_memory_manager.cpp`)

### التوثيق:
- استخدام **Doxygen** للتوثيق
- توثيق جميع الدوال العامة
- توثيق المعاملات والإرجاع
- إضافة أمثلة للاستخدام

---

## 🔧 المكونات الرئيسية

### 1. معالج المحاكاة (CPU)
- **الموقع:** `src/cpu/`
- **المسؤوليات:** ترجمة وتنفيذ تعليمات الضيفة
- **المكونات:**
  - `x86_64_subset_interpreter.cpp` - مترجم التعليمات
  - `jit_executor.cpp` - منفذ JIT
  - `thread_scheduler.cpp` - مجدول الخيوط
  - `prospero_syscalls.cpp` - معالجة syscalls

### 2. محرك الرسوميات (GPU)
- **الموقع:** `src/gpu/`
- **المسؤوليات:** معالجة الرسوميات وترجمتها إلى Vulkan
- **المكونات:**
  - `vulkan_backend.cpp` - محرك Vulkan
  - `pm4_decoder.cpp` - فك ترميز أوامر PM4
  - `pm4_translator.cpp` - ترجمة PM4 إلى أوامر Vulkan
  - `shader_spirv_recompiler.cpp` - ترجمة الشيدرز إلى SPIR-V

### 3. نواة المحاكاة (Kernel)
- **الموقع:** `src/kernel/`
- **المسؤوليات:** محاكاة خدمات النظام
- **المكونات:**
  - `event_flag.cpp` - أعلام الأحداث
  - `semaphore.cpp` - الم녕زات
  - `file_system.cpp` - نظام الملفات
  - `memory_extra.cpp` - خدمات الذاكرة الإضافية
  - `posix_wrappers.cpp` - أغلفة POSIX

### 4. مدير الذاكرة (Memory)
- **الموقع:** `src/memory/`
- **المسؤوليات:** إدارة الذاكرة الافتراضية
- **المكونات:**
  - `virtual_memory_manager.cpp` - المدير الرئيسي

### 5. محمّل ELF (Loader)
- **الموقع:** `src/loader/`
- **المسؤوليات:** تحميل ملفات ELF
- **المكونات:**
  - `elf_loader.cpp` - محمّل ELF
  - `runtime_linker.cpp` - مeshire الروابط في Runtime
  - `game_folder.cpp` - إدارة مجلدات الألعاب

---

## 🎮 مكتبات HLE

### المكتبات المتاحة:
- **libKernel** - محاكاة خدمات النواة
- **libGraphicsDriver** - محاكاة التعريفات الرسومية
- **libNet** - خدمات الشبكة
- **libAudio** - خدمات الصوت
- **libVideoDec2** - فك ترميز الفيديو
- **libJson2** - التعامل مع JSON
- **controller** - التعامل مع التحكم
- **dialog** - النوافذ الحوارية

### إضافة مكتبة HLE جديدة:
1. أنشئ ملف جديد في مجلد `libs/`
2. أضف التعريف في ملف الهيدر المناسب
3. سجّل الدوال في `libs.cpp`
4. أضف اختبارات للدوال الجديدة

---

## 🐛 استكشاف الأخطاء وإصلاحها

### أدوات التنقيب:
```bash
# بناء مع معلومات التنقيب
make CXXFLAGS="-g -O0"

# استخدام GDB
gdb ./ps5_native_vulkan_emulator

# استخدام Valgrind
valgrind --leak-check=full ./ps5_native_vulkan_emulator
```

### مشاكل شائعة:
1. **مشاكل البناء:** تأكد من تثبيت جميع المتطلبات
2. **مشاكل الذاكرة:** استخدم Valgrind لاكتشاف التسريبات
3. **مشاكل الأداء:** استخدم profilers مثل perf أو gprof

---

## 📊 الأداء

### قياس الأداء:
```bash
# بناء مع تحسينات الأداء
make CXXFLAGS="-O3 -march=native"

# تشغيل مع قياس الأداء
time ./ps5_native_vulkan_emulator

# استخدام perf
perf record ./ps5_native_vulkan_emulator
perf report
```

### تحسينات الأداء:
1. استخدام `-O3` للتحسين الشامل
2. استخدام `-march=native` للتحسين المعماري
3. تجنب النسخ غير الضروري للبيانات
4. استخدام هياكل بيانات فعّالة

---

## 🔒 الأمان

### مبادئ الأمان:
1. **لا تنسخ بايتات الضيفة إلى ذاكرة RWX**
2. **تحقق من صلاحيات الذاكرة دائماً**
3. **ارفض النطاقات غير الم映射ة**
4. **استخدم البناء الآمن مع `-Werror`**

### اختبار الأمان:
```bash
# بناء مع فحص الأمان
make CXXFLAGS="-fsanitize=address -fsanitize=undefined"

# تشغيل مع فحص الأمان
./ps5_native_vulkan_emulator
```

---

## 📈 التقدم المستقبلي

### الأولويات:
1. **توسيع تغطية HLE** - إضافة المزيد من الدوال
2. **تحسين الأداء** - تحسين المحاكاة
3. **دعم الألعاب** - اختبار توافق الألعاب
4. **تحسين التوثيق** - تحسين الوثائق

### كيف تساهم:
1. اقرأ_issues المفتوحة على GitHub
2. اختر مهمة تناسب مستواك
3. أنشئ fork للمشروع
4. أجرِ التغييرات المطلوبة
5. أرسل Pull Request

---

## 📞 التواصل

- **GitHub Issues:** للإبلاغ عن الأخطاء وطلب الميزات
- **Discord:** للتواصل مع المطورين
- **البريد الإلكتروني:** للمشاكل الخاصة

---

## 📜 الترخيص

هذا المشروع مرخّص تحت رخصة MIT. يمكنك استخدامه وتعديله وتوزيعه بحرية.

---

**آخر تحديث:** 2026-08-24
**الإصدار:** v1.0