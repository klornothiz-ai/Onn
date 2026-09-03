// ============================================================================
// ProsperoLayer RDNA2 Core - Optional Backend Stubs (FFmpeg absent)
// ============================================================================
// Description: Provides the minimal symbols that would otherwise come from
//              FFmpeg/libavcodec when FFmpeg development headers are not
//              installed. Compiled ONLY when FFmpeg is absent, so it never
//              collides with libavutil's real symbol table.
// ============================================================================

#include "libs/libs.h"
#include "loader/symbolDatabase.h"

// libFont.cpp references this symbol; it is normally provided by FFmpeg's
// libavutil. `extern` forces external linkage (C++ would otherwise make a
// namespace-scope `const` array internal-linkage and the reference would
// not resolve). The table is a zeroed placeholder; the reference backend
// (libFont) is not reachable without a full font path in this build anyway.
extern const uint8_t avpriv_vga16_font[4096] = {0};

namespace Libs {

namespace VideoDec2 {
// libVideoDec2.cpp provides the real registration behind FFmpeg.
LIB_DEFINE(InitVideoDec2_1) {
    // No-op: video-decoder subsystem unavailable without FFmpeg.
}
} // namespace VideoDec2

} // namespace Libs