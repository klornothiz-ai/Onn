#pragma once
// ProsperoLayer PS5 emulator - guest shader subsystem (Kyty-compatible)
#include "common/common.h"
#include <cstdint>
#include <cstddef>

namespace Libs::Graphics {

// Shader binary types (Prospero toolchain).
namespace Prospero {
enum class ShaderBinaryType : uint8_t {
        kFs = 0,
        kVs = 1,
        kCs = 2,
        kGs = 3,
        kHs = 4,
        kPs = 5,
        kGsFront = 6,
        kHsFront = 7,
        kGsBack  = 8,
        kHsBack  = 9,
};

enum class PrimitiveType : uint32_t {
        kPointList = 0,
        kLineList = 1,
        kLineStrip = 2,
        kTriangleList = 3,
        kTriangleStrip = 4,
        kTriangleFan = 5,
        kLineListAdjacency = 6,
        kLineStripAdjacency = 7,
        kTriangleListAdjacency = 8,
        kTriangleStripAdjacency = 9,
        kRectList = 10,
        kRectListLegacy = 11,
        kQuadList = 12,
        kLineLoop = 13,
};

enum class GsOutputPrimitiveType : uint32_t {
        kPoints = 0,
        kLines = 1,
        kTriangles = 2,
        k2dRectangle = 3,
        kRectList = 4,
};

inline uint32_t GpuEnumValue(auto value) {
        return static_cast<uint32_t>(value);
}
} // namespace Prospero

struct ShaderRegister {
        uint32_t offset;
        uint32_t value;
};

struct ShaderSharpResource {
        uint16_t offset_dw;
        uint16_t size;
};

struct ShaderUserData {
        uint16_t            direct_resource_count;
        uint16_t            direct_resource_offset[32];
        uint16_t            sharp_resource_count[4];
        uint64_t            sharp_resource_offset[4];
        uint16_t            eud_size_dw;
        uint16_t            srt_size_dw;
        ShaderSharpResource sharp_data[128];
};

struct ShaderSemantic {
        uint32_t semantic;
        uint32_t hardware_mapping;
        uint32_t size_in_elements;
        uint32_t is_f16;
        uint32_t is_flat_shaded;
        uint32_t is_linear;
        uint32_t is_custom;
        uint32_t static_vb_index;
        uint32_t static_attribute;
        uint32_t reserved;
        uint32_t default_value;
        uint32_t default_value_hi;
};

struct DrawModifier {
        uint32_t enbl_start_vertex_offset;
        uint32_t enbl_start_index_offset;
        uint32_t enbl_start_instance_offset;
        uint32_t enbl_draw_index;
        uint32_t enbl_user_vgprs;
        uint32_t render_target_slice_offset;
        uint32_t fuse_draws;
        uint32_t compiler_flags;
        uint32_t is_default;
        uint32_t reserved;
};

struct ShaderSpecials {
        ShaderRegister ge_cntl;
        ShaderRegister vgt_shader_stages_en;
        ShaderRegister vgt_gs_out_prim_type;
        ShaderRegister ge_user_vgpr_en;
        DrawModifier   draw_modifier;

        struct DataRange {
                uint32_t start;
                uint32_t end;
        };
        DataRange user_data_range;

        DrawModifier dispatch_modifier;
};

struct Shader {
        uint32_t            file_header;
        uint32_t            version;
        uint8_t             type;
        uint8_t             num_cx_registers;
        uint8_t             num_sh_registers;
        uint8_t             num_input_semantics;
        uint8_t             num_output_semantics;
        uint8_t             reserved[3];
        uint32_t            shader_size;
        ShaderUserData*     user_data;
        ShaderSemantic*     input_semantics;
        ShaderSemantic*     output_semantics;
        ShaderRegister*     cx_registers;
        ShaderRegister*     sh_registers;
        ShaderSpecials*     specials;
        const volatile void* code;

        // Kyty-compatible extended fields (used by the SPIR-V recompiler).
        uint8_t target{0};
        uint32_t header_size{0};
        uint32_t special_sizes_bytes{0};
        uint32_t scratch_size_dw_per_thread{0};
        uint32_t embedded_constant_buffer_size_dqw{0};
};

struct ShaderMappedData {
        ShaderUserData* user_data;
        ShaderSemantic* input_semantics;
        uint32_t        num_input_semantics;
        uint32_t        code_size_bytes;
};

void ShaderInit();
void ShaderMapUserData(uint64_t base, const ShaderMappedData& map);

} // namespace Libs::Graphics
