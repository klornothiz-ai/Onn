#pragma once
// ProsperoLayer PS5 emulator - video out presentation interface (Kyty-compatible)
#include "common/common.h"
#include <cstdint>

namespace Graphics {
class RenderContext;
} // namespace Graphics

namespace Libs::Graphics::VideoOut {

struct VideoOutTiling {
        uint32_t mode;
        uint32_t tile_index;
        uint32_t pitch;
        uint32_t base_x;
        uint32_t base_y;
};

struct VideoOutBufferAttribute {
        uint64_t address;
        uint32_t width;
        uint32_t height;
        uint32_t pitch;
        uint32_t pixel_format;
        uint32_t tiling_mode;
        uint32_t aspect_ratio;
        uint32_t color_space;
        uint32_t num_memory_planes;
        uint32_t alpha;
        uint32_t reflection;
        uint32_t reserved0;
        uint32_t reserved1;
        uint64_t reserved2;
};

::Graphics::RenderContext& VideoOutInit(uint32_t width, uint32_t height,
                                        ::Graphics::RenderContext& presenter);
void VideoOutShutdown();

int KYTY_SYSV_ABI VideoOutOpen(int32_t user_id, int32_t type, int32_t index, int32_t* handle);
int KYTY_SYSV_ABI VideoOutClose(int32_t handle);
int KYTY_SYSV_ABI VideoOutSetBufferAttribute2(int32_t handle, VideoOutBufferAttribute* attr,
                                              int32_t num);
int KYTY_SYSV_ABI VideoOutRegisterBuffers2(int32_t handle, int32_t start_index,
                                           const void* const* addresses, int32_t num_buffers,
                                           const void* attribs, int32_t* out_label_addr);
int KYTY_SYSV_ABI VideoOutSubmitChangeBufferAttribute2(int32_t handle, int32_t index,
                                                       const void* attribs);
int KYTY_SYSV_ABI VideoOutUnregisterBuffers(int32_t handle, int32_t start_index,
                                            int32_t num_buffers);
int KYTY_SYSV_ABI VideoOutSetFlipRate(int32_t handle, int32_t rate);
int KYTY_SYSV_ABI VideoOutAddFlipEvent(int32_t handle, int32_t equeue, int32_t id, uint64_t udata);
int KYTY_SYSV_ABI VideoOutAddVblankEvent(int32_t handle, int32_t equeue, int32_t id,
                                         uint64_t udata);
int KYTY_SYSV_ABI VideoOutAddPreVblankStartEvent(int32_t handle, int32_t equeue, int32_t id,
                                                 uint64_t udata);
int KYTY_SYSV_ABI VideoOutDeleteFlipEvent(int32_t handle, int32_t equeue, int32_t id);
int KYTY_SYSV_ABI VideoOutDeleteVblankEvent(int32_t handle, int32_t equeue, int32_t id);
int KYTY_SYSV_ABI VideoOutDeletePreVblankStartEvent(int32_t handle, int32_t equeue, int32_t id);
int KYTY_SYSV_ABI VideoOutSubmitFlip(int32_t handle, int32_t buffer_index, int32_t flip_mode,
                                     int64_t flip_arg);
int KYTY_SYSV_ABI VideoOutGetFlipStatus(int32_t handle, int32_t* out_flip_arg,
                                        int32_t* out_counter);
int KYTY_SYSV_ABI VideoOutIsFlipPending(int32_t handle, int32_t* out_pending);
int KYTY_SYSV_ABI VideoOutGetVblankStatus(int32_t handle, int32_t* out_counter);
int KYTY_SYSV_ABI VideoOutSetWindowModeMargins(int32_t handle, int32_t top, int32_t bottom);
int KYTY_SYSV_ABI VideoOutAddOutputModeEvent(int32_t handle, int32_t equeue, int32_t id,
                                             uint64_t udata);
int KYTY_SYSV_ABI VideoOutGetEventId(const void* event);
int KYTY_SYSV_ABI VideoOutGetEventData(const void* event);
int KYTY_SYSV_ABI VideoOutGetEventCount(const void* event);
int KYTY_SYSV_ABI VideoOutWaitVblank(int32_t handle);
int KYTY_SYSV_ABI VideoOutGetOutputStatus(int32_t handle, int32_t* out_status);
int KYTY_SYSV_ABI VideoOutInitializeOutputOptions(int32_t handle, void* options);
int KYTY_SYSV_ABI VideoOutIsOutputSupported(int32_t handle, int32_t option);
int KYTY_SYSV_ABI VideoOutConfigureOutput(int32_t handle, const void* options);
int KYTY_SYSV_ABI VideoOutLatencyControlWaitBeforeInput(int32_t handle, int32_t option);
int KYTY_SYSV_ABI VideoOutLatencyMeasureSetStartPoint(int32_t handle, int32_t option);
int KYTY_SYSV_ABI VideoOutColorSettingsSetGamma(int32_t handle, float gamma);
int KYTY_SYSV_ABI VideoOutAdjustColor(int32_t handle, const void* settings);

} // namespace Libs::Graphics::VideoOut
