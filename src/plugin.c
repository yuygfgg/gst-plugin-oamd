#include <gst/gst.h>

#include "oamdcapsfeature.h"
#include "oamdserialize.h"

#ifndef PACKAGE
#define PACKAGE "gst-plugin-oamd"
#endif

#ifndef PACKAGE_NAME
#define PACKAGE_NAME "gst-plugin-oamd"
#endif

#ifndef GST_PACKAGE_ORIGIN
#define GST_PACKAGE_ORIGIN "https://github.com/yuygfgg/gst-plugin-oamd"
#endif

static gboolean plugin_init(GstPlugin *plugin) {
    if (!gst_element_register(plugin, "oamdcapsfeature", GST_RANK_NONE,
                              DLB_TYPE_OAMD_CAPS_FEATURE))
        return FALSE;

    if (!gst_element_register(plugin, "oamdserialize", GST_RANK_NONE,
                              DLB_TYPE_OAMD_SERIALIZE))
        return FALSE;

    return TRUE;
}

GST_PLUGIN_DEFINE(1, 18, dlboamdmod, "OAMD helper plugin", plugin_init, "0.1.0",
                  "MPL", PACKAGE_NAME, GST_PACKAGE_ORIGIN)
