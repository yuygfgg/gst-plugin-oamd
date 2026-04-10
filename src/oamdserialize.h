#ifndef GST_PLUGIN_OAMD_OPEN_OAMDSERIALIZE_H
#define GST_PLUGIN_OAMD_OPEN_OAMDSERIALIZE_H

#include <gst/gst.h>

G_BEGIN_DECLS

#define DLB_TYPE_OAMD_SERIALIZE (dlb_oamd_serialize_get_type())
G_DECLARE_FINAL_TYPE(DlbOAMDSerialize, dlb_oamd_serialize, DLB, OAMD_SERIALIZE,
                     GstElement)

G_END_DECLS

#endif
