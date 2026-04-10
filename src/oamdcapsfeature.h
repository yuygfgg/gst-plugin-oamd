#ifndef GST_PLUGIN_OAMD_OPEN_OAMDCAPSFEATURE_H
#define GST_PLUGIN_OAMD_OPEN_OAMDCAPSFEATURE_H

#include <gst/base/gstbasetransform.h>

G_BEGIN_DECLS

#define DLB_TYPE_OAMD_CAPS_FEATURE (dlb_oamd_caps_feature_get_type())
G_DECLARE_FINAL_TYPE(DlbOAMDCapsFeature, dlb_oamd_caps_feature, DLB,
                     OAMD_CAPS_FEATURE, GstBaseTransform)

G_END_DECLS

#endif
