#include "oamdcapsfeature.h"

#include <gst/audio/audio.h>
#include <gst/gst.h>

#include "dlbaudiometa.h"

enum {
    PROP_0,
    PROP_REMOVE,
    PROP_FLOAT,
};

struct _DlbOAMDCapsFeature {
    GstBaseTransform parent;
    gboolean remove;
    gboolean convert_to_float;
    GstAudioInfo in_info;
    GstAudioInfo out_info;
    gboolean have_in_info;
    gboolean have_out_info;
};

G_DEFINE_TYPE(DlbOAMDCapsFeature, dlb_oamd_caps_feature,
              GST_TYPE_BASE_TRANSFORM)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "audio/x-raw, "
        "format = (string) { " GST_AUDIO_NE(S32) ", " GST_AUDIO_NE(
            F32) " }, "
                 "channels = (int) [ 1, 64 ], "
                 "rate = (int) [ 1, MAX ], "
                 "layout = (string) interleaved; "
                 "audio/x-raw(" DLB_CAPS_FEATURE_META_OBJECT_AUDIO_META "), "
                 "format = (string) { " GST_AUDIO_NE(S32) ", " GST_AUDIO_NE(
                     F32) " }, "
                          "channels = (int) [ 1, 64 ], "
                          "rate = (int) [ 1, MAX ], "
                          "layout = (string) interleaved"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "audio/x-raw, "
        "format = (string) { " GST_AUDIO_NE(S32) ", " GST_AUDIO_NE(
            F32) " }, "
                 "channels = (int) [ 1, 64 ], "
                 "rate = (int) [ 1, MAX ], "
                 "layout = (string) interleaved; "
                 "audio/x-raw(" DLB_CAPS_FEATURE_META_OBJECT_AUDIO_META "), "
                 "format = (string) { " GST_AUDIO_NE(S32) ", " GST_AUDIO_NE(
                     F32) " }, "
                          "channels = (int) [ 1, 64 ], "
                          "rate = (int) [ 1, MAX ], "
                          "layout = (string) interleaved"));

static void set_format_value(GstStructure *s, GstPadDirection direction,
                             gboolean convert_to_float) {
    GValue formats = G_VALUE_INIT;
    GValue item = G_VALUE_INIT;

    if (!convert_to_float)
        return;

    if (direction == GST_PAD_SINK) {
        gst_structure_set(s, "format", G_TYPE_STRING, GST_AUDIO_NE(F32), NULL);
        return;
    }

    g_value_init(&formats, GST_TYPE_LIST);
    g_value_init(&item, G_TYPE_STRING);

    g_value_set_static_string(&item, GST_AUDIO_NE(S32));
    gst_value_list_append_value(&formats, &item);
    g_value_set_static_string(&item, GST_AUDIO_NE(F32));
    gst_value_list_append_value(&formats, &item);

    gst_structure_set_value(s, "format", &formats);

    g_value_unset(&item);
    g_value_unset(&formats);
}

static GstCaps *dlb_oamd_caps_feature_transform_caps(GstBaseTransform *trans,
                                                     GstPadDirection direction,
                                                     GstCaps *caps,
                                                     GstCaps *filter) {
    DlbOAMDCapsFeature *self = DLB_OAMD_CAPS_FEATURE(trans);
    GstCaps *ret;
    guint i;

    ret = gst_caps_copy(caps);

    for (i = 0; i < gst_caps_get_size(ret); i++) {
        GstCapsFeatures *features = gst_caps_get_features(ret, i);
        GstStructure *s = gst_caps_get_structure(ret, i);

        if (!gst_caps_features_is_any(features)) {
            if (self->remove && direction == GST_PAD_SINK) {
                gst_caps_features_remove(
                    features, DLB_CAPS_FEATURE_META_OBJECT_AUDIO_META);
            } else if (self->remove && direction == GST_PAD_SRC) {
                if (!gst_caps_features_contains(
                        features, DLB_CAPS_FEATURE_META_OBJECT_AUDIO_META)) {
                    gst_caps_features_add(
                        features, DLB_CAPS_FEATURE_META_OBJECT_AUDIO_META);
                }
            }

            if (gst_caps_features_contains(
                    features, GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY)) {
                gst_caps_features_remove(features,
                                         GST_CAPS_FEATURE_MEMORY_SYSTEM_MEMORY);
            }
        }

        set_format_value(s, direction, self->convert_to_float);
    }

    if (filter != NULL) {
        GstCaps *intersection =
            gst_caps_intersect_full(ret, filter, GST_CAPS_INTERSECT_FIRST);
        gst_caps_unref(ret);
        ret = intersection;
    }

    return ret;
}

static gboolean dlb_oamd_caps_feature_set_caps(GstBaseTransform *trans,
                                               GstCaps *incaps,
                                               GstCaps *outcaps) {
    DlbOAMDCapsFeature *self = DLB_OAMD_CAPS_FEATURE(trans);

    self->have_in_info = gst_audio_info_from_caps(&self->in_info, incaps);
    self->have_out_info = gst_audio_info_from_caps(&self->out_info, outcaps);

    return self->have_in_info && self->have_out_info;
}

static GstFlowReturn dlb_oamd_caps_feature_transform_ip(GstBaseTransform *trans,
                                                        GstBuffer *buf) {
    DlbOAMDCapsFeature *self = DLB_OAMD_CAPS_FEATURE(trans);
    GstMapInfo map;
    gsize i;
    gsize samples;

    if (!self->convert_to_float)
        return GST_FLOW_OK;

    if (!self->have_in_info || !self->have_out_info) {
        GST_ERROR_OBJECT(self, "caps not negotiated");
        return GST_FLOW_NOT_NEGOTIATED;
    }

    if (GST_AUDIO_INFO_FORMAT(&self->in_info) == GST_AUDIO_FORMAT_F32LE)
        return GST_FLOW_OK;

    if (GST_AUDIO_INFO_FORMAT(&self->in_info) != GST_AUDIO_FORMAT_S32LE) {
        GST_ERROR_OBJECT(
            self, "unsupported input format %s",
            gst_audio_format_to_string(GST_AUDIO_INFO_FORMAT(&self->in_info)));
        return GST_FLOW_NOT_NEGOTIATED;
    }

    if (!gst_buffer_map(buf, &map, GST_MAP_READWRITE))
        return GST_FLOW_ERROR;

    samples = map.size / sizeof(gint32);

    for (i = 0; i < samples; i++) {
        gint32 sample;
        gfloat normalized;
        guint8 *slot = map.data + (i * sizeof(gint32));

        memcpy(&sample, slot, sizeof(sample));
        normalized = (gfloat)sample / 2147483648.0f;
        memcpy(slot, &normalized, sizeof(normalized));
    }

    gst_buffer_unmap(buf, &map);
    return GST_FLOW_OK;
}

static void dlb_oamd_caps_feature_set_property(GObject *object,
                                               guint property_id,
                                               const GValue *value,
                                               GParamSpec *pspec) {
    DlbOAMDCapsFeature *self = DLB_OAMD_CAPS_FEATURE(object);

    switch (property_id) {
    case PROP_REMOVE:
        self->remove = g_value_get_boolean(value);
        gst_base_transform_reconfigure_src(GST_BASE_TRANSFORM(self));
        break;
    case PROP_FLOAT:
        self->convert_to_float = g_value_get_boolean(value);
        gst_base_transform_reconfigure_src(GST_BASE_TRANSFORM(self));
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void dlb_oamd_caps_feature_get_property(GObject *object,
                                               guint property_id, GValue *value,
                                               GParamSpec *pspec) {
    DlbOAMDCapsFeature *self = DLB_OAMD_CAPS_FEATURE(object);

    switch (property_id) {
    case PROP_REMOVE:
        g_value_set_boolean(value, self->remove);
        break;
    case PROP_FLOAT:
        g_value_set_boolean(value, self->convert_to_float);
        break;
    default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, pspec);
        break;
    }
}

static void dlb_oamd_caps_feature_class_init(DlbOAMDCapsFeatureClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);
    GstBaseTransformClass *base_transform_class =
        GST_BASE_TRANSFORM_CLASS(klass);

    gobject_class->set_property = dlb_oamd_caps_feature_set_property;
    gobject_class->get_property = dlb_oamd_caps_feature_get_property;

    g_object_class_install_property(
        gobject_class, PROP_REMOVE,
        g_param_spec_boolean(
            "remove", "Remove metadata caps feature",
            "Remove meta:DlbObjectAudioMeta from downstream caps", FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    g_object_class_install_property(
        gobject_class, PROP_FLOAT,
        g_param_spec_boolean(
            "float", "Convert samples to float",
            "Convert audio/x-raw S32LE samples to F32LE in-place", FALSE,
            G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);
    gst_element_class_set_static_metadata(
        element_class, "OAMD caps feature adapter", "Filter/Audio",
        "Adjusts DlbObjectAudioMeta caps features and optional S32LE->F32LE "
        "conversion",
        "Open source implementation");

    base_transform_class->transform_caps = dlb_oamd_caps_feature_transform_caps;
    base_transform_class->set_caps = dlb_oamd_caps_feature_set_caps;
    base_transform_class->transform_ip = dlb_oamd_caps_feature_transform_ip;
}

static void dlb_oamd_caps_feature_init(DlbOAMDCapsFeature *self) {
    self->remove = FALSE;
    self->convert_to_float = FALSE;
    self->have_in_info = FALSE;
    self->have_out_info = FALSE;
    gst_audio_info_init(&self->in_info);
    gst_audio_info_init(&self->out_info);
    gst_base_transform_set_in_place(GST_BASE_TRANSFORM(self), TRUE);
    gst_base_transform_set_gap_aware(GST_BASE_TRANSFORM(self), TRUE);
}
