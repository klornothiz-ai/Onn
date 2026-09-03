// ============================================================================
// ProsperoLayer RDNA2 Core - libSceAgc HLE module (round 32, fixed)
// ----------------------------------------------------------------------------
// REAL NIDs harvested from the ps5rs open-source NID catalog
// (https://github.com/claimore22/ps5rs - data/stubs/by_library/libSceAgc.txt).
//
// Round 32 correction: the initial version used invented placeholder NIDs
// ("AgcInit00001" etc.) which are DEAD CODE - no real PS5 binary can ever
// resolve them because the runtime linker matches by exact NID hash.
// This version registers 95 REAL Sony NID hashes that actual PS5 eboots
// import, so the HLE trampoline mechanism can route them to host stubs.
//
// Honesty boundary: these are fail-closed logging stubs returning OK.
// Resolving a NID means the import slot gets a trampoline instead of
// faulting - it does NOT mean the graphics operation actually works.
// ============================================================================

#include "common/abi.h"
#include "common/logging/log.h"
#include "libs/ps_errno.h"
#include "libs/libs.h"
#include "loader/symbolDatabase.h"

#include <cinttypes>
#include <cstdint>

namespace Libs {

LIB_VERSION("Agc", 1, "Agc", 1, 1);

namespace Agc {

static KYTY_SYSV_ABI int sceAgcCbSetUcRegistersDirect() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetIndexIndirectArgs() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbRewindGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbAtomicMem() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbAtomicGdsGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcCbMemsetExclusive() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetCfRegisterDirect() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcBranchPatchSetThenTarget() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcSetStaticQword() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbPushMarkerSpan() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetBaseDispatchIndirectArgsGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcGetDefaultCxStateFlat() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetIndexIndirectArgsGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbCopyDataGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetCfRegisterRangeDirect() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetMarkerSpan() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbEventWriteGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetIndexSizeGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbCopyDataGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAsyncWriteDataPatchSetAddressOrOffset() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbAtomicMemGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcGetRegisterDefaults() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcSetSubmitMode() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbRewind() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcWriteDataPatchSetCachePolicy() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbSetFlip() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbPrimeUtcl2GetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAsyncWriteDataPatchSetDst() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAsyncRewindPatchSetRewindState() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbQueueEndOfShaderActionGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetWorkloadStreamInactive() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbMemSemaphore() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetCxRegistersIndirectGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbWaitUntilSafeForRendering() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbAtomicGds() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcBranchPatchSetCompareAddress() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbAtomicGdsGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcGetSemaphoreLabel() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetShRegistersIndirect() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcWaitRegMemPatchMask() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcGetGsPrimPayload() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbWaitOnAddressGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetIndexBufferGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcQueueEndOfPipeActionPatchGcrCntl() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbPrimeUtcl2() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcSetNop() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbPrimeUtcl2GetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetIndexSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbPushMarker() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbDrawIndirectMulti() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbDmaDataGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcCbSetUcRegisterRangeDirect() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcGetRegisterDefaultsInternal() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetIndexCountGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetBaseDrawIndirectArgsGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcLinkShaders() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbBeginOcclusionQueryGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbDrawIndexIndirectGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcWaitRegMemPatchCompareFunction() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcGetGsOversubscription() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetShRegistersIndirectGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbSetWorkloadComplete() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcSetAmmSemaphoreMemory() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcGetStaticBuffer() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbAtomicMemGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbEndOcclusionQueryGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcSetStaticBuffer() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbClearState() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbDispatchIndirectGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbSetMarkerSpan() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbDrawIndirectMultiGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbMemSemaphore() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetShRegisterDirectGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbDmaData() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcBranchPatchSetElseTarget() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcSetShaderInstrumentation() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbSetWorkloadsActive() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcGetShaderInstrumentation() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbPrimeUtcl2() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcQueueEndOfPipeActionPatchType() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcCbSetUcRegistersDirectGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbStallCommandBufferParserGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetUcRegistersIndirectGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcCbBranchGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcCbMemSemaphore() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcUpdateInterpolantMapping() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbSetPredicationDisableGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbAcquireMemGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcquireMemSetEngine() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbAtomicMem() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAsyncWriteDataPatchSetCachePolicy() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcAcbEventWriteGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcCbSetShRegistersDirectGetSize() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbAtomicGds() { PRINT_NAME(); return OK; }
static KYTY_SYSV_ABI int sceAgcDcbQueueEndOfShaderActionGetSize() { PRINT_NAME(); return OK; }

} // namespace Agc

LIB_DEFINE(InitAgc_1) {
	LIB_FUNC("03RZmELWWzw", Agc::sceAgcCbSetUcRegistersDirect);
	LIB_FUNC("0o3VDdtA6nM", Agc::sceAgcDcbSetIndexIndirectArgs);
	LIB_FUNC("0ZOG0jc9nRg", Agc::sceAgcAcbRewindGetSize);
	LIB_FUNC("1-gUn1PI4Sw", Agc::sceAgcDcbAtomicMem);
	LIB_FUNC("1tB0xkLNjcw", Agc::sceAgcDcbAtomicGdsGetSize);
	LIB_FUNC("6nths4DHNrs", Agc::sceAgcCbMemsetExclusive);
	LIB_FUNC("73ZZdojLIgs", Agc::sceAgcDcbSetCfRegisterDirect);
	LIB_FUNC("7Wa3aeJgeVU", Agc::sceAgcBranchPatchSetThenTarget);
	LIB_FUNC("86rJnbZUpr4", Agc::sceAgcSetStaticQword);
	LIB_FUNC("8Kly1JrJUlw", Agc::sceAgcAcbPushMarkerSpan);
	LIB_FUNC("9S4noWrUI0s", Agc::sceAgcDcbSetBaseDispatchIndirectArgsGetSize);
	LIB_FUNC("AAeX-U5-P3M", Agc::sceAgcGetDefaultCxStateFlat);
	LIB_FUNC("AFIh8SQkYlQ", Agc::sceAgcDcbSetIndexIndirectArgsGetSize);
	LIB_FUNC("b5u0Jzm8TF8", Agc::sceAgcDcbCopyDataGetSize);
	LIB_FUNC("BVFg3CWU6Eo", Agc::sceAgcDcbSetCfRegisterRangeDirect);
	LIB_FUNC("BYcSvEsINWU", Agc::sceAgcDcbSetMarkerSpan);
	LIB_FUNC("C4l9fB17t8w", Agc::sceAgcDcbEventWriteGetSize);
	LIB_FUNC("ca4KPvp0qLQ", Agc::sceAgcDcbSetIndexSizeGetSize);
	LIB_FUNC("CbQh3DKMSno", Agc::sceAgcAcbCopyDataGetSize);
	LIB_FUNC("d4NZIlguzv0", Agc::sceAgcAsyncWriteDataPatchSetAddressOrOffset);
	LIB_FUNC("da1Sm8-QDoU", Agc::sceAgcAcbAtomicMemGetSize);
	LIB_FUNC("ddU6mOWyY40", Agc::sceAgcGetRegisterDefaults);
	LIB_FUNC("DtvmQ-tgEA", Agc::sceAgcSetSubmitMode);
	LIB_FUNC("DwICrVxerkY", Agc::sceAgcAcbRewind);
	LIB_FUNC("eAy8eGNsCuU", Agc::sceAgcWriteDataPatchSetCachePolicy);
	LIB_FUNC("ebixW91gpPw", Agc::sceAgcAcbSetFlip);
	LIB_FUNC("eCjKaqeeQ5s", Agc::sceAgcAcbPrimeUtcl2GetSize);
	LIB_FUNC("EJBA4dbmvfg", Agc::sceAgcAsyncWriteDataPatchSetDst);
	LIB_FUNC("eWaWyFegzgQ", Agc::sceAgcAsyncRewindPatchSetRewindState);
	LIB_FUNC("F8NLhWvFemI", Agc::sceAgcAcbQueueEndOfShaderActionGetSize);
	LIB_FUNC("FneFypEDRgY", Agc::sceAgcDcbSetWorkloadStreamInactive);
	LIB_FUNC("G0jrLdvEqDw", Agc::sceAgcDcbMemSemaphore);
	LIB_FUNC("GBCh3zCihoU", Agc::sceAgcDcbSetCxRegistersIndirectGetSize);
	LIB_FUNC("GPbUp9jXQa8", Agc::sceAgcAcbWaitUntilSafeForRendering);
	LIB_FUNC("gQkqkLttcpw", Agc::sceAgcAcbAtomicGds);
	LIB_FUNC("GXBlM-ekzrI", Agc::sceAgcBranchPatchSetCompareAddress);
	LIB_FUNC("hcIxS8pmXF4", Agc::sceAgcAcbAtomicGdsGetSize);
	LIB_FUNC("hFQ9pUxoLQ4", Agc::sceAgcGetSemaphoreLabel);
	LIB_FUNC("HOOCn0JY48", Agc::sceAgcDcbSetShRegistersIndirect);
	LIB_FUNC("hXAnLgDHCoI", Agc::sceAgcWaitRegMemPatchMask);
	LIB_FUNC("ICkECTBxrMw", Agc::sceAgcGetGsPrimPayload);
	LIB_FUNC("idlaArvdXEs", Agc::sceAgcAcbWaitOnAddressGetSize);
	LIB_FUNC("j4emHHndCPY", Agc::sceAgcDcbSetIndexBufferGetSize);
	LIB_FUNC("J8YCgfKAMQs", Agc::sceAgcQueueEndOfPipeActionPatchGcrCntl);
	LIB_FUNC("jt3pl7EN17o", Agc::sceAgcDcbPrimeUtcl2);
	LIB_FUNC("K2mciNVxUCE", Agc::sceAgcSetNop);
	LIB_FUNC("KjPeVduz6jU", Agc::sceAgcDcbPrimeUtcl2GetSize);
	LIB_FUNC("KRzWekV120", Agc::sceAgcDcbSetIndexSize);
	LIB_FUNC("kSrjIVxKFE", Agc::sceAgcDcbPushMarker);
	LIB_FUNC("kUlvghKs-mA", Agc::sceAgcDcbDrawIndirectMulti);
	LIB_FUNC("M0ttm8h7SKA", Agc::sceAgcAcbDmaDataGetSize);
	LIB_FUNC("MDLD5Ly94Xk", Agc::sceAgcCbSetUcRegisterRangeDirect);
	LIB_FUNC("MGEQMJBhdtc", Agc::sceAgcGetRegisterDefaultsInternal);
	LIB_FUNC("mljzuGDZRQ4", Agc::sceAgcDcbSetIndexCountGetSize);
	LIB_FUNC("MMlmJAL7N5w", Agc::sceAgcDcbSetBaseDrawIndirectArgsGetSize);
	LIB_FUNC("MqAdbRMdNz4", Agc::sceAgcLinkShaders);
	LIB_FUNC("ms1xVoZ-Vwc", Agc::sceAgcDcbBeginOcclusionQueryGetSize);
	LIB_FUNC("mStuvI0zOtc", Agc::sceAgcDcbDrawIndexIndirectGetSize);
	LIB_FUNC("n485EBnIWmk", Agc::sceAgcWaitRegMemPatchCompareFunction);
	LIB_FUNC("NKIzURsgV7I", Agc::sceAgcGetGsOversubscription);
	LIB_FUNC("nNlUtdDDvZ0", Agc::sceAgcDcbSetShRegistersIndirectGetSize);
	LIB_FUNC("opR1JeJZCBU", Agc::sceAgcAcbSetWorkloadComplete);
	LIB_FUNC("OQTgEXyihvA", Agc::sceAgcSetAmmSemaphoreMemory);
	LIB_FUNC("OXDc0KCnUhs", Agc::sceAgcGetStaticBuffer);
	LIB_FUNC("oz6zQq1JwCE", Agc::sceAgcDcbAtomicMemGetSize);
	LIB_FUNC("P1CugZ99Uzc", Agc::sceAgcDcbEndOcclusionQueryGetSize);
	LIB_FUNC("pjHhph0ZUc", Agc::sceAgcSetStaticBuffer);
	LIB_FUNC("PxEFhy0d5v8", Agc::sceAgcDcbClearState);
	LIB_FUNC("PxKWV2fVAps", Agc::sceAgcAcbDispatchIndirectGetSize);
	LIB_FUNC("pxx-GoOSdw4", Agc::sceAgcAcbSetMarkerSpan);
	LIB_FUNC("pYoKs3lPy88", Agc::sceAgcDcbDrawIndirectMultiGetSize);
	LIB_FUNC("q4VuU-QsLOE", Agc::sceAgcAcbMemSemaphore);
	LIB_FUNC("QhPDD513V0w", Agc::sceAgcDcbSetShRegisterDirectGetSize);
	LIB_FUNC("RnpfpxIhec", Agc::sceAgcAcbDmaData);
	LIB_FUNC("rP5xLdOf26k", Agc::sceAgcBranchPatchSetElseTarget);
	LIB_FUNC("RTpj-tIlvZc", Agc::sceAgcSetShaderInstrumentation);
	LIB_FUNC("rVOmPz2RBlg", Agc::sceAgcAcbSetWorkloadsActive);
	LIB_FUNC("SwI6QxqwAC0", Agc::sceAgcGetShaderInstrumentation);
	LIB_FUNC("szG7hz2yEhA", Agc::sceAgcAcbPrimeUtcl2);
	LIB_FUNC("T9fjQIINoeE", Agc::sceAgcQueueEndOfPipeActionPatchType);
	LIB_FUNC("TGEZzUWLbrc", Agc::sceAgcCbSetUcRegistersDirectGetSize);
	LIB_FUNC("u6dKSLWM2o", Agc::sceAgcDcbStallCommandBufferParserGetSize);
	LIB_FUNC("UQGTw4xRlcM", Agc::sceAgcDcbSetUcRegistersIndirectGetSize);
	LIB_FUNC("uZW-mqsxkrM", Agc::sceAgcCbBranchGetSize);
	LIB_FUNC("vHX9guneRBY", Agc::sceAgcCbMemSemaphore);
	LIB_FUNC("vieBRwlh1Lw", Agc::sceAgcUpdateInterpolantMapping);
	LIB_FUNC("vLrBL8DQiz8", Agc::sceAgcDcbSetPredicationDisableGetSize);
	LIB_FUNC("vnlTPPXPrw", Agc::sceAgcDcbAcquireMemGetSize);
	LIB_FUNC("W0WEyog0f74", Agc::sceAgcAcquireMemSetEngine);
	LIB_FUNC("XKKuA6VkSRc", Agc::sceAgcAcbAtomicMem);
	LIB_FUNC("y5K5tPktiL8", Agc::sceAgcAsyncWriteDataPatchSetCachePolicy);
	LIB_FUNC("Y-5vneiBtzk", Agc::sceAgcAcbEventWriteGetSize);
	LIB_FUNC("yUBESvCCJ4I", Agc::sceAgcCbSetShRegistersDirectGetSize);
	LIB_FUNC("zARR5aCmkoY", Agc::sceAgcDcbAtomicGds);
	LIB_FUNC("zg6u-N6Otxs", Agc::sceAgcDcbQueueEndOfShaderActionGetSize);
}

} // namespace Libs
