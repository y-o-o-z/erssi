#ifndef IRSSI_FE_COMMON_IRC_DCC_FE_DCC_H
#define IRSSI_FE_COMMON_IRC_DCC_FE_DCC_H

char *dcc_get_size_str(uoff_t size);
void dcc_list_print_file(FILE_DCC_REC *dcc);
/* CHAT, SEND, GET, and SCHAT, SSEND, SGET for SDCC (TLS) */
const char *dcc_type_label(DCC_REC *dcc);

#endif
