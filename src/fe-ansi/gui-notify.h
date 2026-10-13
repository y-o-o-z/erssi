#ifndef IRSSI_FE_ANSI_GUI_NOTIFY_H
#define IRSSI_FE_ANSI_GUI_NOTIFY_H

/* Desktop notifications through the terminal (OSC 9 / OSC 777) for the
 * "mention" signal, sent while the terminal reports it is not focused.
 * Settings term_notify and term_notify_text. */
void gui_notify_init(void);
void gui_notify_deinit(void);

#endif
