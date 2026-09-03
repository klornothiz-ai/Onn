#!/usr/bin/env python3
"""Generate the authoritative x86-64 SIMD instruction-form table (Intel syntax).

Every form below is assembled by GNU `as`; only encodings that assemble
cleanly enter the generated tables, so the interpreter's coverage list is
free of made-up encodings.

Outputs (checked in; the build itself stays dependency-free):
  tests/simd_native_forms.S   - one wrapper function per form (differential
                                ground truth executed natively by the host)
  tests/simd_forms_table.inc  - {name, bytes} table consumed by the
                                interpreter-side differential test
  tests/simd_form_counts.inc  - family counters for the coverage test
"""
import subprocess, os, sys, tempfile

OUT_DIR = os.path.join(os.path.dirname(__file__), "..", "tests")

X0, X1, X2, X3 = "xmm0", "xmm1", "xmm2", "xmm3"
Y0, Y1, Y2, Y3 = "ymm0", "ymm1", "ymm2", "ymm3"

F = []
def f(m, ops, imm, fam, w="128", label=None):
    F.append((label or m, m, tuple(ops), imm, fam, w))

# ---------------------------------------------------------------- float ALU
BIN_F = ["addps","addpd","subps","subpd","mulps","mulpd","divps","divpd",
         "minps","minpd","maxps","maxpd","andps","andpd","andnps","andnpd",
         "orps","orpd","xorps","xorpd","haddps","haddpd","hsubps","hsubpd",
         "addsubps","addsubpd"]
SCA_F = ["addss","addsd","subss","subsd","mulss","mulsd","divss","divsd",
         "minss","minsd","maxss","maxsd"]
UNA_F = ["sqrtps","sqrtpd","rsqrtps","rcpps"]
UNA_S = ["sqrtss","sqrtsd","rsqrtss","rcpss"]
for m in BIN_F:
    f(m, [X1, X2], 0, m, "legacy")
    f("v"+m, [X0, X1, X2], 0, m)
    f("v"+m, [Y0, Y1, Y2], 0, m, "256")
for m in SCA_F:
    f(m, [X1, X2], 0, m, "legacy")
    f("v"+m, [X0, X1, X2], 0, m)
for m in UNA_F:
    f(m, [X1, X2], 0, m, "legacy")
    f("v"+m, [X0, X1], 0, m)
    f("v"+m, [Y0, Y1], 0, m, "256")
for m in UNA_S:
    f(m, [X1, X2], 0, m, "legacy")
    f("v"+m, [X0, X1, X2], 0, m)

# --------------------------------------------------------------- conversions
# legacy: dst, src
f("cvtps2pd", [X0, X1], 0, "cvtps2pd", "legacy")
f("vcvtps2pd", [X0, X1], 0, "cvtps2pd")
f("vcvtps2pd", [Y0, X1], 0, "cvtps2pd", "256")
f("cvtpd2ps", [X0, X1], 0, "cvtpd2ps", "legacy")
f("vcvtpd2ps", [X0, X1], 0, "cvtpd2ps")
f("vcvtpd2ps", [X0, Y1], 0, "cvtpd2ps", "256")
f("cvtdq2ps", [X0, X1], 0, "cvtdq2ps", "legacy")
f("vcvtdq2ps", [X0, X1], 0, "cvtdq2ps")
f("vcvtdq2ps", [Y0, Y1], 0, "cvtdq2ps", "256")
f("cvtps2dq", [X0, X1], 0, "cvtps2dq", "legacy")
f("vcvtps2dq", [X0, X1], 0, "cvtps2dq")
f("vcvtps2dq", [Y0, Y1], 0, "cvtps2dq", "256")
f("cvttps2dq", [X0, X1], 0, "cvttps2dq", "legacy")
f("vcvttps2dq", [X0, X1], 0, "cvttps2dq")
f("vcvttps2dq", [Y0, Y1], 0, "cvttps2dq", "256")
f("cvtdq2pd", [X0, X1], 0, "cvtdq2pd", "legacy")
f("vcvtdq2pd", [X0, X1], 0, "cvtdq2pd")
f("vcvtdq2pd", [Y0, X1], 0, "cvtdq2pd", "256")
f("cvtpd2dq", [X0, X1], 0, "cvtpd2dq", "legacy")
f("vcvtpd2dq", [X0, X1], 0, "cvtpd2dq")
f("vcvtpd2dq", [X0, Y1], 0, "cvtpd2dq", "256")
f("cvttpd2dq", [X0, X1], 0, "cvttpd2dq", "legacy")
f("vcvttpd2dq", [X0, X1], 0, "cvttpd2dq")
f("vcvttpd2dq", [X0, Y1], 0, "cvttpd2dq", "256")
f("cvtss2sd", [X0, X1], 0, "cvtss2sd", "legacy")
f("vcvtss2sd", [X0, X1, X2], 0, "cvtss2sd")
f("cvtsd2ss", [X0, X1], 0, "cvtsd2ss", "legacy")
f("vcvtsd2ss", [X0, X1, X2], 0, "cvtsd2ss")
f("cvtsi2ss", [X0, "ecx"], 0, "cvtsi2ss", "legacy")
f("cvtsi2ss", [X0, "rcx"], 0, "cvtsi2ss", "legacy")
f("vcvtsi2ss", [X0, X1, "ecx"], 0, "cvtsi2ss")
f("vcvtsi2ss", [X0, X1, "rcx"], 0, "cvtsi2ss")
f("cvtsi2sd", [X0, "ecx"], 0, "cvtsi2sd", "legacy")
f("cvtsi2sd", [X0, "rcx"], 0, "cvtsi2sd", "legacy")
f("vcvtsi2sd", [X0, X1, "ecx"], 0, "cvtsi2sd")
f("vcvtsi2sd", [X0, X1, "rcx"], 0, "cvtsi2sd")
for m in ["cvtss2si", "cvttsd2si", "cvtsd2si", "cvttss2si"]:
    f(m, ["ecx", X1], 0, m, "legacy"); f("v"+m, ["ecx", X1], 0, m)
    f(m, ["rcx", X1], 0, m, "legacy"); f("v"+m, ["rcx", X1], 0, m)

# ------------------------------------------------------------------- moves
for m in ["movaps","movapd","movups","movupd"]:
    f(m, [X0, X1], 0, m, "legacy")
    f("v"+m, [X0, X1], 0, m); f("v"+m, [Y0, Y1], 0, m, "256")
f("movss", [X0, X1], 0, "movss", "legacy"); f("vmovss", [X0, X1, X2], 0, "movss")
f("movsd", [X0, X1], 0, "movsd", "legacy"); f("vmovsd", [X0, X1, X2], 0, "movsd")
f("movlhps", [X0, X2], 0, "movlhps", "legacy"); f("vmovlhps", [X0, X1, X2], 0, "movlhps")
f("movhlps", [X0, X2], 0, "movhlps", "legacy"); f("vmovhlps", [X0, X1, X2], 0, "movhlps")
f("movsldup", [X0, X1], 0, "movsldup", "legacy"); f("vmovsldup", [X0, X1], 0, "movsldup"); f("vmovsldup", [Y0, Y1], 0, "movsldup", "256")
f("movshdup", [X0, X1], 0, "movshdup", "legacy"); f("vmovshdup", [X0, X1], 0, "movshdup"); f("vmovshdup", [Y0, Y1], 0, "movshdup", "256")
f("movddup", [X0, X1], 0, "movddup", "legacy"); f("vmovddup", [X0, X1], 0, "movddup"); f("vmovddup", [Y0, Y1], 0, "movddup", "256")
f("movdqa", [X0, X1], 0, "movdqa", "legacy"); f("vmovdqa", [X0, X1], 0, "movdqa"); f("vmovdqa", [Y0, Y1], 0, "movdqa", "256")
f("movdqu", [X0, X1], 0, "movdqu", "legacy"); f("vmovdqu", [X0, X1], 0, "movdqu"); f("vmovdqu", [Y0, Y1], 0, "movdqu", "256")
f("movd", [X0, "ecx"], 0, "movd", "legacy"); f("vmovd", [X0, "ecx"], 0, "movd")
f("movq", [X0, "rcx"], 0, "movq", "legacy"); f("vmovq", [X0, "rcx"], 0, "movq")
f("vmovd", ["ecx", X1], 0, "movd_to_gpr"); f("vmovq", ["rcx", X1], 0, "movq_to_gpr")
f("movq", [X0, X1], 0, "movq_xmm", "legacy")

# ------------------------------------------------------------- compare/mask
for m in ["cmpps","cmppd","shufps","shufpd","dpps"]:
    f(m, [X1, X2, 0x4B], 1, m, "legacy")
    f("v"+m, [X0, X1, X2, 0x4B], 1, m)
    if m != "dppd":
        f("v"+m, [Y0, Y1, Y2, 0x4B], 1, m, "256")
f("dppd", [X1, X2, 0x3], 1, "dppd", "legacy"); f("vdppd", [X0, X1, X2, 0x3], 1, "dppd")
for m in ["cmpss","cmpsd"]:
    f(m, [X1, X2, 0x4B], 1, m, "legacy"); f("v"+m, [X0, X1, X2, 0x4B], 1, m)
for m in ["roundps","roundpd"]:
    f(m, [X1, X2, 0x4B], 1, m, "legacy")
    f("v"+m, [X0, X1, 0x4B], 1, m); f("v"+m, [Y0, Y1, 0x4B], 1, m, "256")
for m in ["roundss","roundsd"]:
    f(m, [X1, X2, 0x4B], 1, m, "legacy"); f("v"+m, [X0, X1, X2, 0x4B], 1, m)
f("vtestps", [X1, X2], 0, "vtestps"); f("vtestps", [Y1, Y2], 0, "vtestps", "256")
f("vtestpd", [X1, X2], 0, "vtestpd"); f("vtestpd", [Y1, Y2], 0, "vtestpd", "256")
f("ptest", [X1, X2], 0, "ptest", "legacy"); f("vptest", [X1, X2], 0, "ptest"); f("vptest", [Y1, Y2], 0, "ptest", "256")
for m in ["comiss","ucomiss","comisd","ucomisd"]:
    f(m, [X1, X2], 0, m, "legacy"); f("v"+m, [X1, X2], 0, m)
for m in ["movmskps","movmskpd"]:
    f(m, ["ecx", X1], 0, m, "legacy"); f("v"+m, ["ecx", X1], 0, m); f("v"+m, ["ecx", Y1], 0, m, "256")
f("pmovmskb", ["ecx", X1], 0, "pmovmskb", "legacy"); f("vpmovmskb", ["ecx", X1], 0, "pmovmskb"); f("vpmovmskb", ["ecx", Y1], 0, "pmovmskb", "256")

# ------------------------------------------------------- integer ALU packs
INT2 = ["paddb","paddw","paddd","paddq","psubb","psubw","psubd","psubq",
        "paddsb","paddsw","psubsb","psubsw","paddusb","paddusw",
        "psubusb","psubusw","pand","por","pxor","pandn",
        "pcmpeqb","pcmpeqw","pcmpeqd","pcmpgtb","pcmpgtw","pcmpgtd",
        "pminub","pmaxub","pminsw","pmaxsw","pavgb","pavgw",
        "pmullw","pmulhuw","pmulhw","pmuludq","pmaddwd","psadbw",
        "punpcklbw","punpcklwd","punpckldq","punpcklqdq",
        "punpckhbw","punpckhwd","punpckhdq","punpckhqdq",
        "packsswb","packuswb","packssdw","pcmpeqq","pcmpgtq",
        "pminsb","pminsd","pminuw","pminud","pmaxsb","pmaxsd","pmaxuw","pmaxud",
        "pmuldq","pmulld","phaddw","phaddd","phaddsw","phsubw","phsubd",
        "phsubsw","pmaddubsw","pmulhrsw","psignb","psignw","psignd",
        "pshufb","packusdw"]
IMM2 = {"pblendw": 1, "palignr": 1}
for m in INT2:
    f(m, [X1, X2], 0, m, "legacy")
    f("v"+m, [X0, X1, X2], 0, m)
    f("v"+m, [Y0, Y1, Y2], 0, m, "256")
for m, has_imm in IMM2.items():
    f(m, [X1, X2, 3], 1, m, "legacy")
    f("v"+m, [X0, X1, X2, 3], 1, m)
    f("v"+m, [Y0, Y1, Y2, 3], 1, m, "256")

for m in ["pabsb","pabsw","pabsd"]:
    f(m, [X0, X1], 0, m, "legacy")
    f("v"+m, [X0, X1], 0, m); f("v"+m, [Y0, Y1], 0, m, "256")
for m in ["pshufd","pshufhw","pshuflw"]:
    f(m, [X0, X1, 0x1B], 1, m, "legacy")
    f("v"+m, [X0, X1, 0x1B], 1, m); f("v"+m, [Y0, Y1, 0x1B], 1, m, "256")
for m in ["vpermilps","vpermilpd"]:
    f(m, [X0, X1, 0x1B], 1, m); f(m, [Y0, Y1, 0x1B], 1, m, "256")
    f(m, [X0, X1, X2], 0, m); f(m, [Y0, Y1, Y2], 0, m, "256")
f("vperm2f128", [Y0, Y1, Y2, 0x31], 1, "vperm2f128")
f("vperm2i128", [Y0, Y1, Y2, 0x31], 1, "vperm2i128")
f("vpermq", [Y0, Y1, 0x1B], 1, "vpermq")
f("vpermpd", [Y0, Y1, 0x1B], 1, "vpermpd")
f("vpermd", [Y0, Y1, Y2], 0, "vpermd")
f("vpermps", [Y0, Y1, Y2], 0, "vpermps")

# variable shifts (count in xmm)
for m in ["psllw","pslld","psllq","psrlw","psrld","psrlq","psraw","psrad"]:
    f(m, [X1, X2], 0, "vshift", "legacy")
    f("v"+m, [X0, X1, X2], 0, "vshift")
    f("v"+m, [Y0, Y1, X2], 0, "vshift", "256")   # count reads low 64 bits
for m in ["vpsllvd","vpsrlvd","vpsravd"]:
    f(m, [X0, X1, X2], 0, m); f(m, [Y0, Y1, Y2], 0, m, "256")
f("vpsllvq", [X0, X1, X2], 0, "vpsllvq"); f("vpsllvq", [Y0, Y1, Y2], 0, "vpsllvq", "256")
f("vpsrlvq", [X0, X1, X2], 0, "vpsrlvq"); f("vpsrlvq", [Y0, Y1, Y2], 0, "vpsrlvq", "256")

# blends / broadcast / insert-extract
f("blendps", [X1, X2, 0xB], 1, "blendps", "legacy"); f("vblendps", [X0, X1, X2, 0xB], 1, "blendps"); f("vblendps", [Y0, Y1, Y2, 0xB], 1, "blendps", "256")
f("blendpd", [X1, X2, 0x3], 1, "blendpd", "legacy"); f("vblendpd", [X0, X1, X2, 0x3], 1, "blendpd"); f("vblendpd", [Y0, Y1, Y2, 0x3], 1, "blendpd", "256")
f("pblendvb", [X1, X2], 0, "pblendvb", "legacy")
f("vpblendvb", [X0, X1, X2, X3], 0, "vpblendvb"); f("vpblendvb", [Y0, Y1, Y2, Y3], 0, "vpblendvb", "256")
f("blendvps", [X1, X2], 0, "blendvps", "legacy"); f("vblendvps", [X0, X1, X2, X3], 0, "vblendvps"); f("vblendvps", [Y0, Y1, Y2, Y3], 0, "vblendvps", "256")
f("blendvpd", [X1, X2], 0, "blendvpd", "legacy"); f("vblendvpd", [X0, X1, X2, X3], 0, "vblendvpd"); f("vblendvpd", [Y0, Y1, Y2, Y3], 0, "vblendvpd", "256")
f("vbroadcastss", [X0, X1], 0, "vbroadcastss"); f("vbroadcastss", [Y0, X1], 0, "vbroadcastss", "256")
f("vbroadcastsd", [Y0, X1], 0, "vbroadcastsd", "256")
f("vbroadcastf128", [Y0, "[288+rdi]"], 0, "vbroadcastf128", "256")
f("vinsertf128", [Y0, Y1, X2, 1], 1, "vinsertf128")
f("vextractf128", [X0, Y1, 1], 1, "vextractf128")
f("vinserti128", [Y0, Y1, X2, 1], 1, "vinserti128")
f("vextracti128", [X0, Y1, 1], 1, "vextracti128")
for m in ["vpbroadcastb","vpbroadcastw","vpbroadcastd","vpbroadcastq"]:
    f(m, [X0, X1], 0, m); f(m, [Y0, X1], 0, m, "256")

# pinsr / pextr (GPR<->XMM)
for m, g in [("pinsrb","ecx"),("pinsrw","ecx"),("pinsrd","ecx"),("pinsrq","rcx")]:
    f(m, [X1, g, 2], 1, m, "legacy")
    f("v"+m, [X0, X1, g, 2], 1, m)
for m, g in [("pextrb","ecx"),("pextrw","ecx"),("pextrd","ecx"),("pextrq","rcx")]:
    f(m, [g, X1, 2], 1, m, "legacy")
    f("v"+m, [g, X1, 2], 1, m)
f("extractps", ["ecx", X1, 2], 1, "extractps", "legacy"); f("vextractps", ["ecx", X1, 2], 1, "extractps")
f("insertps", [X1, X2, 0x41], 1, "insertps", "legacy"); f("vinsertps", [X0, X1, X2, 0x41], 1, "insertps")
for m in ["pmovsxbw","pmovsxbd","pmovsxbq","pmovsxwd","pmovsxwq","pmovsxdq",
          "pmovzxbw","pmovzxbd","pmovzxbq","pmovzxwd","pmovzxwq","pmovzxdq"]:
    f(m, [X0, X1], 0, m)
    f("v"+m, [X0, X1], 0, m); f("v"+m, [Y0, X1], 0, m, "256")
f("mpsadbw", [X1, X2, 2], 1, "mpsadbw", "legacy"); f("vmpsadbw", [X0, X1, X2, 2], 1, "mpsadbw"); f("vmpsadbw", [Y0, Y1, Y2, 2], 1, "mpsadbw", "256")
f("phminposuw", [X0, X1], 0, "phminposuw", "legacy"); f("vphminposuw", [X0, X1], 0, "phminposuw")
f("movntdqa", [X1, "[288+rdi]"], 0, "movntdqa", "legacy"); f("vmovntdqa", [X1, "[288+rdi]"], 0, "movntdqa"); f("vmovntdqa", [Y1, "[288+rdi]"], 0, "movntdqa", "256")

# shifts by immediate
for m in ["psllw","pslld","psllq","psrlw","psrld","psrlq","psraw","psrad","pslldq","psrldq"]:
    f(m, [X1, 3], 1, "shift_imm", "legacy")
    f("v"+m, [X0, X1, 3], 1, "shift_imm")
    f("v"+m, [Y0, Y1, 3], 1, "shift_imm", "256")

# FMA family (VEX only)
FMA = []
for pre in ["132","213","231"]:
    for t in ["ps","pd","ss","sd"]:
        FMA += [f"vfmadd{pre}{t}", f"vfnmadd{pre}{t}", f"vfmsub{pre}{t}", f"vfnmsub{pre}{t}"]
    FMA += [f"vfmaddsub{pre}ps", f"vfmaddsub{pre}pd", f"vfmsubadd{pre}ps", f"vfmsubadd{pre}pd"]
for m in FMA:
    if m.endswith("ss") or m.endswith("sd"):
        f(m, [X0, X1, X2], 0, m)
    else:
        f(m, [X0, X1, X2], 0, m); f(m, [Y0, Y1, Y2], 0, m, "256")

# AES + PCLMUL + CRC32 + F16C
for m in ["aesenc","aesenclast","aesdec","aesdeclast"]:
    f(m, [X0, X1], 0, "aes", "legacy"); f("v"+m, [X0, X1, X2], 0, "aes")
f("aesimc", [X0, X1], 0, "aes", "legacy"); f("vaesimc", [X0, X1], 0, "aes")
f("aeskeygenassist", [X0, X1, 3], 1, "aes", "legacy"); f("vaeskeygenassist", [X0, X1, 3], 1, "aes")
f("pclmulqdq", [X1, X2, 0x11], 1, "pclmulqdq", "legacy"); f("vpclmulqdq", [X0, X1, X2, 0x11], 1, "pclmulqdq"); f("vpclmulqdq", [Y0, Y1, Y2, 0x11], 1, "pclmulqdq", "256")
f("crc32b", ["ecx", "cl"], 0, "crc32", "legacy")
f("crc32w", ["ecx", "cx"], 0, "crc32", "legacy")
f("crc32", ["ecx", "ecx"], 0, "crc32", "legacy")
f("crc32q", ["rcx", "rcx"], 0, "crc32", "legacy")
f("vcvtph2ps", [X0, X1], 0, "f16c"); f("vcvtph2ps", [Y0, X1], 0, "f16c", "256")
f("vcvtps2ph", [X0, X1, 4], 1, "f16c"); f("vcvtps2ph", [X0, Y1, 4], 1, "f16c", "256")

# vmaskmov / vpmaskmov (memory-source forms; mask in vvvv)
f("vmaskmovps", [X0, X1, "[288+rdi]"], 0, "vmaskmov"); f("vmaskmovpd", [X0, X1, "[288+rdi]"], 0, "vmaskmov")
f("vmaskmovps", [Y0, Y1, "[288+rdi]"], 0, "vmaskmov", "256"); f("vmaskmovpd", [Y0, Y1, "[288+rdi]"], 0, "vmaskmov", "256")
f("vpmaskmovd", [X0, X1, "[288+rdi]"], 0, "vmaskmov"); f("vpmaskmovq", [X0, X1, "[288+rdi]"], 0, "vmaskmov")
f("vpmaskmovd", [Y0, Y1, "[288+rdi]"], 0, "vmaskmov", "256"); f("vpmaskmovq", [Y0, Y1, "[288+rdi]"], 0, "vmaskmov", "256")

# NOTE: SSE4.2 string compares (PCMPESTRI/PCMPISTRI/PCMPESTRM/PCMPISTRM) are
# intentionally NOT in the differentially-verified table: their flag/index
# semantics are microcode-defined and need a dedicated reverse-engineering
# pass. The engine keeps a best-effort software implementation (map3).

# full vcmp predicate table (0..31) - the predicate decode is where bugs hide
for pred in range(32):
    for t in ["ps","pd","ss","sd"]:
        f(f"vcmp{t}", [X0, X1, X2, pred], 1, f"vcmp{t}", "128", f"vcmp{t}_p{pred}")
    if pred < 16:  # predicates above 15 duplicate; still distinct encodings
        pass
for pred in range(32):
    f("vcmpps", [Y0, Y1, Y2, pred], 1, "vcmpps256", "256", f"vcmpps256_p{pred}")
    f("vcmppd", [Y0, Y1, Y2, pred], 1, "vcmppd256", "256", f"vcmppd256_p{pred}")

# rounding-mode immediates for the round family
for imm in [0, 1, 2, 3, 8, 9, 10, 11]:
    f("roundps", [X1, X2, imm], 1, "roundps_imm", "legacy", f"roundps_i{imm}")
    f("vroundps", [Y0, Y1, imm], 1, "roundps_imm", "128", f"vroundps_i{imm}")
    f("roundss", [X1, X2, imm], 1, "roundss_imm", "legacy", f"roundss_i{imm}")

# representative MEMORY-OPERAND forms (exercise ModRM + bus load/store paths)
f("movups", [X0, "[288+rdi]"], 0, "mem_forms", "legacy", "movups_m")
f("vmovups", [X0, "[288+rdi]"], 0, "mem_forms", "128", "vmovups_m")
f("vmovups", [Y0, "[288+rdi]"], 0, "mem_forms", "256", "vmovups_m256")
f("vmovups", ["[288+rsi]", Y1], 0, "mem_forms", "256", "vmovups_store")
f("vmovdqu", ["[288+rsi]", X1], 0, "mem_forms", "128", "vmovdqu_store")
f("vmovss", [X0, "dword ptr [288+rdi]"], 0, "mem_forms", "128", "vmovss_m")
f("vmovss", ["dword ptr [288+rsi]", X1], 0, "mem_forms", "128", "vmovss_store")
f("vmovsd", [X0, "qword ptr [288+rdi]"], 0, "mem_forms", "128", "vmovsd_m")
f("vmovsd", ["qword ptr [288+rsi]", X1], 0, "mem_forms", "128", "vmovsd_store")
f("vmovhps", [X0, X1, "[288+rdi]"], 0, "mem_forms", "128", "vmovhps_m")
f("vmovlps", [X0, X1, "[288+rdi]"], 0, "mem_forms", "128", "vmovlps_m")
f("vmovhps", ["[288+rsi]", X1], 0, "mem_forms", "128", "vmovhps_store")
f("vaddps", [Y0, Y1, "[288+rdi]"], 0, "mem_forms", "256", "vaddps_m")
f("vaddpd", [X0, X1, "[288+rdi]"], 0, "mem_forms", "128", "vaddpd_m")
f("vpaddb", [Y0, Y1, "[288+rdi]"], 0, "mem_forms", "256", "vpaddb_m")
f("vsubss", [X0, X1, "dword ptr [288+rdi]"], 0, "mem_forms", "128", "vsubss_m")
f("vmulps", [X0, X1, "[288+rdi]"], 0, "mem_forms", "128", "vmulps_m")
f("vdivsd", [X0, X1, "qword ptr [288+rdi]"], 0, "mem_forms", "128", "vdivsd_m")
f("vpshufb", [Y0, Y1, "[288+rdi]"], 0, "mem_forms", "256", "vpshufb_m")
f("vpunpcklqdq", [X0, X1, "[288+rdi]"], 0, "mem_forms", "128", "vpunpcklqdq_m")
f("vpcmpeqb", [Y0, Y1, "[288+rdi]"], 0, "mem_forms", "256", "vpcmpeqb_m")
f("vminps", [Y0, Y1, "[288+rdi]"], 0, "mem_forms", "256", "vminps_m")
f("vmaxpd", [X0, X1, "[288+rdi]"], 0, "mem_forms", "128", "vmaxpd_m")
f("vfmadd231ps", [Y0, Y1, "[288+rdi]"], 0, "mem_forms", "256", "vfmadd231ps_m")
f("vfmadd132sd", [X0, X1, "qword ptr [288+rdi]"], 0, "mem_forms", "128", "vfmadd132sd_m")
f("vcmpps", [X0, X1, "[288+rdi]", 6], 1, "mem_forms", "128", "vcmpps_m")
f("vshufps", [Y0, Y1, "[288+rdi]", 27], 1, "mem_forms", "256", "vshufps_m")
f("vblendps", [Y0, Y1, "[288+rdi]", 11], 1, "mem_forms", "256", "vblendps_m")
f("vpalignr", [X0, X1, "[288+rdi]", 3], 1, "mem_forms", "128", "vpalignr_m")
f("vroundps", [Y0, "[288+rdi]", 4], 1, "mem_forms", "256", "vroundps_m")
f("vpermilps", [Y0, "[288+rdi]", 27], 1, "mem_forms", "256", "vpermilps_m")
f("vptest", [Y1, "[288+rdi]"], 0, "mem_forms", "256", "vptest_m")
f("vucomiss", [X1, "dword ptr [288+rdi]"], 0, "mem_forms", "128", "vucomiss_m")
f("vcvtsi2sd", [X0, X1, "dword ptr [288+rdi]"], 0, "mem_forms", "128", "vcvtsi2sd_m32")
f("vcvtsi2ss", [X0, X1, "qword ptr [288+rdi]"], 0, "mem_forms", "128", "vcvtsi2ss_m64")
f("vcvttps2dq", [Y0, "[288+rdi]"], 0, "mem_forms", "256", "vcvttps2dq_m")
f("vcvtps2pd", [Y0, "[288+rdi]"], 0, "mem_forms", "256", "vcvtps2pd_m")
f("vmovd", [X0, "dword ptr [288+rdi]"], 0, "mem_forms", "128", "vmovd_m")
f("vmovq", [X0, "qword ptr [288+rdi]"], 0, "mem_forms", "128", "vmovq_m")
f("vmovd", ["dword ptr [288+rsi]", X1], 0, "mem_forms", "128", "vmovd_store")
f("vmovq", ["qword ptr [288+rsi]", X1], 0, "mem_forms", "128", "vmovq_store")
f("vpinsrb", [X0, X1, "byte ptr [288+rdi]", 2], 1, "mem_forms", "128", "vpinsrb_m")
f("vpinsrw", [X0, X1, "word ptr [288+rdi]", 2], 1, "mem_forms", "128", "vpinsrw_m")
f("vpinsrd", [X0, X1, "dword ptr [288+rdi]", 2], 1, "mem_forms", "128", "vpinsrd_m")
f("vpinsrq", [X0, X1, "qword ptr [288+rdi]", 2], 1, "mem_forms", "128", "vpinsrq_m")
f("vpextrb", ["byte ptr [288+rsi]", X1, 2], 1, "mem_forms", "128", "vpextrb_m")
f("vpextrw", ["word ptr [288+rsi]", X1, 2], 1, "mem_forms", "128", "vpextrw_m")
f("vpextrd", ["dword ptr [288+rsi]", X1, 2], 1, "mem_forms", "128", "vpextrd_m")
f("vpextrq", ["qword ptr [288+rsi]", X1, 2], 1, "mem_forms", "128", "vpextrq_m")
f("vextractps", ["dword ptr [288+rsi]", X1, 2], 1, "mem_forms", "128", "vextractps_m")
f("vinsertf128", [Y0, Y1, "[288+rdi]", 1], 1, "mem_forms", "128", "vinsertf128_m")
f("vextractf128", ["[288+rsi]", Y1, 1], 1, "mem_forms", "128", "vextractf128_m")
f("vpbroadcastb", [Y0, "byte ptr [288+rdi]"], 0, "mem_forms", "256", "vpbroadcastb_m")
f("vpbroadcastw", [X0, "word ptr [288+rdi]"], 0, "mem_forms", "128", "vpbroadcastw_m")
f("vpbroadcastd", [Y0, "dword ptr [288+rdi]"], 0, "mem_forms", "256", "vpbroadcastd_m")
f("vpbroadcastq", [X0, "qword ptr [288+rdi]"], 0, "mem_forms", "128", "vpbroadcastq_m")
f("vbroadcastss", [Y0, "dword ptr [288+rdi]"], 0, "mem_forms", "256", "vbroadcastss_m")
f("vbroadcastsd", [Y0, "qword ptr [288+rdi]"], 0, "mem_forms", "256", "vbroadcastsd_m")
f("vpmovsxbw", [Y0, "[288+rdi]"], 0, "mem_forms", "256", "vpmovsxbw_m")
f("vpmovzxbd", [X0, "[288+rdi]"], 0, "mem_forms", "128", "vpmovzxbd_m")
f("vpmovsxdq", [Y0, "[288+rdi]"], 0, "mem_forms", "256", "vpmovsxdq_m")
f("vpsllw", [X0, X1, "[288+rdi]"], 0, "mem_forms", "128", "vpsllw_m")
f("vaesenc", [X0, X1, "[288+rdi]"], 0, "mem_forms", "128", "vaesenc_m")
f("vpclmulqdq", [X0, X1, "[288+rdi]", 17], 1, "mem_forms", "128", "vpclmulqdq_m")
f("vpmaskmovd", [Y0, Y1, "[288+rdi]"], 0, "mem_forms", "256", "vpmaskmovd_m")
f("vmaskmovps", ["[288+rsi]", Y1, Y0], 0, "mem_forms", "256", "vmaskmovps_store")
f("vmaskmovpd", ["[288+rsi]", X1, X0], 0, "mem_forms", "128", "vmaskmovpd_store")
f("vpmaskmovd", ["[288+rsi]", Y1, Y0], 0, "mem_forms", "256", "vpmaskmovd_store")
f("vcvtph2ps", [X0, "[288+rdi]"], 0, "mem_forms", "128", "vcvtph2ps_m")
f("vcvtps2ph", ["[288+rsi]", X1, 4], 1, "mem_forms", "128", "vcvtps2ph_m")
f("vmovntdq", ["[288+rsi]", Y1], 0, "mem_forms", "256", "vmovntdq_store")
f("vmovntdqa", [Y1, "[288+rdi]"], 0, "mem_forms", "256", "vmovntdqa_m")

# ---------------------------------------------------------------- generate
lines = [".intel_syntax noprefix", ".text", ".align 64"]
entries = []
for i, (name, m, ops, imm, fam, w) in enumerate(F):
    label = f"form_{i}"
    lines.append(f".globl\t{label}")
    lines.append(f"{label}:")
    lines.append("\tpush\tr15")                       # r15 is callee-saved
    # inputs: ymm0..3 from [rdi+0..127], rcx/rax/rdx from [rdi+256/264/272].
    # NOTE: no vzeroupper -- the upper YMM halves are part of the input state.
    for r in range(4):
        lines.append(f"\tvmovdqu\tymm{r}, [{r*32}+rdi]")
    lines.append("\tmov\trcx, [256+rdi]")
    lines.append("\tmov\trax, [264+rdi]")
    lines.append("\tmov\trdx, [272+rdi]")
    # zero the arithmetic flags so flag-neutral instructions compare equal
    # (push 0 / popfq clears CF/PF/AF/ZF/SF/OF without setting any of them)
    lines.append("\tpush\t0")
    lines.append("\tpopfq")
    parts = [str(o) for o in ops]
    lines.append("\t" + m + " " + ", ".join(parts))
    lines.append("\tpushfq")
    lines.append("\tpop\tr15")
    lines.append("\tand\tr15d, 0x8D5")
    for r in range(4):
        lines.append(f"\tvmovdqu\t[{r*32}+rsi], ymm{r}")
    lines.append("\tmov\t[256+rsi], rcx")
    lines.append("\tmov\t[264+rsi], rax")
    lines.append("\tmov\t[272+rsi], rdx")
    lines.append("\tmov\t[280+rsi], r15")
    lines.append("\tpop\tr15")
    lines.append("\tret")
    entries.append((label, name, m, ops, imm, fam, w))

asm_path = os.path.join(OUT_DIR, "simd_native_forms.S")
with open(asm_path, "w") as fh:
    fh.write("// GENERATED by scripts/gen_simd_forms.py - do not edit.\n")
    fh.write("// Differential ground truth: each wrapper executes one SIMD form\n")
    fh.write("// natively (inputs from rdi, outputs+flags to rsi).\n\n")
    fh.write("\n".join(lines) + "\n")

obj = os.path.join(tempfile.mkdtemp(), "forms.o")
r = subprocess.run(["as", "-o", obj, asm_path], capture_output=True, text=True)
if r.returncode != 0:
    errs = [l for l in r.stderr.splitlines() if "Error" in l]
    print(f"AS FAILED ({len(errs)} errors):"); print("\n".join(errs[:25])); sys.exit(1)
r = subprocess.run(["objdump", "-d", "-M", "intel", obj], capture_output=True, text=True)

cur = None; captured = {}
for ln in r.stdout.splitlines():
    if "<form_" in ln and ":" in ln and ln.strip().endswith(">:"):
        cur = ln.split("<")[1].split(">")[0]
        captured[cur] = []
        continue
    if cur and "\t" in ln and ":" in ln:
        parts = ln.split("\t", 2)
        if len(parts) == 2 and captured[cur]:
            # objdump wraps instructions longer than 7 bytes: the
            # continuation line carries more bytes and no mnemonic.
            prev_b, prev_mn = captured[cur][-1]
            captured[cur][-1] = (prev_b + parts[1].split(), prev_mn)
        elif len(parts) == 3:
            bytes_col = parts[1].strip().split()
            mn = parts[2].strip()
            if mn == "" and captured[cur]:
                prev_b, prev_mn = captured[cur][-1]
                captured[cur][-1] = (prev_b + bytes_col, prev_mn)
            else:
                captured[cur].append((bytes_col, mn))

inc = ["// GENERATED by scripts/gen_simd_forms.py - do not edit.",
       "// {mnemonic, family, bytes} each verified by GNU as round trip.",
       "#pragma once",
       "#include <cstdint>",
       "namespace SimdForms {",
       "struct Form { const char* name; const char* family; const uint8_t* bytes; int nbytes; };"]

byte_arrays, table = [], []
for i, (label, name, m, ops, imm, fam, w) in enumerate(entries):
    cap = captured.get(label, [])
    idx = 0
    for j, (b, mn) in enumerate(cap):
        if mn.startswith("mov") and "rdx" in mn and j > 4:
            idx = j + 1
            break
    # skip the flag-normalizing push/popfq (or xor) prelude lines
    while idx < len(cap):
        mn = cap[idx][1]
        if (mn.startswith("xor") and "r15" in mn) or mn.startswith("push") or mn.startswith("popf") or (mn.startswith("and") and "r15" in mn):
            idx += 1
        else:
            break
    if idx >= len(cap):
        print(f"MISSING instr for {label} ({m})"); sys.exit(1)
    b, mn = cap[idx]
    try:
        bs = [int(x, 16) for x in b]
    except ValueError:
        print(f"BAD BYTES {label}: {b}"); sys.exit(1)
    if not bs:
        print(f"EMPTY {label}"); sys.exit(1)
    arr = ", ".join(f"0x{x:02X}" for x in bs)
    byte_arrays.append(f"static const uint8_t b_{i}[] = {{{arr}}};")
    table.append(f"{{\"{m}\", \"{fam}\", b_{i}, {len(bs)}}},")

inc += byte_arrays
inc.append("static const Form kForms[] = {")
inc += table
inc.append("};")
inc.append(f"static const int kFormCount = {len(table)};")
inc.append("} // namespace SimdForms")
with open(os.path.join(OUT_DIR, "simd_forms_table.inc"), "w") as fh:
    fh.write("\n".join(inc) + "\n")

fams = {}
for (_, _, _, _, fam, w) in F:
    fams[fam] = fams.get(fam, 0) + 1
cnt = ["// GENERATED - form counts per family", "#pragma once",
       "namespace SimdForms {"]
for k in sorted(fams):
    cnt.append(f"constexpr int k_{k} = {fams[k]};")
cnt.append(f"constexpr int k_total = {len(F)};")
cnt.append("} // namespace SimdForms")

# thunk table: C array of function pointers form_0..form_N
thunk = ["// GENERATED - do not edit.", "#pragma once",
         "typedef void (*FormFn)(const void*, void*);",
         'extern "C" {']
thunk.extend(f"void form_{i}(const void*, void*);" for i in range(len(F)))
thunk.append("}")
thunk.append(f"static FormFn SimdFormThunks[{len(F)}] = {{")
thunk.extend(f"    form_{i}," for i in range(len(F)))
thunk.append("};")
with open(os.path.join(OUT_DIR, "simd_form_thunk.inc"), "w") as fh:
    fh.write("\n".join(thunk) + "\n")
with open(os.path.join(OUT_DIR, "simd_form_counts.inc"), "w") as fh:
    fh.write("\n".join(cnt) + "\n")

print(f"OK: {len(F)} forms across {len(fams)} families")
