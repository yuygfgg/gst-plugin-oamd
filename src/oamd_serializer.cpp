#include "oamd_serializer_api.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glib.h>

#include "glaze/yaml.hpp"

namespace {
constexpr std::array<uint8_t, 6> kISFCountList = {4, 8, 10, 14, 15, 30};
constexpr std::array<uint16_t, 16> kRampDurationList = {
    32,   64,   128,  256,  320,  480,  1000, 1001,
    1024, 1600, 1601, 1602, 1920, 2000, 2002, 2048,
};

template <typename T> using Result = std::expected<T, std::string>;

struct BitReader {
    const guint8* data = nullptr;
    guint64 total_bits = 0;
    guint64 bit_pos = 0;

    BitReader(const guint8* input, gsize size)
        : data(input), total_bits(static_cast<guint64>(size) * 8), bit_pos(0) {}

    guint64 position() const { return bit_pos; }

    guint64 available() const { return total_bits - bit_pos; }

    template <typename T> bool get_n(guint32 n, T& out, std::string& error) {
        guint64 value = 0;
        guint64 i;

        if (n > 64) {
            error = "requested bit width is too large";
            return false;
        }

        if (bit_pos + n > total_bits) {
            error = "unexpected end of OAMD payload";
            return false;
        }

        for (i = 0; i < n; i++) {
            const guint64 pos = bit_pos + i;
            const guint8 byte = data[pos / 8];
            const guint8 bit = (byte >> (7 - (pos % 8))) & 1;
            value = (value << 1) | bit;
        }

        bit_pos += n;
        out = static_cast<T>(value);
        return true;
    }

    bool get(bool& out, std::string& error) {
        guint8 value = 0;

        if (!get_n(1, value, error))
            return false;

        out = value != 0;
        return true;
    }

    bool skip_n(guint32 n, std::string& error) {
        if (bit_pos + n > total_bits) {
            error = "unexpected end of OAMD payload";
            return false;
        }

        bit_pos += n;
        return true;
    }
};

static bool get_variable_bits_max(BitReader& reader, guint32 n,
                                  guint32 max_num_groups, guint64& out,
                                  std::string& error) {
    guint64 value = 0;
    guint32 num_group = 0;
    bool read_more = true;

    while (read_more && num_group < max_num_groups) {
        guint64 read = 0;

        if (!reader.get_n(n, read, error))
            return false;

        value += read;
        if (!reader.get(read_more, error))
            return false;

        if (read_more) {
            value = (value + 1) << n;
            num_group += 1;
        }
    }

    out = value;
    return true;
}

static bool get_sn(BitReader& reader, guint32 n, gint64& out,
                   std::string& error) {
    gint64 value = 0;
    gint64 sign = 0;

    if (n == 0 || n > 62) {
        error = "invalid signed bit width";
        return false;
    }

    if (!reader.get_n(n, value, error))
        return false;

    sign = gint64(1) << (n - 1);
    out = (value ^ sign) - sign;
    return true;
}

struct BedAssignment {
    std::array<bool, 17> channels{};

    static BedAssignment from_non_std(guint32 value) {
        BedAssignment ret;
        guint32 i;

        for (i = 0; i < ret.channels.size(); i++)
            ret.channels[i] = ((value >> i) & 1U) != 0;

        return ret;
    }

    static BedAssignment with_lfe_only() {
        BedAssignment ret;
        ret.channels[3] = true;
        return ret;
    }

    static BedAssignment from_std(guint16 value) {
        BedAssignment ret;
        auto set_pair = [&ret](guint32 a, guint32 b) {
            ret.channels[a] = true;
            ret.channels[b] = true;
        };

        if ((value >> 0) & 1U)
            set_pair(0, 1);
        if ((value >> 1) & 1U)
            ret.channels[2] = true;
        if ((value >> 2) & 1U)
            ret.channels[3] = true;
        if ((value >> 3) & 1U)
            set_pair(4, 5);
        if ((value >> 4) & 1U)
            set_pair(6, 7);
        if ((value >> 5) & 1U)
            set_pair(8, 9);
        if ((value >> 6) & 1U)
            set_pair(10, 11);
        if ((value >> 7) & 1U)
            set_pair(12, 13);
        if ((value >> 8) & 1U)
            set_pair(14, 15);
        if ((value >> 9) & 1U)
            ret.channels[16] = true;

        return ret;
    }

    guint8 count_beds() const {
        return static_cast<guint8>(
            std::count(channels.begin(), channels.end(), true));
    }
};

struct ProgramAssignment {
    bool b_dyn_object_only_program = false;
    bool b_lfe_present = false;
    guint8 content_description = 0;
    bool b_bed_chan_distribute = false;
    guint8 intermediate_spatial_format_idx = 0;
    guint8 num_dynamic_objects = 0;
    std::vector<BedAssignment> bed_assignment{};
    guint8 beds_or_isf_count = 0;
};

struct ObjectBasicInfo {
    guint8 object_gain_idx = 0;
    guint8 object_gain_bits = 0;
    bool b_default_object_priority = false;
    guint8 object_priority_bits = 0;
    double object_priority = 0.0;
};

struct ObjectRenderInfo {
    bool b_differential_position_specified = false;
    double pos3d_x = 0.0;
    double pos3d_y = 0.0;
    double pos3d_z = 0.0;
    bool b_object_distance_specified = false;
    bool b_object_at_infinity = false;
    guint8 distance_factor_idx = 0;
    guint8 zone_constraints_idx = 0;
    bool b_enable_elevation = false;
    guint8 object_size_idx = 0;
    guint8 object_size_bits = 0;
    guint8 object_width_bits = 0;
    guint8 object_depth_bits = 0;
    guint8 object_height_bits = 0;
    bool b_object_use_screen_ref = false;
    guint8 screen_factor_bits = 0;
    guint8 depth_factor_idx = 0;
    bool b_object_snap = false;
};

struct ObjectInfoBlock {
    bool b_object_not_active = false;
    guint8 object_basic_info_status_idx = 0;
    std::optional<ObjectBasicInfo> object_basic_info{};
    bool b_object_in_bed_or_isf = false;
    guint8 object_render_info_status_idx = 0;
    std::optional<ObjectRenderInfo> object_render_info{};
    bool b_additional_table_data_exists = false;
    guint32 additional_table_data_size_bits = 0;
};

struct ObjectData {
    std::vector<ObjectInfoBlock> object_info_block{};
};

struct BlockUpdateInfo {
    guint8 block_offset_factor_bits = 0;
    guint8 ramp_duration_code = 0;
    guint16 ramp_duration = 0;
};

struct MDUpdateInfo {
    guint8 sample_offset_code = 0;
    guint8 sample_offset = 0;
    guint8 num_obj_info_blocks = 0;
    std::vector<BlockUpdateInfo> block_update_info{};
};

struct ObjectElement {
    MDUpdateInfo md_update_info{};
    bool b_reserved_data_not_present = false;
    std::vector<ObjectData> object_data{};
};

struct OAElementMD {
    guint8 oa_element_id_idx = 0;
    guint64 oa_element_size_bits = 0;
    std::optional<guint8> alternate_object_data_id_idx{};
    bool b_discard_unknown_element = false;
    std::optional<ObjectElement> object_element{};
};

struct ObjectAudioMetadataPayload {
    guint8 oa_md_version_bits = 0;
    guint8 object_count = 0;
    ProgramAssignment program_assignment{};
    bool b_alternate_object_data_present = false;
    std::vector<OAElementMD> oa_element_md{};
};

static bool parse_program_assignment(BitReader& reader, ProgramAssignment& prog,
                                     std::string& error) {
    if (!reader.get(prog.b_dyn_object_only_program, error))
        return false;

    if (prog.b_dyn_object_only_program) {
        if (!reader.get(prog.b_lfe_present, error))
            return false;
        prog.bed_assignment.push_back(BedAssignment::with_lfe_only());
    } else {
        if (!reader.get_n(4, prog.content_description, error))
            return false;

        if (prog.content_description & 1U) {
            bool multiple_instances = false;
            guint8 num_bed_instances = 1;

            if (!reader.get(prog.b_bed_chan_distribute, error))
                return false;
            if (!reader.get(multiple_instances, error))
                return false;

            if (multiple_instances) {
                guint8 bits3 = 0;
                if (!reader.get_n(3, bits3, error))
                    return false;
                num_bed_instances = bits3 + 2;
            }

            for (guint8 i = 0; i < num_bed_instances; i++) {
                bool b_lfe_only = false;
                bool b_standard_chan_assign = false;
                guint16 std_value = 0;
                guint32 non_std_value = 0;

                if (!reader.get(b_lfe_only, error))
                    return false;

                if (b_lfe_only) {
                    prog.bed_assignment.push_back(
                        BedAssignment::with_lfe_only());
                    continue;
                }

                if (!reader.get(b_standard_chan_assign, error))
                    return false;

                if (b_standard_chan_assign) {
                    if (!reader.get_n(10, std_value, error))
                        return false;
                    prog.bed_assignment.push_back(
                        BedAssignment::from_std(std_value));
                } else {
                    if (!reader.get_n(17, non_std_value, error))
                        return false;
                    prog.bed_assignment.push_back(
                        BedAssignment::from_non_std(non_std_value));
                }
            }
        }

        if (prog.content_description & 2U) {
            if (!reader.get_n(3, prog.intermediate_spatial_format_idx, error))
                return false;

            if (prog.intermediate_spatial_format_idx >= kISFCountList.size()) {
                error = "invalid intermediate spatial format index";
                return false;
            }

            prog.beds_or_isf_count +=
                kISFCountList[prog.intermediate_spatial_format_idx];
        }

        if (prog.content_description & 4U) {
            guint8 num_dynamic_objects_bits = 0;

            if (!reader.get_n(5, num_dynamic_objects_bits, error))
                return false;

            if (num_dynamic_objects_bits == 31) {
                guint8 ext = 0;
                if (!reader.get_n(7, ext, error))
                    return false;
                num_dynamic_objects_bits =
                    static_cast<guint8>(num_dynamic_objects_bits + ext);
            }

            prog.num_dynamic_objects = num_dynamic_objects_bits + 1;
        }

        if (prog.content_description & 8U) {
            guint32 reserved_data_size = 0;

            if (!reader.get_n(4, reserved_data_size, error))
                return false;

            reserved_data_size = (reserved_data_size + 1) << 3;
            if (!reader.skip_n(reserved_data_size, error))
                return false;
        }
    }

    for (const auto& bed : prog.bed_assignment)
        prog.beds_or_isf_count =
            static_cast<guint8>(prog.beds_or_isf_count + bed.count_beds());

    return true;
}

static bool parse_block_update_info(BitReader& reader, BlockUpdateInfo& info,
                                    std::string& error) {
    guint16 ramp_duration_bits = 0;
    bool use_ramp_duration_idx = false;

    if (!reader.get_n(6, info.block_offset_factor_bits, error))
        return false;
    if (!reader.get_n(2, info.ramp_duration_code, error))
        return false;

    switch (info.ramp_duration_code) {
    case 0:
        info.ramp_duration = 0;
        break;
    case 1:
        info.ramp_duration = 512;
        break;
    case 2:
        info.ramp_duration = 1536;
        break;
    case 3: {
        guint64 idx = 0;

        if (!reader.get(use_ramp_duration_idx, error))
            return false;

        if (use_ramp_duration_idx) {
            if (!reader.get_n(4, idx, error))
                return false;

            if (idx >= kRampDurationList.size()) {
                error = "invalid ramp duration index";
                return false;
            }

            info.ramp_duration = kRampDurationList[idx];
        } else {
            if (!reader.get_n(11, ramp_duration_bits, error))
                return false;
            info.ramp_duration = ramp_duration_bits;
        }
        break;
    }
    default:
        error = "invalid ramp duration code";
        return false;
    }

    return true;
}

static bool parse_md_update_info(BitReader& reader, MDUpdateInfo& info,
                                 std::string& error) {
    guint8 sample_offset_idx = 0;
    guint8 sample_offset_bits = 0;

    if (!reader.get_n(2, info.sample_offset_code, error))
        return false;

    switch (info.sample_offset_code) {
    case 0:
        info.sample_offset = 0;
        break;
    case 1:
        if (!reader.get_n(2, sample_offset_idx, error))
            return false;

        switch (sample_offset_idx) {
        case 0:
            info.sample_offset = 8;
            break;
        case 1:
            info.sample_offset = 16;
            break;
        case 2:
            info.sample_offset = 18;
            break;
        case 3:
            info.sample_offset = 24;
            break;
        default:
            error = "invalid sample offset index";
            return false;
        }
        break;
    case 2:
        if (!reader.get_n(5, sample_offset_bits, error))
            return false;
        info.sample_offset = sample_offset_bits;
        break;
    default:
        error = "invalid sample offset code";
        return false;
    }

    if (!reader.get_n(3, info.num_obj_info_blocks, error))
        return false;
    info.num_obj_info_blocks =
        static_cast<guint8>(info.num_obj_info_blocks + 1);

    info.block_update_info.reserve(info.num_obj_info_blocks);
    for (guint8 i = 0; i < info.num_obj_info_blocks; i++) {
        BlockUpdateInfo block;

        if (!parse_block_update_info(reader, block, error))
            return false;

        info.block_update_info.push_back(block);
    }

    return true;
}

static bool parse_object_basic_info(BitReader& reader, ObjectInfoBlock& object,
                                    std::string& error) {
    guint8 obj_basic_info = 0;
    ObjectBasicInfo basic;

    if (object.object_basic_info_status_idx == 1) {
        obj_basic_info = 3;
    } else if (!reader.get_n(2, obj_basic_info, error)) {
        return false;
    }

    if (obj_basic_info & 1U) {
        if (!reader.get_n(2, basic.object_gain_idx, error))
            return false;

        if (basic.object_gain_idx == 2) {
            if (!reader.get_n(6, basic.object_gain_bits, error))
                return false;
        }
    }

    if (obj_basic_info & 2U) {
        if (!reader.get(basic.b_default_object_priority, error))
            return false;

        if (!basic.b_default_object_priority) {
            if (!reader.get_n(5, basic.object_priority_bits, error))
                return false;
            basic.object_priority =
                static_cast<double>(basic.object_priority_bits) / 32.0;
        } else {
            basic.object_priority = 1.0;
        }
    }

    object.object_basic_info = basic;
    return true;
}

static bool parse_object_render_info(BitReader& reader, ObjectInfoBlock& object,
                                     guint8 block, std::string& error) {
    guint8 object_render_info = 0;
    ObjectRenderInfo render;

    if (object.object_render_info_status_idx == 1) {
        object_render_info = 15;
    } else if (!reader.get_n(4, object_render_info, error)) {
        return false;
    }

    if (object_render_info & 1U) {
        gint64 signed_bits = 0;
        guint64 bits = 0;
        bool sign_z = false;

        if (block == 0) {
            render.b_differential_position_specified = false;
        } else if (!reader.get(render.b_differential_position_specified,
                               error)) {
            return false;
        }

        if (render.b_differential_position_specified) {
            if (!get_sn(reader, 3, signed_bits, error))
                return false;
            render.pos3d_x =
                std::min(static_cast<double>(signed_bits) / 62.0, 1.0);

            if (!get_sn(reader, 3, signed_bits, error))
                return false;
            render.pos3d_y =
                std::min(static_cast<double>(signed_bits) / 62.0, 1.0);

            if (!get_sn(reader, 3, signed_bits, error))
                return false;
            render.pos3d_z =
                std::min(static_cast<double>(signed_bits) / 15.0, 1.0);
        } else {
            if (!reader.get_n(6, bits, error))
                return false;
            render.pos3d_x = static_cast<double>(bits) / 62.0;

            if (!reader.get_n(6, bits, error))
                return false;
            render.pos3d_y = static_cast<double>(bits) / 62.0;

            if (!reader.get(sign_z, error))
                return false;

            if (!reader.get_n(4, bits, error))
                return false;
            render.pos3d_z = std::min((static_cast<double>(bits) / 15.0) *
                                          (sign_z ? 1.0 : -1.0),
                                      1.0);
        }

        if (!reader.get(render.b_object_distance_specified, error))
            return false;

        if (render.b_object_distance_specified) {
            if (!reader.get(render.b_object_at_infinity, error))
                return false;

            if (!render.b_object_at_infinity) {
                if (!reader.get_n(4, render.distance_factor_idx, error))
                    return false;
            }
        }
    }

    if (object_render_info & 2U) {
        if (!reader.get_n(3, render.zone_constraints_idx, error))
            return false;
        if (!reader.get(render.b_enable_elevation, error))
            return false;
    }

    if (object_render_info & 4U) {
        if (!reader.get_n(2, render.object_size_idx, error))
            return false;

        if (render.object_size_idx == 1) {
            if (!reader.get_n(5, render.object_size_bits, error))
                return false;
        } else if (render.object_size_idx == 2) {
            if (!reader.get_n(5, render.object_width_bits, error))
                return false;
            if (!reader.get_n(5, render.object_depth_bits, error))
                return false;
            if (!reader.get_n(5, render.object_height_bits, error))
                return false;
        }
    }

    if (object_render_info & 8U) {
        if (!reader.get(render.b_object_use_screen_ref, error))
            return false;

        if (render.b_object_use_screen_ref) {
            if (!reader.get_n(3, render.screen_factor_bits, error))
                return false;
            if (!reader.get_n(2, render.depth_factor_idx, error))
                return false;
        } else {
            render.screen_factor_bits = 0;
        }
    }

    if (!reader.get(render.b_object_snap, error))
        return false;

    object.object_render_info = render;
    return true;
}

static bool parse_object_info_block(BitReader& reader,
                                    const ProgramAssignment& program,
                                    guint8 block, guint8 object_index,
                                    ObjectInfoBlock& object,
                                    std::string& error) {
    if (!reader.get(object.b_object_not_active, error))
        return false;

    if (object.b_object_not_active) {
        object.object_basic_info_status_idx = 0;
    } else if (block == 0) {
        object.object_basic_info_status_idx = 1;
    } else if (!reader.get_n(2, object.object_basic_info_status_idx, error)) {
        return false;
    }

    if ((object.object_basic_info_status_idx & 1U) != 0 &&
        !parse_object_basic_info(reader, object, error)) {
        return false;
    }

    object.b_object_in_bed_or_isf = object_index < program.beds_or_isf_count;

    if (object.b_object_not_active) {
        object.object_render_info_status_idx = 0;
    } else if (!object.b_object_in_bed_or_isf) {
        if (block == 0) {
            object.object_render_info_status_idx = 1;
        } else if (!reader.get_n(2, object.object_render_info_status_idx,
                                 error)) {
            return false;
        }
    } else {
        object.object_render_info_status_idx = 0;
    }

    if ((object.object_render_info_status_idx & 1U) != 0 &&
        !parse_object_render_info(reader, object, block, error)) {
        return false;
    }

    if (!reader.get(object.b_additional_table_data_exists, error))
        return false;

    if (object.b_additional_table_data_exists) {
        if (!reader.get_n(4, object.additional_table_data_size_bits, error))
            return false;
        if (!reader.skip_n(object.additional_table_data_size_bits + 1, error))
            return false;
    }

    return true;
}

static bool parse_object_element(BitReader& reader,
                                 const ObjectAudioMetadataPayload& payload,
                                 ObjectElement& element, std::string& error) {
    if (!parse_md_update_info(reader, element.md_update_info, error))
        return false;
    if (!reader.get(element.b_reserved_data_not_present, error))
        return false;

    if (!element.b_reserved_data_not_present && !reader.skip_n(5, error))
        return false;

    element.object_data.reserve(payload.object_count);
    for (guint8 i = 0; i < payload.object_count; i++) {
        ObjectData object_data;

        object_data.object_info_block.reserve(
            element.md_update_info.num_obj_info_blocks);
        for (guint8 block = 0;
             block < element.md_update_info.num_obj_info_blocks; block++) {
            ObjectInfoBlock info_block;

            if (!parse_object_info_block(reader, payload.program_assignment,
                                         block, i, info_block, error)) {
                return false;
            }

            object_data.object_info_block.push_back(info_block);
        }

        element.object_data.push_back(std::move(object_data));
    }

    return true;
}

static bool parse_oa_element_md(BitReader& reader,
                                ObjectAudioMetadataPayload& payload,
                                OAElementMD& md, std::string& error) {
    guint64 pos_end = 0;

    if (!reader.get_n(4, md.oa_element_id_idx, error))
        return false;
    if (!get_variable_bits_max(reader, 4, 4, md.oa_element_size_bits, error))
        return false;

    pos_end = std::min(reader.position() + md.oa_element_size_bits + 1,
                       reader.position() + reader.available());

    if (payload.b_alternate_object_data_present) {
        guint8 value = 0;
        if (!reader.get_n(4, value, error))
            return false;
        md.alternate_object_data_id_idx = value;
    }

    if (!reader.get(md.b_discard_unknown_element, error))
        return false;

    if (md.oa_element_id_idx == 1) {
        ObjectElement object_element;

        if (!parse_object_element(reader, payload, object_element, error))
            return false;

        md.object_element = std::move(object_element);
    }

    if (pos_end > reader.position() &&
        !reader.skip_n(static_cast<guint32>(pos_end - reader.position()),
                       error)) {
        return false;
    }

    return true;
}

static bool read_payload(const guint8* bytes, gsize size,
                         ObjectAudioMetadataPayload& payload,
                         std::string& error) {
    BitReader reader(bytes, size);
    guint8 object_count_bits = 0;
    guint8 oa_element_count_bits = 0;

    if (!reader.get_n(2, payload.oa_md_version_bits, error))
        return false;

    if (payload.oa_md_version_bits == 3) {
        guint8 ext = 0;
        if (!reader.get_n(3, ext, error))
            return false;
        payload.oa_md_version_bits =
            static_cast<guint8>(payload.oa_md_version_bits + ext);
    }

    if (payload.oa_md_version_bits != 0) {
        error = "unsupported oa_md_version_bits";
        return false;
    }

    if (!reader.get_n(5, object_count_bits, error))
        return false;

    if (object_count_bits == 31) {
        guint8 ext = 0;
        if (!reader.get_n(7, ext, error))
            return false;
        object_count_bits = static_cast<guint8>(object_count_bits + ext);
    }

    payload.object_count = object_count_bits + 1;

    if (!parse_program_assignment(reader, payload.program_assignment, error))
        return false;
    if (!reader.get(payload.b_alternate_object_data_present, error))
        return false;
    if (!reader.get_n(4, oa_element_count_bits, error))
        return false;

    if (oa_element_count_bits == 15) {
        guint8 ext = 0;
        if (!reader.get_n(5, ext, error))
            return false;
        oa_element_count_bits =
            static_cast<guint8>(oa_element_count_bits + ext);
    }

    payload.oa_element_md.reserve(oa_element_count_bits);
    for (guint8 i = 0; i < oa_element_count_bits; i++) {
        OAElementMD md;

        if (!parse_oa_element_md(reader, payload, md, error))
            return false;

        payload.oa_element_md.push_back(std::move(md));
    }

    return true;
}

} // namespace

struct YamlEvent {
    std::optional<guint32> ID{};
    std::optional<guint64> samplePos{};
    std::optional<bool> active{};
    std::optional<std::vector<float>> pos{};
    std::optional<bool> snap{};
    std::optional<bool> elevation{};
    std::optional<std::string> zones{};
    std::optional<float> size{};
    std::optional<std::vector<float>> size3D{};
    std::optional<guint32> decorr{};
    std::optional<double> importance{};
    std::optional<std::string> gain{};
    std::optional<guint32> rampLength{};
    std::optional<bool> trimBypass{};
    std::optional<gint32> dialog{};
    std::optional<gint32> music{};
    std::optional<float> screenFactor{};
    std::optional<float> depthFactor{};
    std::optional<std::string> headTrackMode{};
    std::optional<std::string> binauralRenderMode{};

    bool operator==(const YamlEvent& other) const = default;
};

struct YamlConfiguration {
    std::optional<guint32> sampleRate{};
    std::vector<YamlEvent> events{};
};

namespace {

static bool build_configuration(const ObjectAudioMetadataPayload& payload,
                                guint32 sample_rate, guint64 sample_pos,
                                YamlConfiguration& config, std::string& error) {
    const auto object_count = static_cast<gsize>(payload.object_count);
    gint8 prev_object_gain = 0;
    guint64 event_sample_pos = sample_pos;

    if (payload.oa_element_md.empty()) {
        error = "OAMD payload contains no OA elements";
        return false;
    }

    if (!payload.oa_element_md.front().object_element.has_value()) {
        error = "OAMD payload does not contain a supported object element";
        return false;
    }

    const auto& object_element = *payload.oa_element_md.front().object_element;

    if (object_element.md_update_info.num_obj_info_blocks != 1) {
        error = "NOT_IMPL: Multiple Update Blocks";
        return false;
    }

    event_sample_pos += object_element.md_update_info.sample_offset;
    config.sampleRate = sample_rate;
    config.events.reserve(object_count);

    for (gsize i = 0; i < object_count; i++) {
        const auto& object_data =
            object_element.object_data[i].object_info_block[0];
        YamlEvent event;

        if (object_data.b_object_in_bed_or_isf) {
            if (i != 0) {
                error = "NOT_IMPL: LFE Only";
                return false;
            }
            event.ID = 3;
        } else {
            event.ID = static_cast<guint32>(i + 9);
        }

        event.samplePos = event_sample_pos;
        event.active = true;

        if (object_data.object_basic_info.has_value()) {
            const auto& basic = *object_data.object_basic_info;
            gint8 object_gain = 0;

            event.importance = basic.object_priority;

            switch (basic.object_gain_idx) {
            case 0:
                object_gain = 0;
                break;
            case 1:
                object_gain = -128;
                break;
            case 2:
                if (basic.object_gain_bits <= 14) {
                    object_gain =
                        static_cast<gint8>(basic.object_gain_bits + 1);
                } else {
                    object_gain =
                        static_cast<gint8>(basic.object_gain_bits - 64);
                }
                break;
            case 3:
                object_gain = prev_object_gain;
                break;
            default:
                error = "invalid object gain index";
                return false;
            }

            prev_object_gain = object_gain;
            event.gain =
                (object_gain == -128) ? "-inf" : std::to_string(object_gain);
            event.rampLength =
                object_element.md_update_info.block_update_info[0]
                    .ramp_duration;
        }

        if (object_data.object_render_info.has_value()) {
            const auto& render = *object_data.object_render_info;

            if (render.b_differential_position_specified) {
                error = "NOT_IMPL: diff pos";
                return false;
            }

            event.elevation = render.b_enable_elevation;
            event.snap = render.b_object_snap;
            event.pos = std::vector<float>{
                static_cast<float>((render.pos3d_x - 0.5) * 2.0),
                static_cast<float>((0.5 - render.pos3d_y) * 2.0),
                static_cast<float>(render.pos3d_z),
            };

            switch (render.zone_constraints_idx) {
            case 0:
                event.zones = "all";
                break;
            case 1:
                event.zones = "no back";
                break;
            case 2:
                event.zones = "no sides";
                break;
            case 3:
                event.zones = "center back";
                break;
            case 4:
                event.zones = "screen only";
                break;
            case 5:
                event.zones = "surround only";
                break;
            default:
                error = "invalid zone constraints index";
                return false;
            }

            if (render.object_size_idx == 2) {
                event.size3D = std::vector<float>{
                    static_cast<float>(render.object_width_bits) / 31.0f,
                    static_cast<float>(render.object_depth_bits) / 31.0f,
                    static_cast<float>(render.object_height_bits) / 31.0f,
                };
            } else {
                switch (render.object_size_idx) {
                case 0:
                    event.size = 0.0f;
                    break;
                case 1:
                    event.size =
                        static_cast<float>(render.object_size_bits) / 31.0f;
                    break;
                default:
                    error = "invalid object size index";
                    return false;
                }
            }

            event.screenFactor =
                render.b_object_use_screen_ref
                    ? 0.0f
                    : static_cast<float>(render.screen_factor_bits) / 8.0f;

            switch (render.depth_factor_idx) {
            case 0:
                event.depthFactor = 0.25f;
                break;
            case 1:
                event.depthFactor = 0.5f;
                break;
            case 2:
                event.depthFactor = 0.75f;
                break;
            case 3:
                event.depthFactor = 1.0f;
                break;
            default:
                error = "invalid depth factor index";
                return false;
            }

            event.dialog = -1;
            event.music = -1;
            event.binauralRenderMode = "undefined";
        } else {
            event.binauralRenderMode = "off";
        }

        event.trimBypass = false;
        event.headTrackMode = "undefined";
        config.events.push_back(std::move(event));
    }

    return true;
}

static YamlEvent compare_events(const YamlEvent& left, const YamlEvent& right) {
    YamlEvent out;

    out.active = (left.active == right.active) ? std::nullopt : right.active;
    out.pos = (left.pos == right.pos) ? std::nullopt : right.pos;
    out.snap = (left.snap == right.snap) ? std::nullopt : right.snap;
    out.elevation =
        (left.elevation == right.elevation) ? std::nullopt : right.elevation;
    out.zones = (left.zones == right.zones) ? std::nullopt : right.zones;
    out.size = (left.size == right.size) ? std::nullopt : right.size;
    out.size3D = (left.size3D == right.size3D) ? std::nullopt : right.size3D;
    out.decorr = (left.decorr == right.decorr) ? std::nullopt : right.decorr;
    out.importance =
        (left.importance == right.importance) ? std::nullopt : right.importance;
    out.gain = (left.gain == right.gain) ? std::nullopt : right.gain;
    out.rampLength =
        (left.rampLength == right.rampLength) ? std::nullopt : right.rampLength;
    out.trimBypass =
        (left.trimBypass == right.trimBypass) ? std::nullopt : right.trimBypass;
    out.dialog = (left.dialog == right.dialog) ? std::nullopt : right.dialog;
    out.music = (left.music == right.music) ? std::nullopt : right.music;
    out.screenFactor = (left.screenFactor == right.screenFactor)
                           ? std::nullopt
                           : right.screenFactor;
    out.depthFactor = (left.depthFactor == right.depthFactor)
                          ? std::nullopt
                          : right.depthFactor;
    out.headTrackMode = (left.headTrackMode == right.headTrackMode)
                            ? std::nullopt
                            : right.headTrackMode;
    out.binauralRenderMode =
        (left.binauralRenderMode == right.binauralRenderMode)
            ? std::nullopt
            : right.binauralRenderMode;

    if (!(out == YamlEvent{})) {
        out.ID = right.ID;
        out.samplePos = right.samplePos;
    }

    return out;
}

static std::vector<YamlEvent>
compare_event_vectors(const std::vector<YamlEvent>& left,
                      const std::vector<YamlEvent>& right) {
    std::vector<YamlEvent> out;
    const gsize n = std::min(left.size(), right.size());

    out.reserve(n);
    for (gsize i = 0; i < n; i++)
        out.push_back(compare_events(left[i], right[i]));

    return out;
}

static bool is_integer_scalar(std::string_view value) {
    if (value.empty())
        return false;

    gsize i = 0;
    if (value[0] == '-') {
        if (value.size() == 1)
            return false;
        i = 1;
    }

    for (; i < value.size(); i++) {
        if (!g_ascii_isdigit(static_cast<guchar>(value[i])))
            return false;
    }

    return true;
}

static void postprocess_yaml(std::string& output) {
    static constexpr std::array<std::string_view, 4> kFloatKeys = {
        "importance",
        "size",
        "screenFactor",
        "depthFactor",
    };

    output.erase(std::remove(output.begin(), output.end(), '\''), output.end());

    std::string processed;
    gsize line_start = 0;

    while (line_start < output.size()) {
        const gsize line_end = output.find('\n', line_start);
        const bool has_newline = line_end != std::string::npos;
        const gsize end = has_newline ? line_end : output.size();
        std::string line = output.substr(line_start, end - line_start);

        for (const auto& key : kFloatKeys) {
            const std::string marker = std::string(key) + ": ";
            const gsize marker_pos = line.find(marker);

            if (marker_pos == std::string::npos)
                continue;

            const gsize value_pos = marker_pos + marker.size();
            const std::string value = line.substr(value_pos);

            if (is_integer_scalar(value))
                line += ".0";
            break;
        }

        processed += line;
        if (has_newline)
            processed.push_back('\n');
        if (!has_newline)
            break;

        line_start = end + 1;
    }

    output = std::move(processed);
}

static bool serialize_configuration(YamlConfiguration config,
                                    bool remove_header, std::string& output,
                                    std::string& error) {
    if (remove_header) {
        std::vector<YamlEvent> filtered;

        filtered.reserve(config.events.size());
        for (const auto& event : config.events) {
            if (!(event == YamlEvent{}))
                filtered.push_back(event);
        }

        if (filtered.empty()) {
            output.clear();
            return true;
        }

        config.events = std::move(filtered);
        config.sampleRate.reset();
    }

    output.clear();
    if (auto ec = glz::write<glz::opts{
            .format = glz::YAML, .skip_null_members = true}>(config, output)) {
        error = glz::format_error(ec, output);
        return false;
    }

    postprocess_yaml(output);

    if (!output.empty() && output.back() != '\n')
        output.push_back('\n');

    if (remove_header) {
        static constexpr std::string_view prefix = "events:\n";

        if (output.rfind(prefix, 0) == 0)
            output.erase(0, prefix.size());
    }

    return true;
}

static gchar* dup_string(const std::string& value) {
    return g_strndup(value.c_str(), value.size());
}

static void set_error(gchar** error_out, const std::string& message) {
    if (error_out != nullptr)
        *error_out = dup_string(message);
}
} // namespace

struct _OAMDSerializerState {
    std::vector<YamlEvent> previous_events{};
    bool emitted_header = false;
};

extern "C" OAMDSerializerState* oamd_serializer_new(void) {
    return new _OAMDSerializerState();
}

extern "C" void oamd_serializer_reset(OAMDSerializerState* state) {
    if (state == nullptr)
        return;

    state->previous_events.clear();
    state->emitted_header = false;
}

extern "C" void oamd_serializer_free(OAMDSerializerState* state) {
    delete state;
}

extern "C" void oamd_string_free(gchar* value) { g_free(value); }

extern "C" gchar* oamd_serializer_process_payload(
    OAMDSerializerState* state, const guint8* payload, gsize payload_len,
    guint32 sample_rate, guint64 sample_pos, gchar** error_out) {
    ObjectAudioMetadataPayload parsed;
    YamlConfiguration config;
    std::vector<YamlEvent> current_events;
    std::string yaml;
    std::string error;

    if (error_out != nullptr)
        *error_out = nullptr;

    if (state == nullptr) {
        set_error(error_out, "serializer state is NULL");
        return nullptr;
    }

    if (payload == nullptr || payload_len == 0) {
        set_error(error_out, "payload is NULL or empty");
        return nullptr;
    }

    if (!read_payload(payload, payload_len, parsed, error)) {
        set_error(error_out, error);
        return nullptr;
    }

    if (!build_configuration(parsed, sample_rate, sample_pos, config, error)) {
        set_error(error_out, error);
        return nullptr;
    }

    current_events = config.events;
    if (!state->previous_events.empty())
        config.events =
            compare_event_vectors(state->previous_events, current_events);

    if (!serialize_configuration(config, state->emitted_header, yaml, error)) {
        set_error(error_out, error);
        return nullptr;
    }

    state->previous_events = std::move(current_events);
    if (!yaml.empty())
        state->emitted_header = true;

    return dup_string(yaml);
}
