#ifndef GST_PLUGIN_OAMD_OPEN_OAMD_SERIALIZER_API_H
#define GST_PLUGIN_OAMD_OPEN_OAMD_SERIALIZER_API_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct _OAMDSerializerState OAMDSerializerState;

OAMDSerializerState* oamd_serializer_new(void);
void oamd_serializer_reset(OAMDSerializerState* state);
void oamd_serializer_free(OAMDSerializerState* state);
gchar* oamd_serializer_process_payload(OAMDSerializerState* state,
                                       const guint8* payload, gsize payload_len,
                                       guint32 sample_rate, guint64 sample_pos,
                                       gchar** error_out);
void oamd_string_free(gchar* value);

G_END_DECLS

#endif
