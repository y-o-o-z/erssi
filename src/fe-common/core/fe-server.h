#ifndef IRSSI_FE_COMMON_CORE_FE_SERVER_H
#define IRSSI_FE_COMMON_CORE_FE_SERVER_H

#include <irssi/src/fe-common/core/fe-windows.h>

/* window_close_disconnects: /WINDOW CLOSE collects the tags of the servers
   each window it closes shows, then hands them over once the windows are
   gone. A server none of the remaining windows shows is disconnected and its
   pending reconnect dropped. fe_server_windows_closed() frees the list. */
GSList *fe_server_window_tags(WINDOW_REC *window, GSList *tags);
void fe_server_windows_closed(GSList *tags);

#endif
