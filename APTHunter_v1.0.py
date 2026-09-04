# -*- coding: utf-8 -*-
"""
APTHunter v1.0 -- مراقب سلوك APT (جانب الدفاع الأزرق)
=====================================================
أداة مراقبة وكشف بقراءة فقط: لا تعدّل النظام، لا تعطّل أي حماية،
ولا تنشئ أو تحذف أو تتصل بأي شيء. هدفها عرض مؤشرات الاختراق (IOCs)
لتدريب فرق الاستجابة على اكتشافها والتعامل معها.

الفصول الأربعة:
  1) حالة الحماية : Windows Defender + جدار الحماية + UAC
  2) الباب الخلفي : مفاتيح Run + مجلدات Startup + المهام المجدولة
  3) الانتشار     : مسح الشبكة المحلية (ping + فحص منافذ SMB)
  4) الإخفاء      : أحداث مسح سجلات الأحداث (1102 / 104)

التشغيل:      python APTHunter_v1.0.py    (على Windows 10/11)
الاعتماديات:  مكتبات قياسية فقط (winreg/subprocess/socket) --
              حزمة wmi اختيارية كبديل احتياطي لفحص Defender.
يُفضّل تشغيله بصلاحيات مسؤول لقراءة بعض الحقول كاملة؛
الأداة نفسها لا تحتاج صلاحيات للتعديل لأنها لا تعدّل شيئاً.
"""

import os
import re
import sys
import socket
import subprocess
import datetime
import concurrent.futures
from collections import OrderedDict

# إخفاء نوافذ الأوامر المنبثقة عند استدعاء أدوات النظام
CREATE_NO_WINDOW = 0x08000000 if os.name == "nt" else 0

try:
    import winreg
    HAS_WINREG = True
except ImportError:
    winreg = None
    HAS_WINREG = False

LINE = "=" * 70


def run_cmd(cmd, timeout=40):
    """تنفيذ أمر خارجي بصمت وإرجاع نص ناتجه (قراءة فقط)."""
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            errors="replace",
            creationflags=CREATE_NO_WINDOW,
            timeout=timeout,
        )
        return (proc.stdout or "") + "\n" + (proc.stderr or "")
    except Exception:
        return ""


def banner(title):
    """فاصل عرض لقسم من التقرير."""
    print("\n" + LINE)
    print("  " + title)
    print(LINE)


# ---------------------------------------------------------------- الفصل 1 --
# حالة الحماية: قراءة الحالة الحالية فقط، دون أي تعطيل.
# ----------------------------------------------------------------


def check_defender():
    """حالة Windows Defender عبر PowerShell، مع بديل WMI اختياري."""
    print("[1.1] Windows Defender")
    out = run_cmd([
        "powershell", "-NoProfile", "-NonInteractive", "-Command",
        "Get-MpComputerStatus | Select-Object AntivirusEnabled,"
        "RealTimeProtectionEnabled,AntivirusSignatureLastUpdated | "
        "Format-List | Out-String",
    ])
    fields = {
        "AntivirusEnabled": None,
        "RealTimeProtectionEnabled": None,
        "AntivirusSignatureLastUpdated": None,
    }
    for name in fields:
        m = re.search(re.escape(name) + r"\s*:\s*(.+)", out, re.I)
        if m:
            fields[name] = m.group(1).strip()

    if all(v is None for v in fields.values()):
        # بديل احتياطي عبر حزمة wmi إن كانت مثبتة على الجهاز
        try:
            import wmi
            status = wmi.WMI(namespace=r"root\Microsoft\Windows\Defender")
            item = status.MSFT_MpComputerStatus()[0]
            fields["AntivirusEnabled"] = str(item.AntivirusEnabled)
            fields["RealTimeProtectionEnabled"] = str(item.RealTimeProtectionEnabled)
            fields["AntivirusSignatureLastUpdated"] = str(
                getattr(item, "AntivirusSignatureLastUpdated", ""))
        except Exception:
            print("   ! تعذّر قراءة الحالة (قد تمنع سياسة المنظمة الاستعلام).")
            return None

    ok = True
    for label, key in (("الحماية في الوقت الحقيقي", "RealTimeProtectionEnabled"),
                       ("برنامج الحماية مفعّل", "AntivirusEnabled")):
        val = fields[key]
        if val is None:
            print(f"   ? {label}: غير معروف")
        elif str(val).lower().startswith("true"):
            print(f"   + {label}: مفعّل")
        else:
            print(f"   ! {label}: معطّل")
            ok = False
    updated = fields["AntivirusSignatureLastUpdated"]
    if updated and updated != "None":
        print(f"   + آخر تحديث للتوقيعات: {updated}")
    return ok


def check_firewall():
    """حالة جدار الحماية لكل ملف شخصي عبر netsh (بديل: PowerShell)."""
    print("[1.2] جدار الحماية (Windows Firewall)")
    profiles = {"domain": None, "private": None, "public": None}
    out = run_cmd(["netsh", "advfirewall", "show", "allprofiles", "state"])
    current = None
    for line in out.splitlines():
        m = re.search(r"\b(Domain|Private|Public)\s+Profile", line, re.I)
        if m:
            current = m.group(1).lower()
            continue
        m = re.search(r"\bState\s+(\S+)", line, re.I)
        if m and current:
            profiles[current] = m.group(1).upper() == "ON"

    if all(v is None for v in profiles.values()):
        out = run_cmd([
            "powershell", "-NoProfile", "-NonInteractive", "-Command",
            "Get-NetFirewallProfile | ForEach-Object { $_.Name + '=' + $_.Enabled }",
        ])
        for line in out.splitlines():
            key, _, val = line.partition("=")
            if key.strip().lower() in profiles:
                profiles[key.strip().lower()] = val.strip().lower() == "true"

    ok = True
    for name, enabled in profiles.items():
        if enabled is None:
            print(f"   ? الملف الشخصي {name}: غير معروف")
        elif enabled:
            print(f"   + الملف الشخصي {name}: مفعّل")
        else:
            print(f"   ! الملف الشخصي {name}: معطّل")
            ok = False
    return ok


def check_uac():
    """حالة UAC من سجل النظام (مفتاح EnableLUA)."""
    print("[1.3] UAC (التحكم في حسابات المستخدمين)")
    if not HAS_WINREG:
        print("   ! وحدة winreg غير متاحة (بيئة غير Windows).")
        return None
    try:
        key = winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System",
        )
        value, _ = winreg.QueryValueEx(key, "EnableLUA")
        winreg.CloseKey(key)
    except FileNotFoundError:
        print("   ! مفتاح UAC غير موجود في السجل (غالباً معطّل أو نظام قديم).")
        return False
    except OSError as err:
        print(f"   ! تعذّرت القراءة: {err}")
        return None

    enabled = int(value) == 1
    if enabled:
        print("   + UAC: مفعّل")
    else:
        print("   ! UAC: معطّل -- نمط شائع لدى أدوات APT قبل تصعيد الصلاحيات")
    return enabled


# ---------------------------------------------------------------- الفصل 2 --
# الباب الخلفي (محاكاة): رصد نقاط البدء التلقائي فقط، دون إنشاء أي شيء.
# ----------------------------------------------------------------

# مجلدات Startup للمستخدم الحالي وجميع المستخدمين
STARTUP_DIRS = [
    r"%APPDATA%\Microsoft\Windows\Start Menu\Programs\Startup",
    r"%ProgramData%\Microsoft\Windows\Start Menu\Programs\Startup",
]


def read_run_keys():
    """قراءة مفاتيح Run/RunOnce من السجل وعرض أسماء وقيم المداخل."""
    if not HAS_WINREG:
        print("   ! وحدة winreg غير متاحة (بيئة غير Windows).")
        return 0
    # مفاتيح السجل للمستخدم الحالي ولجميع المستخدمين (بنسختي 32/64)
    run_paths = [
        (winreg.HKEY_LOCAL_MACHINE,
         r"SOFTWARE\Microsoft\Windows\CurrentVersion\Run"),
        (winreg.HKEY_LOCAL_MACHINE,
         r"SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce"),
        (winreg.HKEY_LOCAL_MACHINE,
         r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Run"),
        (winreg.HKEY_LOCAL_MACHINE,
         r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\RunOnce"),
        (winreg.HKEY_CURRENT_USER,
         r"SOFTWARE\Microsoft\Windows\CurrentVersion\Run"),
        (winreg.HKEY_CURRENT_USER,
         r"SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce"),
    ]
    print("   -- مفاتيح التشغيل التلقائي في السجل (Run / RunOnce):")
    found = 0
    for hive, path in run_paths:
        try:
            key = winreg.OpenKey(hive, path)
        except FileNotFoundError:
            continue
        except OSError as err:
            print(f"   ! {path}: {err}")
            continue
        try:
            index = 0
            while True:
                try:
                    name, value, _ = winreg.EnumValue(key, index)
                except OSError:
                    break
                index += 1
                if not value:
                    continue  # قيم فارغة لا تفيد كمؤشر
                found += 1
                print(f"     * {name}  =  {value}")
        finally:
            winreg.CloseKey(key)
    if not found:
        print("     (لا توجد مداخل)")
    return found


def list_startup_folders():
    """سرد الملفات الموجودة في مجلدات Startup (بدون تشغيلها)."""
    print("   -- مجلدات Startup:")
    found = 0
    for directory in STARTUP_DIRS:
        path = os.path.expandvars(directory)
        if not os.path.isdir(path):
            continue
        for name in os.listdir(path):
            if name.lower() == "desktop.ini":
                continue
            full = os.path.join(path, name)
            if os.path.isfile(full):
                found += 1
                print(f"     * {full}")
    if not found:
        print("     (المجلدات فارغة أو غير موجودة)")
    return found


def list_scheduled_tasks():
    """سرد المهام المجدولة النشطة (تُخفيها أدوات APT في مجلدات مخصصة)."""
    print("   -- المهام المجدولة غير المعطّلة:")
    out = run_cmd([
        "powershell", "-NoProfile", "-NonInteractive", "-Command",
        "Get-ScheduledTask -ErrorAction SilentlyContinue | "
        "Where-Object { $_.State -ne 'Disabled' } | "
        "ForEach-Object { $_.TaskPath + $_.TaskName + '  [State: ' + $_.State + ']' }",
    ])
    tasks = [ln.strip() for ln in out.splitlines() if ln.strip()]
    if not tasks:
        print("     (لا توجد مهام نشطة أو تعذّر الاستعلام)")
        return 0
    for task in tasks[:40]:
        print(f"     * {task}")
    extra = len(tasks) - 40
    if extra > 0:
        print(f"     ... و{extra} مهمة أخرى")
    return len(tasks)


# ---------------------------------------------------------------- الفصل 3 --
# الانتشار (محاكاة): اكتشاف الأجهزة المتصلة بالشبكة المحلية.
# لا نسخ ذاتي ولا اتصال بأي جهاز -- فحص ping ومنافذ فقط.
# ----------------------------------------------------------------

IPV4_RE = re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}\b")


def mask_to_cidr(mask):
    """تحويل قناع نقطي إلى عدد بتات الشبكة (مثال: 255.255.255.0 -> 24)."""
    try:
        return sum(bin(int(part)).count("1") for part in mask.split("."))
    except ValueError:
        return 24


def prefix_to_mask(prefix):
    """تحويل طول البادئة إلى قناع نقطي (مثال: 24 -> 255.255.255.0)."""
    binary = ("1" * prefix).ljust(32, "0")
    return ".".join(str(int(binary[i * 8:(i + 1) * 8], 2)) for i in range(4))


def get_local_network():
    """استخراج (عنوان IPv4، قناع) لأول واجهة شبكة مناسبة من ipconfig."""
    out = run_cmd(["ipconfig"])
    pairs = []
    candidate = None
    for line in out.splitlines():
        m = re.search(r"IPv4[^:]{0,40}:", line, re.I)
        if m:
            m2 = IPV4_RE.search(line)
            candidate = m2.group(0) if m2 else None
            continue
        if "255." in line:
            m2 = IPV4_RE.search(line)
            if m2 and candidate:
                pairs.append((candidate, m2.group(0)))
                candidate = None

    if not pairs:  # بديل: PowerShell للأسماء غير الإنجليزية
        out = run_cmd([
            "powershell", "-NoProfile", "-NonInteractive", "-Command",
            "Get-NetIPAddress -AddressFamily IPv4 | "
            "Where-Object { $_.IPAddress -ne '127.0.0.1' -and "
            "$_.IPAddress -notlike '169.254.*' } | "
            "ForEach-Object { $_.IPAddress + ' ' + $_.PrefixLength }",
        ])
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 2:
                try:
                    pairs.append((parts[0], prefix_to_mask(int(parts[1]))))
                except ValueError:
                    continue

    for ip, mask in pairs:
        first, second = (int(o) for o in ip.split(".")[:2])
        rfc1918 = (first == 10
                   or (first == 172 and 16 <= second <= 31)
                   or (first == 192 and second == 168))
        if first in (0, 127, 255):
            continue
        if rfc1918:
            return ip, mask
    # لا يوجد عنوان خاص: نأخذ أول عنوان واجهة صالح
    for ip, mask in pairs:
        first = int(ip.split(".")[0])
        if first not in (0, 127, 169, 255):
            return ip, mask
    return None, None


def probe_smb_ports(ip, ports=(135, 139, 445), timeout=0.4):
    """فحص سريع لمنافذ ويندوز لتمييز الأجهزة المستجيبة رغم حجب ICMP."""
    for port in ports:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
                sock.settimeout(timeout)
                if sock.connect_ex((ip, port)) == 0:
                    return port
        except OSError:
            continue
    return None


def ping_alive(ip):
    """اختبار وجود الجهاز عبر ping (قراءة فقط، دون أي اتصال آخر)."""
    out = run_cmd(["ping", "-n", "1", "-w", "700", ip], timeout=10)
    return bool(re.search(r"ttl\s*=", out, re.I))


def scan_host(ip):
    """فحص مضيف واحد: ping أولاً، ثم منافذ SMB كاكتشاف مكمّل."""
    alive = ping_alive(ip)
    port = probe_smb_ports(ip) if not alive else None
    if alive or port:
        return ip, alive, port
    return None


def scan_network():
    """بناء قائمة عناوين الشبكة الفرعية ومسحها بشكل متوازٍ."""
    print("[3.1] الأجهزة النشطة على الشبكة المحلية")
    ip, mask = get_local_network()
    if not ip:
        print("   ! تعذّر تحديد الشبكة المحلية (تحقق من واجهة الشبكة).")
        return []

    cidr = mask_to_cidr(mask)
    net = [int(o) for o in ip.split(".")]
    for i in range(4):
        net[i] &= int(mask.split(".")[i])

    limited = False
    if cidr < 24:
        # شبكة أوسع من /24: نقتصر على /24 التي تحتوي الجهاز الحالي
        # لتجنب مسح ضخم عبر /16 أو أكثر
        limited = True
        net[3] = 0
        cidr = 24
    host_bits = 32 - cidr

    print(f"   الشبكة: {'.'.join(map(str, net))} /{32 - host_bits}"
          + ("  (مُقيّد إلى 254 مضيفاً)" if limited else ""))
    print("   جارٍ المسح... قد يستغرق بضع ثوانٍ")

    own = ip
    targets = []
    for i in range(1, 2 ** host_bits - 1):  # استبعاد بث الشبكة (broadcast)
        addr = f"{net[0]}.{net[1]}.{net[2]}.{net[3] + i}"
        if addr != own:
            targets.append(addr)

    found = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=48) as pool:
        for result in pool.map(scan_host, targets):
            if result:
                found.append(result)

    def key(item):
        return tuple(int(o) for o in item[0].split("."))
    found.sort(key=key)

    if not found:
        print("   - لم يُرصد أي جهاز نشط (قد تحجب الشبكة استجابات ICMP).")
        return []

    for addr, alive, port in found:
        if alive:
            tail = f"  [يفتح المنفذ {port}]" if port else ""
            print(f"     * {addr}  (يستجيب لـ ping){tail}")
        else:
            print(f"     * {addr}  (لا يستجيب لـ ping لكنه يفتح المنفذ {port})")
    print(f"   + عدد الأجهزة النشطة المكتشفة: {len(found)}")
    return found


# ---------------------------------------------------------------- الفصل 4 --
# الإخفاء (محاكاة): كشف أحداث تنظيف/مسح سجلات الأحداث.
# ملاحظة تقنية: ويندوز لا يحتفظ بالأحداث المحذوفة نفسها، لكن عملية المسح
# تترك خلفها الحدث 104 (تم مسح سجل) والحدث 1102 (تنظيف سجل الأمان)،
# وهما المؤشر الذي ترصده فرق الدفاع.
# ----------------------------------------------------------------


def get_cleared_log_events():
    """جلب آخر أحداث 104/1102 من سجلات النظام مع التواريخ والأوصاف."""
    events = []
    for log in ("Security", "System", "Application"):
        script = (
            "Get-WinEvent -FilterHashtable @{{LogName='{log}'; Id=1102,104}} "
            "-MaxEvents 40 -ErrorAction SilentlyContinue | ForEach-Object {{ "
            "'{log}|' + $_.Id + '|' + "
            "$_.TimeCreated.ToString('yyyy-MM-dd HH:mm:ss') + '|' + "
            "($_.Message -replace '[\\r\\n]+', ' ') }}"
        ).format(log=log)
        out = run_cmd(["powershell", "-NoProfile", "-NonInteractive",
                       "-Command", script], timeout=60)
        for line in out.splitlines():
            parts = line.split("|", 3)
            if len(parts) == 4 and parts[1].isdigit():
                events.append({
                    "log": parts[0],
                    "id": int(parts[1]),
                    "time": parts[2],
                    "message": parts[3][:140],
                })

    events.sort(key=lambda e: e["time"], reverse=True)
    return events[:10]


def report_cleared_events():
    """عرض تحذيري لآخر 10 أحداث تنظيف سجلات الأحداث."""
    print("[4.1] أحداث مسح سجلات الأحداث (آخر 10)")
    events = get_cleared_log_events()
    if not events:
        print("   - لا توجد أحداث مسح حديثة في السجلات الثلاثة الرئيسية.")
        return 0

    print("   ! تحذير: عُثر على أحداث تنظيف سجلات -- مؤشر اختراق محتمل")
    print("     لاحظ أن الفاعل يمسح الآثار بهذه الطريقة بعد تنفيذ عمله.")
    for e in events:
        label = "نظّف سجل الأمان" if e["id"] == 1102 else "مسح سجل أحداث"
        print(f"     [{e['time']}] {label} (سجل {e['log']})")
        print(f"         {e['message'].strip()}")
    return len(events)


# ---------------------------------------------------------------- التقرير --


def main():
    if os.name != "nt":
        print("هذه الأداة مخصصة لنظام Windows (تعتمد على ipconfig/netsh/"
              "PowerShell و winreg) ولا يمكن تشغيلها على هذا النظام.")
        sys.exit(1)

    print(LINE)
    print("  APTHunter v1.0 -- مراقب سلوك APT (وضع المراقبة فقط)")
    print("  قراءة فقط: لا تعدّل النظام ولا تعطّل أي حماية")
    print(LINE)

    start = datetime.datetime.now()
    results = OrderedDict()

    banner("الفصل 1: حالة الحماية")
    results["الحماية"] = {
        "defender": check_defender(),
        "firewall": check_firewall(),
        "uac": check_uac(),
    }

    banner("الفصل 2: نقاط البدء التلقائي (الباب الخلفي - محاكاة)")
    results["RunKeys"] = read_run_keys()
    results["StartupFolders"] = list_startup_folders()
    results["ScheduledTasks"] = list_scheduled_tasks()

    banner("الفصل 3: انتشار الشبكة المحلية (محاكاة)")
    results["ActiveHosts"] = scan_network()

    banner("الفصل 4: إخفاء الآثار (محاكاة)")
    results["ClearedEvents"] = report_cleared_events()

    # ------------------------------------------------ الملخص النهائي
    banner("ملخص التقرير")
    prot = results["الحماية"]
    prot_line = (
        f"Defender: {prot['defender']} | الجدار: {prot['firewall']} | UAC: {prot['uac']}"
    )
    print("  " + prot_line)
    print(f"  مداخل البدء التلقائي: {results['RunKeys'] + results['StartupFolders']} "
          f"(سجل {results['RunKeys']} + مجلدات {results['StartupFolders']})")
    print(f"  مهام مجدولة نشطة: {results['ScheduledTasks']}")
    print(f"  أجهزة نشطة على الشبكة: {len(results['ActiveHosts'])}")
    print(f"  أحداث تنظيف السجلات: {results['ClearedEvents']}")

    elapsed = (datetime.datetime.now() - start).total_seconds()
    print(LINE)
    print(f"  انتهى الفحص خلال {elapsed:.1f} ثانية.")
    print("  لم تُجرَّ أي تعديلات على النظام -- الأداة تعمل بالقراءة فقط.")
    print(LINE)


if __name__ == "__main__":
    main()
