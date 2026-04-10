#include <glib.h>

#ifdef g_once_init_enter_pointer
#undef g_once_init_enter_pointer
#endif

#ifdef g_once_init_leave_pointer
#undef g_once_init_leave_pointer
#endif

gboolean g_once_init_enter_pointer(void *location) {
    return (g_once_init_enter)(location);
}

void g_once_init_leave_pointer(void *location, gpointer result) {
    (g_once_init_leave)(location, (gsize)result);
}
