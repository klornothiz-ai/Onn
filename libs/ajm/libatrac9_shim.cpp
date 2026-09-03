// ProsperoLayer PS5 emulator - libatrac9 shim implementation.
// The real library is proprietary; this provides linkable symbols so the
// emulator builds. All decode operations report NOT_SUPPORTED.
#include "libs/ajm/libatrac9.h"
#include <cstring>

namespace {

struct Atrac9HandleImpl {
        bool initialized{false};
};

} // namespace

extern "C" {

Atrac9Handle Atrac9GetHandle(void) {
        auto* h = new (std::nothrow) Atrac9HandleImpl;
        return h;
}

void Atrac9ReleaseHandle(Atrac9Handle handle) {
        delete static_cast<Atrac9HandleImpl*>(handle);
}

int Atrac9InitDecoder(Atrac9Handle handle, const uint8_t* /*config_data*/) {
        if (handle == nullptr) {
                return ATRAC9_ERROR_FAIL;
        }
        auto* h = static_cast<Atrac9HandleImpl*>(handle);
        h->initialized = false;
        // No real decoder is available: report not supported so the AJM
        // layer fails gracefully instead of producing garbage audio.
        return ATRAC9_ERROR_NOT_SUPPORTED;
}

int Atrac9GetCodecInfo(Atrac9Handle handle, Atrac9CodecInfo* info) {
        if (handle == nullptr || info == nullptr) {
                return ATRAC9_ERROR_FAIL;
        }
        std::memset(info, 0, sizeof(*info));
        return ATRAC9_ERROR_NOT_SUPPORTED;
}

int Atrac9Decode(Atrac9Handle handle, const uint8_t* /*input*/, int16_t* /*output*/,
                 int* bytes_used, int /*flags*/) {
        if (bytes_used != nullptr) {
                *bytes_used = 0;
        }
        return handle != nullptr ? ATRAC9_ERROR_NOT_SUPPORTED : ATRAC9_ERROR_FAIL;
}

int Atrac9DecodeS32(Atrac9Handle handle, const uint8_t* /*input*/, int32_t* /*output*/,
                    int* bytes_used, int /*flags*/) {
        if (bytes_used != nullptr) {
                *bytes_used = 0;
        }
        return handle != nullptr ? ATRAC9_ERROR_NOT_SUPPORTED : ATRAC9_ERROR_FAIL;
}

int Atrac9DecodeF32(Atrac9Handle handle, const uint8_t* /*input*/, float* /*output*/,
                    int* bytes_used, int /*flags*/) {
        if (bytes_used != nullptr) {
                *bytes_used = 0;
        }
        return handle != nullptr ? ATRAC9_ERROR_NOT_SUPPORTED : ATRAC9_ERROR_FAIL;
}

} // extern "C"
