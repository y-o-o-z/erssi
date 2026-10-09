#ifndef IRSSI_E2E_FORMATS_H
#define IRSSI_E2E_FORMATS_H

#include <irssi/src/fe-common/core/formats.h>

/* must be in sync with e2e_formats[] */
enum {
	TXT_E2E_MODULE_NAME,

	TXT_E2E_MESSAGE,
	TXT_E2E_DEBUG
};

extern FORMAT_REC e2e_formats[];

#endif
