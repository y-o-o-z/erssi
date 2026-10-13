#ifndef IRSSI_FE_COMMON_CORE_FE_LOG_H
#define IRSSI_FE_COMMON_CORE_FE_LOG_H

/* Target name as autolog writes it into a file name: path and Windows
 * metacharacters (/ \ | * ? " < > :) become '_' and '%' is doubled so strftime()
 * leaves it alone. The caller g_free()s the result; case is not folded. */
char *fe_log_escape_target(const char *target);

#endif
