#include "oamd_serializer_api.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <glib.h>

#include "glaze/yaml.hpp"

namespace {

constexpr gsize kMaxObjectCount = 159;
constexpr gint8 kGainMinusInfinity = -128;
// libdlb_oamdi.dylib .rodata @ 0x0000e720.
constexpr std::array<guint8, 6> kISFCountList = {4, 8, 10, 14, 15, 30};
// libdlb_oamdi.dylib .rodata @ 0x0000e8d0.
constexpr std::array<guint16, 16> kRampDurationList = {
    32,   64,   128,  256,  320,  480,  1000, 1001,
    1024, 1600, 1601, 1602, 1920, 2000, 2002, 2048,
};

// In `dee_audio_filter_dthd.dylib`, the trim quantizer at offset `0x0009bff0` uses an explicit float trim table:
//  `{3.0, 1.5, 0.75, 0.0, -0.75, -1.5, -3.0, -4.5, -6.0, -7.5, -9.0, -10.5, -12.0, -13.5, -15.0, -36.0}`.
// The corresponding raw-code map at offset `0x0091ecc0` is
//  `{0,1,2,3,16,4,5,6,7,8,9,10,11,12,13,14,15}`.
constexpr std::array<double, 16> kTrimLut = {
    6.0,  3.0,  1.5,  0.75,  -0.75, -1.5,  -3.0,  -4.5,
    -6.0, -7.5, -9.0, -10.5, -12.0, -13.5, -15.0, -36.0,
};

// Derived from libdlb_oamdi.dylib .rodata @ 0x0000ee90, which
// stores the special object-divergence codes {26, 29, 32, 63}. Resolving those
// through the Q14 table at 0x0000ee10 yields {0x200c / 16384,
// 0x26f2 / 16384, 0x2d1c / 16384, 0x4000 / 16384} =
// {0.500732421875, 0.6085205078125, 0.704833984375, 1.0}.
constexpr std::array<double, 4> kObjectDivTableTable = {
    0.500755,
    0.608529,
    0.704833,
    1.0,
};
// Derived from libdlb_oamdi.dylib .rodata @ 0x0000ee10, a Q14
// lookup table. Decimal values below are rounded from the exact source entries
// divided by 16384; index 0 remains reserved/null.
constexpr std::array<std::optional<double>, 64> kObjectDivCodeTable = {
    std::nullopt, 0.0,      0.004026, 0.00716,  0.012731, 0.020173, 0.028485,
    0.04021,      0.050582, 0.063601, 0.079914, 0.100299, 0.125666, 0.140532,
    0.157027,     0.175282, 0.195417, 0.217536, 0.241718, 0.268002, 0.296377,
    0.326766,     0.359017, 0.392895, 0.428081, 0.464184, 0.500755, 0.537316,
    0.573389,     0.608529, 0.642346, 0.674524, 0.704833, 0.733123, 0.75932,
    0.783416,     0.805451, 0.825506, 0.843686, 0.860112, 0.874914, 0.888222,
    0.900168,     0.910875, 0.920461, 0.929035, 0.936698, 0.943544, 0.949656,
    0.955112,     0.95998,  0.964322, 0.968195, 0.974729, 0.979923, 0.98405,
    0.98733,      0.989935, 0.992874, 0.994955, 0.996817, 0.99821,  0.998993,
    1.0,
};
// libdlb_oamdi.dylib .rodata @ 0x0000e880 as four int32 values {1, 2, -1, -2}.
constexpr std::array<double, 4> kExtPrecPos3DLut = {1.0, 2.0, -1.0, -2.0};
constexpr std::array<std::string_view, 6> kZoneLabels = {
    "all", "no back", "no sides", "center back", "screen only", "surround only",
};

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

        if (n > 64) {
            error = "requested bit width is too large";
            return false;
        }

        if (bit_pos + n > total_bits) {
            error = "unexpected end of OAMD payload";
            return false;
        }

        for (guint32 i = 0; i < n; ++i) {
            const guint64 pos = bit_pos + i;
            const guint8 byte = data[pos / 8];
            const guint8 bit = (byte >> (7 - (pos % 8))) & 1U;
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

    bool skip_n(guint64 n, std::string& error) {
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
    const gint64 sign = gint64(1) << (n - 1);

    if (n == 0 || n > 62) {
        error = "invalid signed bit width";
        return false;
    }

    if (!reader.get_n(n, value, error))
        return false;

    out = (value ^ sign) - sign;
    return true;
}

struct BedAssignment {
    std::array<bool, 17> channels{};

    static BedAssignment from_non_std(guint32 value) {
        BedAssignment ret;

        for (gsize i = 0; i < ret.channels.size(); ++i)
            ret.channels[i] = ((value >> i) & 1U) != 0;

        return ret;
    }

    static BedAssignment from_std(guint16 value) {
        static constexpr std::array<std::array<guint8, 2>, 10> kStdBedList = {{
            {{0, 1}},
            {{2, 0xff}},
            {{3, 0xff}},
            {{4, 5}},
            {{6, 7}},
            {{8, 9}},
            {{10, 11}},
            {{12, 13}},
            {{14, 15}},
            {{16, 0xff}},
        }};

        BedAssignment ret;

        for (gsize i = 0; i < kStdBedList.size(); ++i) {
            if (((value >> i) & 1U) == 0)
                continue;

            ret.channels[kStdBedList[i][0]] = true;
            if (kStdBedList[i][1] != 0xff)
                ret.channels[kStdBedList[i][1]] = true;
        }

        return ret;
    }

    static BedAssignment with_lfe_only() {
        BedAssignment ret;
        ret.channels[3] = true;
        return ret;
    }

    gsize count_beds() const {
        return static_cast<gsize>(
            std::count(channels.begin(), channels.end(), true));
    }

    std::vector<gsize> to_index_vec() const {
        std::vector<gsize> out;

        out.reserve(count_beds());
        for (gsize i = 0; i < channels.size(); ++i) {
            if (channels[i])
                out.push_back(i);
        }

        return out;
    }
};

struct ProgramAssignment {
    bool b_dyn_object_only_program = false;
    bool b_bed_chan_distribute = false;
    std::vector<BedAssignment> bed_assignment{};
    gsize num_bed_objects = 0;
    gsize num_isf_objects = 0;
    gsize num_dynamic_objects = 0;

    gsize beds_or_isf_count() const {
        return num_bed_objects + num_isf_objects;
    }
};

struct ObjectBasicInfo {
    gint8 object_gain = kGainMinusInfinity;
    double object_priority = 0.0;

    std::string gain_string() const {
        if (object_gain == kGainMinusInfinity)
            return "-inf";

        return std::to_string(object_gain);
    }
};

struct ObjectRenderInfo {
    bool b_differential_position_specified = false;
    std::array<double, 3> pos3d{0.5, 0.5, 0.0};
    bool b_object_distance_specified = false;
    bool b_object_at_infinity = false;
    guint8 distance_factor_idx = 0;
    guint8 zone_constraints_idx = 0;
    bool b_enable_elevation = true;
    std::array<double, 3> object_size{0.0, 0.0, 0.0};
    bool b_has_size3d = false;
    bool b_object_use_screen_ref = false;
    double screen_factor = 0.0;
    double depth_factor = 0.25;
    bool b_object_snap = false;
};

struct BlockUpdateInfo {
    guint8 block_offset_factor_bits = 0;
    guint8 ramp_duration_code = 0;
    guint16 ramp_duration = 0;
};

struct MDUpdateInfo {
    gsize sample_offset = 0;
    gsize num_obj_info_blocks = 0;
    std::vector<BlockUpdateInfo> block_update_info{};
};

struct ObjectInfoBlock {
    bool b_object_not_active = false;
    guint8 object_basic_info_status_idx = 0;
    bool has_object_basic_info = false;
    ObjectBasicInfo object_basic_info{};
    bool b_object_in_bed_or_isf = false;
    guint8 object_render_info_status_idx = 0;
    bool has_object_render_info = false;
    ObjectRenderInfo object_render_info{};
    bool b_additional_table_data_exists = false;
    guint32 additional_table_data_size_bits = 0;
};

using ObjectData = std::vector<ObjectInfoBlock>;

struct ObjectElement {
    MDUpdateInfo md_update_info{};
    bool b_reserved_data_not_present = false;
    guint8 reserved_data = 32;
    std::vector<ObjectData> object_data{};
};

struct Trim {
    bool b_default_trim = true;
    bool b_disable_trim = false;
    std::optional<double> trim_centre{};
    std::optional<double> trim_surround{};
    std::optional<double> trim_height{};
    std::optional<double> bal3d_y_tb{};
    std::optional<double> bal3d_y_lis{};
};

struct TrimElement {
    guint8 warp_mode = 0;
    guint8 reserved = 0;
    guint8 global_trim_mode = 0;
    std::array<std::optional<Trim>, 9> trims{};
    bool b_disable_trim_per_obj = false;
    std::vector<bool> b_disable_trim{};
};

struct ObjectDivergenceBlock {
    double object_divergence = 0.0;
    bool b_object_divergence = false;
    guint8 object_div_mode = 0;
    guint8 object_div_table = 0;
    guint8 object_div_code = 0;
};

struct ExtendedPrecisionPositionBlock {
    double ext_prec_pos3d_x = 0.0;
    double ext_prec_pos3d_y = 0.0;
    double ext_prec_pos3d_z = 0.0;
};

struct ExtendedObjectElement {
    bool b_obj_div_block = false;
    std::vector<std::vector<ObjectDivergenceBlock>> object_div_block{};
    bool b_ext_prec_pos_block = false;
    std::vector<std::vector<ExtendedPrecisionPositionBlock>>
        ext_prec_pos_block{};
};

struct OAMDParserState {
    gsize object_count = 0;
    ProgramAssignment program_assignment{};
    bool b_alternate_object_data_present = false;
    std::array<gint8, kMaxObjectCount> prev_object_gain{};
    ObjectBasicInfo prev_object_basic_info{};
    ObjectRenderInfo prev_object_render_info{};
    std::optional<ObjectElement> object_element{};
    std::optional<TrimElement> trim_element{};
    std::optional<ExtendedObjectElement> extended_object_element{};
};

struct OAElementMD {
    guint8 oa_element_id_idx = 0;
    guint64 oa_element_size_bits = 0;
    std::optional<guint8> alternate_object_data_id_idx{};
    bool b_discard_unknown_element = false;
};

struct ObjectAudioMetadataPayload {
    guint64 evo_sample_offset = 0;
    guint8 oamd_version = 0;
    gsize object_count = 0;
    ProgramAssignment program_assignment{};
    bool b_alternate_object_data_present = false;
    std::optional<ObjectElement> object_element{};
    std::optional<TrimElement> trim_element{};
    std::optional<ExtendedObjectElement> extended_object_element{};
    std::vector<OAElementMD> oa_element_md{};

    std::vector<std::vector<std::array<double, 3>>> get_damf_pos() const {
        std::vector<std::vector<std::array<double, 3>>> damf_pos(object_count);

        if (object_element.has_value()) {
            for (gsize object_index = 0; object_index < object_count;
                 ++object_index) {
                if (object_index >= object_element->object_data.size())
                    continue;

                const auto& object_blocks =
                    object_element->object_data[object_index];

                damf_pos[object_index].reserve(object_blocks.size());
                for (const auto& block : object_blocks)
                    damf_pos[object_index].push_back(
                        block.object_render_info.pos3d);
            }
        }

        if (extended_object_element.has_value()) {
            for (gsize object_index = 0;
                 object_index <
                 std::min(object_count,
                          extended_object_element->ext_prec_pos_block.size());
                 ++object_index) {
                auto& pos_blocks = damf_pos[object_index];
                const auto& ext_blocks =
                    extended_object_element->ext_prec_pos_block[object_index];

                for (gsize block_index = 0;
                     block_index <
                     std::min(pos_blocks.size(), ext_blocks.size());
                     ++block_index) {
                    pos_blocks[block_index][0] +=
                        ext_blocks[block_index].ext_prec_pos3d_x;
                    pos_blocks[block_index][1] +=
                        ext_blocks[block_index].ext_prec_pos3d_y;
                    pos_blocks[block_index][2] +=
                        ext_blocks[block_index].ext_prec_pos3d_z;
                }
            }
        }

        for (auto& pos_blocks : damf_pos) {
            for (auto& pos3d : pos_blocks) {
                pos3d[0] = (std::clamp(pos3d[0], 0.0, 1.0) - 0.5) * 2.0;
                pos3d[1] = (0.5 - std::clamp(pos3d[1], 0.0, 1.0)) * 2.0;
                pos3d[2] = std::clamp(pos3d[2], -1.0, 1.0);
            }
        }

        return damf_pos;
    }
};

static bool parse_program_assignment(BitReader& reader, OAMDParserState& state,
                                     std::string& error) {
    auto& prog = state.program_assignment;

    if (!reader.get(prog.b_dyn_object_only_program, error))
        return false;

    if (prog.b_dyn_object_only_program) {
        prog.num_dynamic_objects = state.object_count;

        bool b_lfe_present = false;

        if (!reader.get(b_lfe_present, error))
            return false;

        if (b_lfe_present) {
            prog.bed_assignment.push_back(BedAssignment::with_lfe_only());
            if (prog.num_dynamic_objects > 0)
                prog.num_dynamic_objects -= 1;
        }
    } else {
        guint8 content_description = 0;

        if (!reader.get_n(4, content_description, error))
            return false;

        if ((content_description & 1U) != 0) {
            bool multiple_instances = false;
            guint8 num_bed_instances = 1;

            if (!reader.get(prog.b_bed_chan_distribute, error))
                return false;
            if (!reader.get(multiple_instances, error))
                return false;

            if (multiple_instances) {
                guint8 num_bed_instances_bits = 0;

                if (!reader.get_n(3, num_bed_instances_bits, error))
                    return false;

                num_bed_instances =
                    static_cast<guint8>(num_bed_instances_bits + 2);
            }

            for (guint8 i = 0; i < num_bed_instances; ++i) {
                bool b_lfe_only = false;

                if (!reader.get(b_lfe_only, error))
                    return false;

                if (b_lfe_only) {
                    prog.bed_assignment.push_back(
                        BedAssignment::with_lfe_only());
                    continue;
                }

                bool b_standard_chan_assign = false;

                if (!reader.get(b_standard_chan_assign, error))
                    return false;

                if (b_standard_chan_assign) {
                    guint16 value = 0;

                    if (!reader.get_n(10, value, error))
                        return false;

                    prog.bed_assignment.push_back(
                        BedAssignment::from_std(value));
                } else {
                    guint32 value = 0;

                    if (!reader.get_n(17, value, error))
                        return false;

                    prog.bed_assignment.push_back(
                        BedAssignment::from_non_std(value));
                }
            }
        }

        if ((content_description & 2U) != 0) {
            guint8 intermediate_spatial_format_idx = 0;

            if (!reader.get_n(3, intermediate_spatial_format_idx, error))
                return false;

            if (intermediate_spatial_format_idx >= kISFCountList.size()) {
                error = "invalid intermediate spatial format index";
                return false;
            }

            prog.num_isf_objects =
                kISFCountList[intermediate_spatial_format_idx];
        }

        if ((content_description & 4U) != 0) {
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

            prog.num_dynamic_objects =
                static_cast<gsize>(num_dynamic_objects_bits) + 1;
        }

        if ((content_description & 8U) != 0) {
            guint32 reserved_data_size_bits = 0;

            if (!reader.get_n(4, reserved_data_size_bits, error))
                return false;

            if (!reader.skip_n(
                    (static_cast<guint64>(reserved_data_size_bits) + 1) << 3,
                    error)) {
                return false;
            }
        }
    }

    for (const auto& bed : prog.bed_assignment)
        prog.num_bed_objects += bed.count_beds();

    return true;
}

static bool parse_block_update_info(BitReader& reader, BlockUpdateInfo& info,
                                    std::string& error) {
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
    case 3:
        if (!reader.get(use_ramp_duration_idx, error))
            return false;

        if (use_ramp_duration_idx) {
            guint64 idx = 0;

            if (!reader.get_n(4, idx, error))
                return false;

            if (idx >= kRampDurationList.size()) {
                error = "invalid ramp duration index";
                return false;
            }

            info.ramp_duration = kRampDurationList[idx];
        } else {
            if (!reader.get_n(11, info.ramp_duration, error))
                return false;
        }
        break;
    default:
        error = "invalid ramp duration code";
        return false;
    }

    return true;
}

static bool parse_md_update_info(BitReader& reader, MDUpdateInfo& info,
                                 std::string& error) {
    guint8 sample_offset_code = 0;

    if (!reader.get_n(2, sample_offset_code, error))
        return false;

    switch (sample_offset_code) {
    case 0:
        info.sample_offset = 0;
        break;
    case 1: {
        guint8 sample_offset_idx = 0;

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
    }
    case 2: {
        guint8 sample_offset_bits = 0;

        if (!reader.get_n(5, sample_offset_bits, error))
            return false;

        info.sample_offset = sample_offset_bits;
        break;
    }
    default:
        error = "invalid sample offset code";
        return false;
    }

    guint8 num_obj_info_blocks_bits = 0;

    if (!reader.get_n(3, num_obj_info_blocks_bits, error))
        return false;

    info.num_obj_info_blocks = static_cast<gsize>(num_obj_info_blocks_bits) + 1;
    info.block_update_info.reserve(info.num_obj_info_blocks);

    for (gsize i = 0; i < info.num_obj_info_blocks; ++i) {
        BlockUpdateInfo block_update_info;

        if (!parse_block_update_info(reader, block_update_info, error))
            return false;

        info.block_update_info.push_back(block_update_info);
    }

    return true;
}

static bool parse_object_basic_info(BitReader& reader, OAMDParserState& state,
                                    gsize object_index, gsize block_index,
                                    guint8 status_idx,
                                    const ObjectBasicInfo& prev_basic_info,
                                    ObjectBasicInfo& basic_info,
                                    std::string& error) {
    guint8 object_basic_info_bits = 0;

    basic_info = prev_basic_info;

    if (status_idx == 1) {
        object_basic_info_bits = 3;
    } else if (!reader.get_n(2, object_basic_info_bits, error)) {
        return false;
    }

    if ((object_basic_info_bits & 1U) != 0) {
        guint8 object_gain_idx = 0;

        if (!reader.get_n(2, object_gain_idx, error))
            return false;

        switch (object_gain_idx) {
        case 0:
            basic_info.object_gain = 0;
            break;
        case 1:
            basic_info.object_gain = kGainMinusInfinity;
            break;
        case 2: {
            guint8 object_gain_bits = 0;

            if (!reader.get_n(6, object_gain_bits, error))
                return false;

            basic_info.object_gain =
                (object_gain_bits <= 14)
                    ? static_cast<gint8>(object_gain_bits + 1)
                    : static_cast<gint8>(object_gain_bits - 64);
            break;
        }
        case 3:
            basic_info.object_gain =
                (object_index == 0) ? 0 : state.prev_object_gain[block_index];
            break;
        default:
            error = "invalid object gain index";
            return false;
        }

        state.prev_object_gain[block_index] = basic_info.object_gain;
    }

    if ((object_basic_info_bits & 2U) != 0) {
        bool b_default_object_priority = false;

        if (!reader.get(b_default_object_priority, error))
            return false;

        if (b_default_object_priority) {
            basic_info.object_priority = 1.0;
        } else {
            guint8 object_priority_bits = 0;

            if (!reader.get_n(5, object_priority_bits, error))
                return false;

            basic_info.object_priority =
                static_cast<double>(object_priority_bits) / 32.0;
        }
    }

    return true;
}

static bool parse_object_render_info(BitReader& reader, guint8 status_idx,
                                     gsize block_index,
                                     const ObjectRenderInfo& prev_render_info,
                                     ObjectRenderInfo& render_info,
                                     std::string& error) {
    guint8 object_render_info_bits = 0;

    render_info = prev_render_info;

    if (status_idx == 1) {
        object_render_info_bits = 15;
    } else if (!reader.get_n(4, object_render_info_bits, error)) {
        return false;
    }

    if ((object_render_info_bits & 1U) != 0) {
        if (block_index == 0) {
            render_info.b_differential_position_specified = false;
        } else if (!reader.get(render_info.b_differential_position_specified,
                               error)) {
            return false;
        }

        if (render_info.b_differential_position_specified) {
            gint64 signed_bits = 0;

            if (!get_sn(reader, 3, signed_bits, error))
                return false;
            render_info.pos3d[0] = prev_render_info.pos3d[0] +
                                   static_cast<double>(signed_bits) / 62.0;

            if (!get_sn(reader, 3, signed_bits, error))
                return false;
            render_info.pos3d[1] = prev_render_info.pos3d[1] +
                                   static_cast<double>(signed_bits) / 62.0;

            if (!get_sn(reader, 3, signed_bits, error))
                return false;
            render_info.pos3d[2] = prev_render_info.pos3d[2] +
                                   static_cast<double>(signed_bits) / 15.0;
        } else {
            guint64 bits = 0;
            bool sign_z = false;

            if (!reader.get_n(6, bits, error))
                return false;
            render_info.pos3d[0] = static_cast<double>(bits) / 62.0;

            if (!reader.get_n(6, bits, error))
                return false;
            render_info.pos3d[1] = static_cast<double>(bits) / 62.0;

            if (!reader.get(sign_z, error))
                return false;
            if (!reader.get_n(4, bits, error))
                return false;
            render_info.pos3d[2] =
                static_cast<double>(bits) / 15.0 * (sign_z ? 1.0 : -1.0);
        }

        if (!reader.get(render_info.b_object_distance_specified, error))
            return false;

        if (render_info.b_object_distance_specified) {
            if (!reader.get(render_info.b_object_at_infinity, error))
                return false;
            if (!render_info.b_object_at_infinity &&
                !reader.get_n(4, render_info.distance_factor_idx, error)) {
                return false;
            }
        }
    }

    if ((object_render_info_bits & 2U) != 0) {
        if (!reader.get_n(3, render_info.zone_constraints_idx, error))
            return false;
        if (!reader.get(render_info.b_enable_elevation, error))
            return false;
    }

    if ((object_render_info_bits & 4U) != 0) {
        guint8 object_size_idx = 0;

        if (!reader.get_n(2, object_size_idx, error))
            return false;

        render_info.b_has_size3d = false;

        switch (object_size_idx) {
        case 0:
            render_info.object_size = {0.0, 0.0, 0.0};
            break;
        case 1: {
            guint8 object_size_bits = 0;

            if (!reader.get_n(5, object_size_bits, error))
                return false;

            const double object_size =
                static_cast<double>(object_size_bits) / 31.0;
            render_info.object_size = {object_size, object_size, object_size};
            break;
        }
        case 2: {
            guint8 object_width_bits = 0;
            guint8 object_depth_bits = 0;
            guint8 object_height_bits = 0;

            if (!reader.get_n(5, object_width_bits, error))
                return false;
            if (!reader.get_n(5, object_depth_bits, error))
                return false;
            if (!reader.get_n(5, object_height_bits, error))
                return false;

            render_info.object_size = {
                static_cast<double>(object_width_bits) / 31.0,
                static_cast<double>(object_depth_bits) / 31.0,
                static_cast<double>(object_height_bits) / 31.0,
            };
            render_info.b_has_size3d = true;
            break;
        }
        default:
            render_info.object_size = {0.0, 0.0, 0.0};
            render_info.b_has_size3d = false;
            break;
        }
    }

    if ((object_render_info_bits & 8U) != 0) {
        if (!reader.get(render_info.b_object_use_screen_ref, error))
            return false;

        if (render_info.b_object_use_screen_ref) {
            guint8 screen_factor_bits = 0;
            guint8 depth_factor_idx = 0;

            if (!reader.get_n(3, screen_factor_bits, error))
                return false;
            if (!reader.get_n(2, depth_factor_idx, error))
                return false;

            render_info.screen_factor =
                static_cast<double>(screen_factor_bits + 1) / 8.0;
            render_info.depth_factor =
                0.25 * static_cast<double>(depth_factor_idx + 1);
        } else {
            render_info.screen_factor = 0.0;
        }
    }

    if (!reader.get(render_info.b_object_snap, error))
        return false;

    return true;
}

static bool parse_object_info_block(BitReader& reader, OAMDParserState& state,
                                    gsize object_index, gsize block_index,
                                    ObjectInfoBlock& object_info_block,
                                    std::string& error) {
    if (!reader.get(object_info_block.b_object_not_active, error))
        return false;

    if (object_info_block.b_object_not_active) {
        object_info_block.object_basic_info_status_idx = 0;
    } else if (block_index == 0) {
        object_info_block.object_basic_info_status_idx = 1;
    } else if (!reader.get_n(2, object_info_block.object_basic_info_status_idx,
                             error)) {
        return false;
    }

    {
        const ObjectBasicInfo prev_basic_info =
            (block_index == 0) ? ObjectBasicInfo{}
                               : state.prev_object_basic_info;

        switch (object_info_block.object_basic_info_status_idx) {
        case 0:
            object_info_block.object_basic_info = ObjectBasicInfo{};
            object_info_block.has_object_basic_info = false;
            break;
        case 1:
        case 3:
            if (!parse_object_basic_info(
                    reader, state, object_index, block_index,
                    object_info_block.object_basic_info_status_idx,
                    prev_basic_info, object_info_block.object_basic_info,
                    error)) {
                return false;
            }
            object_info_block.has_object_basic_info = true;
            break;
        case 2:
            object_info_block.object_basic_info = prev_basic_info;
            object_info_block.has_object_basic_info = true;
            break;
        default:
            error = "invalid object basic info status";
            return false;
        }

        state.prev_object_basic_info = object_info_block.object_basic_info;
    }

    object_info_block.b_object_in_bed_or_isf =
        object_index < state.program_assignment.beds_or_isf_count();

    if (object_info_block.b_object_not_active) {
        object_info_block.object_render_info_status_idx = 0;
    } else if (!object_info_block.b_object_in_bed_or_isf) {
        if (block_index == 0) {
            object_info_block.object_render_info_status_idx = 1;
        } else if (!reader.get_n(
                       2, object_info_block.object_render_info_status_idx,
                       error)) {
            return false;
        }
    } else {
        object_info_block.object_render_info_status_idx = 0;
    }

    {
        const ObjectRenderInfo prev_render_info =
            (block_index == 0) ? ObjectRenderInfo{}
                               : state.prev_object_render_info;

        switch (object_info_block.object_render_info_status_idx) {
        case 0:
            object_info_block.object_render_info = ObjectRenderInfo{};
            object_info_block.has_object_render_info = false;
            break;
        case 1:
        case 3:
            if (!parse_object_render_info(
                    reader, object_info_block.object_render_info_status_idx,
                    block_index, prev_render_info,
                    object_info_block.object_render_info, error)) {
                return false;
            }
            object_info_block.has_object_render_info = true;
            break;
        case 2:
            object_info_block.object_render_info = prev_render_info;
            object_info_block.has_object_render_info = true;
            break;
        default:
            error = "invalid object render info status";
            return false;
        }

        state.prev_object_render_info = object_info_block.object_render_info;
    }

    if (!reader.get(object_info_block.b_additional_table_data_exists, error))
        return false;

    if (object_info_block.b_additional_table_data_exists) {
        if (!reader.get_n(4, object_info_block.additional_table_data_size_bits,
                          error)) {
            return false;
        }

        if (!reader.skip_n(
                (static_cast<guint64>(
                     object_info_block.additional_table_data_size_bits) +
                 1) << 3,
                error)) {
            return false;
        }
    }

    return true;
}

static bool parse_object_element(BitReader& reader, OAMDParserState& state,
                                 ObjectElement& element, std::string& error) {
    if (!parse_md_update_info(reader, element.md_update_info, error))
        return false;
    if (!reader.get(element.b_reserved_data_not_present, error))
        return false;

    if (!element.b_reserved_data_not_present &&
        !reader.get_n(5, element.reserved_data, error)) {
        return false;
    }

    element.object_data.reserve(state.object_count);

    for (gsize object_index = 0; object_index < state.object_count;
         ++object_index) {
        ObjectData object_data;

        object_data.reserve(element.md_update_info.num_obj_info_blocks);
        for (gsize block_index = 0;
             block_index < element.md_update_info.num_obj_info_blocks;
             ++block_index) {
            ObjectInfoBlock object_info_block;

            if (!parse_object_info_block(reader, state, object_index,
                                         block_index, object_info_block,
                                         error)) {
                return false;
            }

            object_data.push_back(object_info_block);
        }

        element.object_data.push_back(std::move(object_data));
    }

    return true;
}

static bool parse_trim_element(BitReader& reader, const OAMDParserState& state,
                               TrimElement& element, std::string& error) {
    if (!reader.get_n(2, element.warp_mode, error))
        return false;
    if (!reader.get_n(2, element.reserved, error))
        return false;
    if (!reader.get_n(2, element.global_trim_mode, error))
        return false;

    if (element.global_trim_mode == 2) {
        for (auto& trim : element.trims) {
            Trim value;

            if (!reader.get(value.b_default_trim, error))
                return false;

            if (value.b_default_trim) {
                trim = value;
                continue;
            }

            if (!reader.get(value.b_disable_trim, error))
                return false;

            if (!value.b_disable_trim) {
                guint8 trim_balance_presence = 0;

                if (!reader.get_n(5, trim_balance_presence, error))
                    return false;

                if ((trim_balance_presence & 0x1U) != 0) {
                    guint8 idx = 0;

                    if (!reader.get_n(4, idx, error))
                        return false;
                    value.trim_centre = kTrimLut[idx];
                }

                if ((trim_balance_presence & 0x2U) != 0) {
                    guint8 idx = 0;

                    if (!reader.get_n(4, idx, error))
                        return false;
                    value.trim_surround = kTrimLut[idx];
                }

                if ((trim_balance_presence & 0x4U) != 0) {
                    guint8 idx = 0;

                    if (!reader.get_n(4, idx, error))
                        return false;
                    value.trim_height = kTrimLut[idx];
                }

                if ((trim_balance_presence & 0x8U) != 0) {
                    bool sign = false;
                    guint8 bits = 0;

                    if (!reader.get(sign, error))
                        return false;
                    if (!reader.get_n(4, bits, error))
                        return false;
                    value.bal3d_y_tb = static_cast<double>(bits + 1) / 16.0 *
                                       (sign ? 1.0 : -1.0);
                }

                if ((trim_balance_presence & 0x10U) != 0) {
                    bool sign = false;
                    guint8 bits = 0;

                    if (!reader.get(sign, error))
                        return false;
                    if (!reader.get_n(4, bits, error))
                        return false;
                    value.bal3d_y_lis = static_cast<double>(bits + 1) / 16.0 *
                                        (sign ? 1.0 : -1.0);
                }
            }

            trim = value;
        }
    }

    if (!reader.get(element.b_disable_trim_per_obj, error))
        return false;

    if (element.b_disable_trim_per_obj) {
        element.b_disable_trim.reserve(state.object_count);

        for (gsize i = 0; i < state.object_count; ++i) {
            bool value = false;

            if (!reader.get(value, error))
                return false;

            element.b_disable_trim.push_back(value);
        }
    }

    return true;
}

static bool parse_extended_object_element(BitReader& reader,
                                          OAMDParserState& state,
                                          ExtendedObjectElement& element,
                                          std::string& error) {
    if (!reader.get(element.b_obj_div_block, error))
        return false;

    if (!state.object_element.has_value())
        return true;

    const auto& object_element = *state.object_element;

    if (element.b_obj_div_block) {
        element.object_div_block.reserve(state.object_count);

        for (gsize object_index = 0; object_index < state.object_count;
             ++object_index) {
            std::vector<ObjectDivergenceBlock> block_list;

            if (object_index < object_element.object_data.size())
                block_list.reserve(
                    object_element.object_data[object_index].size());

            for (const auto& object_info_block :
                 object_element.object_data[object_index]) {
                ObjectDivergenceBlock block;

                if (!object_info_block.b_object_not_active &&
                    !object_info_block.b_object_in_bed_or_isf) {
                    if (!reader.get(block.b_object_divergence, error))
                        return false;

                    if (block.b_object_divergence) {
                        if (!reader.get_n(2, block.object_div_mode, error))
                            return false;

                        if (block.object_div_mode == 0) {
                            if (!reader.get_n(2, block.object_div_table, error))
                                return false;
                            block.object_divergence =
                                kObjectDivTableTable[block.object_div_table];
                        } else if (block.object_div_mode == 1) {
                            if (!block_list.empty())
                                block = block_list.back();
                        } else {
                            if (!reader.get_n(6, block.object_div_code, error))
                                return false;
                            if (kObjectDivCodeTable[block.object_div_code]
                                    .has_value()) {
                                block.object_divergence =
                                    *kObjectDivCodeTable[block.object_div_code];
                            }
                        }
                    }
                }

                block_list.push_back(block);
            }

            element.object_div_block.push_back(std::move(block_list));
        }
    }

    if (!reader.get(element.b_ext_prec_pos_block, error))
        return false;

    if (element.b_ext_prec_pos_block) {
        element.ext_prec_pos_block.reserve(state.object_count);

        for (gsize object_index = 0; object_index < state.object_count;
             ++object_index) {
            std::vector<ExtendedPrecisionPositionBlock> block_list;

            if (object_index < object_element.object_data.size())
                block_list.reserve(
                    object_element.object_data[object_index].size());

            for (const auto& object_info_block :
                 object_element.object_data[object_index]) {
                ExtendedPrecisionPositionBlock block;

                if (!object_info_block.b_object_not_active &&
                    !object_info_block.b_object_in_bed_or_isf) {
                    bool b_ext_prec_pos = false;

                    if (!reader.get(b_ext_prec_pos, error))
                        return false;

                    if (b_ext_prec_pos) {
                        guint8 ext_prec_pos_presence = 0;

                        if (!reader.get_n(3, ext_prec_pos_presence, error))
                            return false;

                        if ((ext_prec_pos_presence & 0x1U) != 0) {
                            guint8 idx = 0;

                            if (!reader.get_n(2, idx, error))
                                return false;
                            block.ext_prec_pos3d_x =
                                kExtPrecPos3DLut[idx] / 310.0;
                        }

                        if ((ext_prec_pos_presence & 0x2U) != 0) {
                            guint8 idx = 0;

                            if (!reader.get_n(2, idx, error))
                                return false;
                            block.ext_prec_pos3d_y =
                                kExtPrecPos3DLut[idx] / 310.0;
                        }

                        if ((ext_prec_pos_presence & 0x4U) != 0) {
                            guint8 idx = 0;

                            if (!reader.get_n(2, idx, error))
                                return false;
                            block.ext_prec_pos3d_z =
                                kExtPrecPos3DLut[idx] / 75.0;
                        }
                    }
                }

                block_list.push_back(block);
            }

            element.ext_prec_pos_block.push_back(std::move(block_list));
        }
    }

    return true;
}

static bool parse_oa_element_md(BitReader& reader, OAMDParserState& state,
                                OAElementMD& md, std::string& error) {
    guint64 element_size_bits = 0;

    if (!reader.get_n(4, md.oa_element_id_idx, error))
        return false;
    if (!get_variable_bits_max(reader, 4, 4, element_size_bits, error))
        return false;

    md.oa_element_size_bits = element_size_bits;

    const guint64 element_size = (element_size_bits + 1) << 3;
    const guint64 pos_start = reader.position();
    const guint64 pos_end =
        pos_start + std::min(element_size, reader.available());

    if (state.b_alternate_object_data_present) {
        guint8 alternate_object_data_id_idx = 0;

        if (!reader.get_n(4, alternate_object_data_id_idx, error))
            return false;

        md.alternate_object_data_id_idx = alternate_object_data_id_idx;
    }

    if (!reader.get(md.b_discard_unknown_element, error))
        return false;

    switch (md.oa_element_id_idx) {
    case 1: {
        ObjectElement object_element;

        if (!parse_object_element(reader, state, object_element, error))
            return false;
        state.object_element = std::move(object_element);
        break;
    }
    case 2: {
        TrimElement trim_element;

        if (!parse_trim_element(reader, state, trim_element, error))
            return false;
        state.trim_element = std::move(trim_element);
        break;
    }
    case 5: {
        ExtendedObjectElement extended_object_element;

        if (!parse_extended_object_element(reader, state,
                                           extended_object_element, error)) {
            return false;
        }
        state.extended_object_element = std::move(extended_object_element);
        break;
    }
    default:
        break;
    }

    if (pos_end > reader.position() &&
        !reader.skip_n(pos_end - reader.position(), error)) {
        return false;
    }

    return true;
}

static bool read_payload(const guint8* bytes, gsize size,
                         ObjectAudioMetadataPayload& payload,
                         std::string& error) {
    BitReader reader(bytes, size);
    OAMDParserState state;
    guint8 object_count_bits = 0;
    guint8 oa_element_count_bits = 0;

    if (!reader.get_n(2, payload.oamd_version, error))
        return false;

    if (payload.oamd_version == 3) {
        guint8 ext = 0;

        if (!reader.get_n(3, ext, error))
            return false;

        payload.oamd_version = static_cast<guint8>(payload.oamd_version + ext);
    }

    if (payload.oamd_version != 0) {
        error = "unsupported OAMD version";
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

    state.object_count = static_cast<gsize>(object_count_bits) + 1;
    if (state.object_count > kMaxObjectCount) {
        error = "object count exceeds supported maximum";
        return false;
    }

    if (!parse_program_assignment(reader, state, error))
        return false;
    if (!reader.get(state.b_alternate_object_data_present, error))
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
    for (guint8 i = 0; i < oa_element_count_bits; ++i) {
        OAElementMD md;

        if (!parse_oa_element_md(reader, state, md, error))
            return false;

        payload.oa_element_md.push_back(std::move(md));
    }

    payload.object_count = state.object_count;
    payload.program_assignment = std::move(state.program_assignment);
    payload.b_alternate_object_data_present =
        state.b_alternate_object_data_present;
    payload.object_element = std::move(state.object_element);
    payload.trim_element = std::move(state.trim_element);
    payload.extended_object_element = std::move(state.extended_object_element);

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

static std::vector<bool>
build_trim_bypass_vector(const ObjectAudioMetadataPayload& payload) {
    std::vector<bool> out(payload.object_count, false);

    if (!payload.trim_element.has_value())
        return out;

    const auto& trim_element = *payload.trim_element;

    if (trim_element.b_disable_trim_per_obj) {
        out.assign(trim_element.b_disable_trim.begin(),
                   trim_element.b_disable_trim.end());
        if (out.size() < payload.object_count)
            out.resize(payload.object_count, false);
        return out;
    }

    if (trim_element.global_trim_mode == 1)
        std::fill(out.begin(), out.end(), true);

    return out;
}

static std::vector<gsize>
collect_bed_indices(const ProgramAssignment& program_assignment) {
    std::vector<gsize> out;

    for (const auto& bed_assignment : program_assignment.bed_assignment) {
        auto indices = bed_assignment.to_index_vec();
        out.insert(out.end(), indices.begin(), indices.end());
    }

    return out;
}

static guint32 map_bed_index_to_id(gsize index) {
    if (index < 8)
        return static_cast<guint32>(index);
    if (index < 10)
        return static_cast<guint32>(index + 122);
    if (index < 12)
        return static_cast<guint32>(index - 2);

    return static_cast<guint32>(index + 120);
}

static bool build_configuration(const ObjectAudioMetadataPayload& payload,
                                guint32 sample_rate, guint64 sample_pos,
                                YamlConfiguration& config, std::string& error) {
    const auto object_count = payload.object_count;
    const auto pos_vec = payload.get_damf_pos();
    const auto trim_bypass_vec = build_trim_bypass_vector(payload);
    const auto bed_index_vec = collect_bed_indices(payload.program_assignment);

    config.sampleRate = sample_rate;

    if (!payload.object_element.has_value()) {
        error = "OAMD payload does not contain a supported object element";
        return false;
    }

    if (payload.program_assignment.num_isf_objects != 0) {
        error = "NOT_IMPL: ISF objects";
        return false;
    }

    const auto& object_element = *payload.object_element;
    const auto num_blocks = object_element.md_update_info.num_obj_info_blocks;

    if (num_blocks == 0) {
        error = "OAMD object element contains no update blocks";
        return false;
    }

    config.events.reserve(object_count * num_blocks);

    const guint64 base_sample_pos = sample_pos + payload.evo_sample_offset +
                                    object_element.md_update_info.sample_offset;

    for (gsize block_index = 0; block_index < num_blocks; ++block_index) {
        guint64 current_sample_pos = base_sample_pos;
        guint32 ramp_duration = 0;

        if (block_index <
            object_element.md_update_info.block_update_info.size()) {
            const auto& block_update_info =
                object_element.md_update_info.block_update_info[block_index];
            /*
             * For the single-block payloads emitted by the Reference Player
             * decoder, block_offset_factor_bits does not advance the first
             * event position. Adding it here produced a +1 samplePos skew
             * versus the proprietary serializer on real .mlp content.
             */
            if (block_index > 0)
                current_sample_pos +=
                    block_update_info.block_offset_factor_bits;
            ramp_duration = block_update_info.ramp_duration;
        }

        for (gsize object_index = 0; object_index < object_count;
             ++object_index) {
            if (object_index >= object_element.object_data.size() ||
                block_index >=
                    object_element.object_data[object_index].size()) {
                continue;
            }

            const auto& object_data =
                object_element.object_data[object_index][block_index];
            YamlEvent event;

            if (object_data.b_object_in_bed_or_isf) {
                if (object_index >= bed_index_vec.size()) {
                    error = "NOT_IMPL: unsupported bed or ISF assignment";
                    return false;
                }

                event.ID = map_bed_index_to_id(bed_index_vec[object_index]);
            } else {
                event.ID = static_cast<guint32>(
                    object_index + 10 -
                    static_cast<gsize>(bed_index_vec.size()));
            }

            event.samplePos = current_sample_pos;
            event.active = !object_data.b_object_not_active;

            if (object_data.has_object_basic_info) {
                event.importance =
                    object_data.object_basic_info.object_priority;
                event.gain = object_data.object_basic_info.gain_string();
                event.rampLength = ramp_duration;
            }

            if (!object_data.b_object_in_bed_or_isf &&
                object_data.has_object_render_info) {
                const auto& render = object_data.object_render_info;

                event.elevation = render.b_enable_elevation;
                event.snap = render.b_object_snap;

                if (object_index < pos_vec.size() &&
                    block_index < pos_vec[object_index].size()) {
                    const auto& pos = pos_vec[object_index][block_index];
                    event.pos = std::vector<float>{
                        static_cast<float>(pos[0]),
                        static_cast<float>(pos[1]),
                        static_cast<float>(pos[2]),
                    };
                }

                if (render.zone_constraints_idx < kZoneLabels.size()) {
                    event.zones =
                        std::string(kZoneLabels[render.zone_constraints_idx]);
                } else {
                    error = "invalid zone constraints index";
                    return false;
                }

                if (render.b_has_size3d) {
                    event.size3D = std::vector<float>{
                        static_cast<float>(render.object_size[0]),
                        static_cast<float>(render.object_size[1]),
                        static_cast<float>(render.object_size[2]),
                    };
                } else {
                    event.size = static_cast<float>(render.object_size[0]);
                }

                event.screenFactor = static_cast<float>(render.screen_factor);
                event.depthFactor = static_cast<float>(render.depth_factor);
                event.dialog = -1;
                event.music = -1;
                event.binauralRenderMode = "undefined";
            } else {
                event.binauralRenderMode = "off";
            }

            event.trimBypass = (object_index < trim_bypass_vec.size())
                                   ? trim_bypass_vec[object_index]
                                   : false;
            event.headTrackMode = "undefined";

            config.events.push_back(std::move(event));
        }
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

    out.reserve(right.size());
    for (gsize i = 0; i < right.size(); ++i) {
        if (i < left.size()) {
            out.push_back(compare_events(left[i], right[i]));
        } else {
            out.push_back(right[i]);
        }
    }

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

    for (; i < value.size(); ++i) {
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
