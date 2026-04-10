#include "oamdserialize.h"

#include <gst/audio/audio.h>
#include <gst/gst.h>

#include "dlbaudiometa.h"
#include "oamd_serializer_api.h"

struct _DlbOAMDSerialize {
    GstElement parent;
    GstPad *sinkpad;
    GstPad *srcpad;
    GstAudioInfo audio_info;
    gboolean have_audio_info;
    gboolean src_caps_sent;
    gboolean have_next_sample_pos;
    guint64 next_sample_pos;
    OAMDSerializerState *serializer;
};

G_DEFINE_TYPE(DlbOAMDSerialize, dlb_oamd_serialize, GST_TYPE_ELEMENT)

static GstStaticPadTemplate sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink", GST_PAD_SINK, GST_PAD_ALWAYS,
    GST_STATIC_CAPS(
        "audio/x-raw(" DLB_CAPS_FEATURE_META_OBJECT_AUDIO_META "), "
        "format = (string) { " GST_AUDIO_NE(S16) ", " GST_AUDIO_NE(
            S32) ", " GST_AUDIO_NE(F32) ", " GST_AUDIO_NE(F64) " }, "
                                                               "channels = "
                                                               "(int) [ 1, 64 "
                                                               "], "
                                                               "rate = (int) [ "
                                                               "1, MAX ], "
                                                               "layout = "
                                                               "(string) "
                                                               "interleaved"));

static GstStaticPadTemplate src_template = GST_STATIC_PAD_TEMPLATE(
    "src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("text/x-raw"));

static void dlb_oamd_serialize_reset_state(DlbOAMDSerialize *self) {
    self->have_audio_info = FALSE;
    self->src_caps_sent = FALSE;
    self->have_next_sample_pos = FALSE;
    self->next_sample_pos = 0;
    gst_audio_info_init(&self->audio_info);

    if (self->serializer != NULL)
        oamd_serializer_reset(self->serializer);
}

static gboolean dlb_oamd_serialize_push_src_caps(DlbOAMDSerialize *self) {
    GstCaps *caps;
    gboolean ok;

    if (self->src_caps_sent)
        return TRUE;

    caps = gst_caps_new_empty_simple("text/x-raw");
    ok = gst_pad_push_event(self->srcpad, gst_event_new_caps(caps));
    gst_caps_unref(caps);

    self->src_caps_sent = ok;
    return ok;
}

static guint64 dlb_oamd_serialize_get_base_sample_pos(DlbOAMDSerialize *self,
                                                      GstBuffer *buffer) {
    if (GST_BUFFER_OFFSET_IS_VALID(buffer))
        return GST_BUFFER_OFFSET(buffer);

    if (self->have_next_sample_pos &&
        !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT)) {
        return self->next_sample_pos;
    }

    if (GST_BUFFER_PTS_IS_VALID(buffer) && self->have_audio_info &&
        GST_AUDIO_INFO_RATE(&self->audio_info) > 0) {
        return gst_util_uint64_scale_round(
            GST_BUFFER_PTS(buffer), GST_AUDIO_INFO_RATE(&self->audio_info),
            GST_SECOND);
    }

    return self->next_sample_pos;
}

static guint64 dlb_oamd_serialize_get_buffer_samples(DlbOAMDSerialize *self,
                                                     GstBuffer *buffer) {
    gsize bpf;

    if (!self->have_audio_info)
        return 0;

    bpf = GST_AUDIO_INFO_BPF(&self->audio_info);
    if (bpf == 0)
        return 0;

    return gst_buffer_get_size(buffer) / bpf;
}

typedef struct {
    const GstMetaInfo *info;
    GPtrArray *metas;
} CollectMetaContext;

static gboolean collect_oamd_meta(GstBuffer *buffer, GstMeta **meta,
                                  gpointer user_data) {
    CollectMetaContext *ctx = (CollectMetaContext *)user_data;

    (void)buffer;

    if (*meta != NULL && (*meta)->info == ctx->info)
        g_ptr_array_add(ctx->metas, *meta);

    return TRUE;
}

static gint compare_oamd_meta_seqnum(gconstpointer a, gconstpointer b) {
    return gst_meta_compare_seqnum((const GstMeta *)a, (const GstMeta *)b);
}

static GstFlowReturn push_text_buffer(DlbOAMDSerialize *self, const gchar *text,
                                      guint64 sample_pos) {
    GstBuffer *outbuf;
    GstMapInfo map;
    gsize len = strlen(text);

    if (len == 0)
        return GST_FLOW_OK;

    outbuf = gst_buffer_new_allocate(NULL, len, NULL);
    if (outbuf == NULL)
        return GST_FLOW_ERROR;

    if (!gst_buffer_map(outbuf, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(outbuf);
        return GST_FLOW_ERROR;
    }

    memcpy(map.data, text, len);
    gst_buffer_unmap(outbuf, &map);

    GST_BUFFER_OFFSET(outbuf) = sample_pos;
    if (self->have_audio_info && GST_AUDIO_INFO_RATE(&self->audio_info) > 0) {
        GST_BUFFER_PTS(outbuf) = gst_util_uint64_scale_round(
            sample_pos, GST_SECOND, GST_AUDIO_INFO_RATE(&self->audio_info));
    }

    return gst_pad_push(self->srcpad, outbuf);
}

static GstFlowReturn dlb_oamd_serialize_chain(GstPad *pad, GstObject *parent,
                                              GstBuffer *buffer) {
    DlbOAMDSerialize *self = DLB_OAMD_SERIALIZE(parent);
    const GstMetaInfo *info;
    CollectMetaContext ctx;
    guint64 base_sample_pos;
    guint64 buffer_samples;
    GstFlowReturn flow = GST_FLOW_OK;
    guint i;

    (void)pad;

    if (!self->have_audio_info) {
        gst_buffer_unref(buffer);
        GST_ERROR_OBJECT(self, "received data before CAPS");
        return GST_FLOW_NOT_NEGOTIATED;
    }

    if (!dlb_oamd_serialize_push_src_caps(self)) {
        gst_buffer_unref(buffer);
        GST_ERROR_OBJECT(self, "failed to push text/x-raw caps");
        return GST_FLOW_NOT_NEGOTIATED;
    }

    info = gst_meta_get_info("DlbObjectAudioMeta");
    if (info == NULL) {
        gst_buffer_unref(buffer);
        GST_ERROR_OBJECT(self, "DlbObjectAudioMeta is not registered");
        return GST_FLOW_ERROR;
    }

    ctx.info = info;
    ctx.metas = g_ptr_array_new();
    gst_buffer_foreach_meta(buffer, collect_oamd_meta, &ctx);
    g_ptr_array_sort(ctx.metas, compare_oamd_meta_seqnum);

    base_sample_pos = dlb_oamd_serialize_get_base_sample_pos(self, buffer);

    for (i = 0; i < ctx.metas->len; i++) {
        const DlbObjectAudioMeta *meta =
            (const DlbObjectAudioMeta *)g_ptr_array_index(ctx.metas, i);
        gchar *error = NULL;
        gchar *yaml;
        guint64 sample_pos;

        if (meta->payload == NULL || meta->size == 0)
            continue;

        sample_pos = base_sample_pos + meta->offset;
        yaml = oamd_serializer_process_payload(
            self->serializer, (const guint8 *)meta->payload, meta->size,
            GST_AUDIO_INFO_RATE(&self->audio_info), sample_pos, &error);

        if (yaml == NULL) {
            GST_ELEMENT_ERROR(self, STREAM, FAILED,
                              ("Failed to serialize OAMD metadata"),
                              ("%s", error != NULL ? error : "unknown error"));
            if (error != NULL)
                oamd_string_free(error);
            flow = GST_FLOW_ERROR;
            break;
        }

        if (error != NULL)
            oamd_string_free(error);

        flow = push_text_buffer(self, yaml, sample_pos);
        oamd_string_free(yaml);

        if (flow != GST_FLOW_OK)
            break;
    }

    buffer_samples = dlb_oamd_serialize_get_buffer_samples(self, buffer);
    self->next_sample_pos = base_sample_pos + buffer_samples;
    self->have_next_sample_pos = TRUE;

    g_ptr_array_free(ctx.metas, TRUE);
    gst_buffer_unref(buffer);

    return flow;
}

static gboolean dlb_oamd_serialize_sink_event(GstPad *pad, GstObject *parent,
                                              GstEvent *event) {
    DlbOAMDSerialize *self = DLB_OAMD_SERIALIZE(parent);

    (void)pad;

    switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_CAPS: {
        GstCaps *caps = NULL;

        gst_event_parse_caps(event, &caps);
        if (caps != NULL)
            self->have_audio_info =
                gst_audio_info_from_caps(&self->audio_info, caps);
        self->src_caps_sent = FALSE;

        gst_event_unref(event);
        return self->have_audio_info && dlb_oamd_serialize_push_src_caps(self);
    }
    case GST_EVENT_FLUSH_STOP:
        dlb_oamd_serialize_reset_state(self);
        break;
    default:
        break;
    }

    return gst_pad_event_default(pad, parent, event);
}

static GstStateChangeReturn
dlb_oamd_serialize_change_state(GstElement *element,
                                GstStateChange transition) {
    DlbOAMDSerialize *self = DLB_OAMD_SERIALIZE(element);
    GstStateChangeReturn ret;

    if (transition == GST_STATE_CHANGE_READY_TO_PAUSED)
        dlb_oamd_serialize_reset_state(self);

    ret = GST_ELEMENT_CLASS(dlb_oamd_serialize_parent_class)
              ->change_state(element, transition);

    if (ret == GST_STATE_CHANGE_FAILURE)
        return ret;

    if (transition == GST_STATE_CHANGE_PAUSED_TO_READY)
        dlb_oamd_serialize_reset_state(self);

    return ret;
}

static void dlb_oamd_serialize_finalize(GObject *object) {
    DlbOAMDSerialize *self = DLB_OAMD_SERIALIZE(object);

    if (self->serializer != NULL) {
        oamd_serializer_free(self->serializer);
        self->serializer = NULL;
    }

    G_OBJECT_CLASS(dlb_oamd_serialize_parent_class)->finalize(object);
}

static void dlb_oamd_serialize_class_init(DlbOAMDSerializeClass *klass) {
    GObjectClass *gobject_class = G_OBJECT_CLASS(klass);
    GstElementClass *element_class = GST_ELEMENT_CLASS(klass);

    gobject_class->finalize = dlb_oamd_serialize_finalize;
    element_class->change_state = dlb_oamd_serialize_change_state;

    gst_element_class_add_static_pad_template(element_class, &sink_template);
    gst_element_class_add_static_pad_template(element_class, &src_template);
    gst_element_class_set_static_metadata(
        element_class, "OAMD serializer", "Formatter",
        "Serializes DlbObjectAudioMeta payloads to text/x-raw YAML events",
        "Open source implementation");
}

static void dlb_oamd_serialize_init(DlbOAMDSerialize *self) {
    self->sinkpad = gst_pad_new_from_static_template(&sink_template, "sink");
    self->srcpad = gst_pad_new_from_static_template(&src_template, "src");
    self->serializer = oamd_serializer_new();

    gst_pad_set_chain_function(self->sinkpad,
                               GST_DEBUG_FUNCPTR(dlb_oamd_serialize_chain));
    gst_pad_set_event_function(
        self->sinkpad, GST_DEBUG_FUNCPTR(dlb_oamd_serialize_sink_event));

    gst_element_add_pad(GST_ELEMENT(self), self->sinkpad);
    gst_element_add_pad(GST_ELEMENT(self), self->srcpad);

    dlb_oamd_serialize_reset_state(self);
}
