#pragma once
// ProsperoLayer PS5 emulator - libatrac9 shim interface.
//
// The real libatrac9 is a proprietary Sony library that is not publicly
// available. This shim provides the exact ABI used by the AJM ATRAC9
// decoder so the emulator builds; decoding returns ATRAC9_ERROR_NOT_SUPPORTED
// unless a real implementation is linked in (see ATRAC9_USE_EXTERNAL).
#include "common/common.h"
#include <cstdint>
#include <cstddef>

#ifdef __cplusplus
extern "C" {
#endif

#define ATRAC9_CONFIG_DATA_SIZE 20

#define ATRAC9_OK               0
#define ATRAC9_ERROR_FAIL      -1
#define ATRAC9_ERROR_NOT_SUPPORTED -2

typedef struct Atrac9CodecInfo {
        uint32_t channels;
        uint32_t samplingRate;
        uint32_t frameSamples;
        uint32_t superframeSize;
        uint32_t framesInSuperframe;
} Atrac9CodecInfo;

typedef void* Atrac9Handle;

Atrac9Handle Atrac9GetHandle(void);
void         Atrac9ReleaseHandle(Atrac9Handle handle);
int          Atrac9InitDecoder(Atrac9Handle handle, const uint8_t* config_data);
int          Atrac9GetCodecInfo(Atrac9Handle handle, Atrac9CodecInfo* info);
int          Atrac9Decode(Atrac9Handle handle, const uint8_t* input, int16_t* output,
                          int* bytes_used, int flags);
int          Atrac9DecodeS32(Atrac9Handle handle, const uint8_t* input, int32_t* output,
                             int* bytes_used, int flags);
int          Atrac9DecodeF32(Atrac9Handle handle, const uint8_t* input, float* output,
                             int* bytes_used, int flags);

#ifdef __cplusplus
}
#endif
