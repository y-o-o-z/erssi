#ifndef IRSSI_FE_COMMON_CORE_LOG_SEARCH_H
#define IRSSI_FE_COMMON_CORE_LOG_SEARCH_H

#include <glib.h>
#include <irssi/src/core/iregex.h>

typedef struct {
	char *file_label; /* base name of the log file */
	char *date;       /* last "Log opened ..." / "Day changed ..." text, or "" */
	char *line;       /* valid UTF-8, at most 400 characters (+ "…") */
} LOG_SEARCH_HIT;

typedef struct {
	GPtrArray *hits; /* LOG_SEARCH_HIT *, chronological; NULL after a summary decode */
	int files;       /* files actually scanned */
	gint64 bytes;    /* their total size */
	gboolean size_limit, file_limit;
} LOG_SEARCH_RESULT;

/* Turn an autolog_path setting into a glob for g_pattern_spec: "~" -> HOME,
 * $tag/${tag}/$1 -> TAG or "*", $0/${0} -> the escaped, lower-cased TARGET or
 * "*", any other $name -> "*", "%%" -> "%", any other "%X" -> "*". */
char *log_search_glob(const char *autolog_path, const char *home, const char *tag,
                      const char *target);

/* Regular files matching GLOB (absolute; a relative glob gives an empty result),
 * found below its longest wildcard-free directory prefix. The glob is matched per
 * path component, so '*' never crosses '/'; at most 4 wildcard components.
 * Symlinks inside the tree, directories or files, are never followed (the
 * wildcard-free prefix itself may be a link). Newest mtime first, at most MAX_FILES.
 * Array of g_malloc()ed paths. *TRUNCATED (if given) is set when matching files
 * were dropped (MAX_FILES, or an internal cap on matches/directory entries; the
 * newest are kept for the match cap, but not guaranteed after the entry cap). */
GPtrArray *log_search_files(const char *glob, int max_files, gboolean *truncated);

/* Cap on matched files kept while walking (newest win); lowered by tests only. */
extern int log_search_match_cap;

/* Scan FILES (as returned above) in order. Initialises RES. Keeps the newest
 * MAX_HITS hits overall (chronological); stops before the next file once MAX_HITS
 * were collected. A file that does not fit the remaining MAX_BYTES is searched from
 * its last bytes (starting at a line start, date "" there; only if at least 64 KiB
 * of budget remain), size_limit is set and the run ends; files is counted for it.
 * NUL bytes inside a line are read as spaces. */
void log_search_run(GPtrArray *files, Regex *re, int max_hits, gint64 max_bytes,
                    LOG_SEARCH_RESULT *res);

void log_search_hit_free(LOG_SEARCH_HIT *hit);
void log_search_result_clear(LOG_SEARCH_RESULT *res);

/* "H\t<label>\t<date>\t<line>\n"; TAB, CR and LF inside fields become spaces. */
char *log_search_encode(const LOG_SEARCH_HIT *hit);
gboolean log_search_decode(const char *line, LOG_SEARCH_HIT *hit);

/* "S\t<hits>\t<files>\t<bytes>\t<size_limit>\t<file_limit>\n" */
char *log_search_encode_summary(const LOG_SEARCH_RESULT *r);
gboolean log_search_decode_summary(const char *line, int *hits, LOG_SEARCH_RESULT *r);

#endif
