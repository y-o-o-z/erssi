/*
 fe-log-search.c : /SEARCH in the autolog files, in a child process

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
*/

#include "module.h"
#include <irssi/src/fe-common/core/module-formats.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/core/commands.h>
#include <irssi/src/core/levels.h>
#include <irssi/src/core/misc.h>
#include <irssi/src/core/network.h>
#include <irssi/src/core/pidwait.h>
#include <irssi/src/core/settings.h>
#include <irssi/src/core/servers.h>
#include <irssi/src/core/channels.h>
#include <irssi/src/core/queries.h>
#ifdef HAVE_CAPSICUM
#include <irssi/src/core/capsicum.h>
#endif

#include <irssi/src/fe-common/core/fe-windows.h>
#include <irssi/src/fe-common/core/log-search.h>
#include <irssi/src/fe-common/core/printtext.h>

#if GLIB_CHECK_VERSION(2, 80, 0)
#include <glib-unix.h>
#endif

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#if !GLIB_CHECK_VERSION(2, 80, 0) && defined(__linux__)
#	include <sys/syscall.h>
#endif

#define SEARCH_LEVEL (MSGLEVEL_CLIENTCRAP | MSGLEVEL_NO_ACT)
#define SEARCH_DEFAULT_MAX 50
#define SEARCH_MAX_HITS 1000
#define SEARCH_MAX_FILES 5000
/* the child's whole run, in seconds */
#define SEARCH_TIMEOUT 120
/* longest line accepted from the child */
#define SEARCH_LINE_CAP (8 * 1024)
#define SEARCH_LABEL_MAX 1024

typedef struct {
	int pid;          /* 0 once reaped */
	int fd;           /* read end of the pipe */
	GIOChannel *chan;
	int tag;
	WINDOW_REC *win;  /* NULL once destroyed */
	char *text;       /* what was searched for, for the summary */
	int max_hits;
	GString *buf;     /* an incomplete line */
	GPtrArray *hits;  /* LOG_SEARCH_HIT * */
} SEARCH_REC;

static SEARCH_REC *search;

/* ---- the child ---- */

/* The directory the labels are relative to: the glob up to the component with
   its first wildcard, as log_search_files() walks it; the file's own directory
   without a wildcard. */
static char *glob_base(const char *glob)
{
	const char *w = glob + strcspn(glob, "*?");

	while (w > glob && *w != '/')
		w--;
	return g_strndup(glob, w - glob);
}

/* PATH relative to BASE (its base name if it is not below BASE), without the
   extension of its last component: "#test", "E2E/#test" */
static char *hit_label(const char *base, const char *path)
{
	gsize blen = strlen(base);
	const char *rel, *slash;
	char *label, *dot;

	if (blen > 0 && strncmp(path, base, blen) == 0 && path[blen] == '/' &&
	    path[blen + 1] != '\0')
		rel = path + blen + 1;
	else
		rel = (slash = strrchr(path, '/')) != NULL ? slash + 1 : path;
	label = g_strdup(rel);
	/* keep a hit line well below the parent's line cap; a cut UTF-8
	   sequence is made valid there */
	if (strlen(label) > SEARCH_LABEL_MAX)
		label[SEARCH_LABEL_MAX] = '\0';
	slash = strrchr(label, '/');
	slash = slash != NULL ? slash + 1 : label;
	dot = strrchr(slash, '.');
	if (dot != NULL && dot > slash)
		*dot = '\0';
	return label;
}

static void write_all(int fd, const char *data, gsize len)
{
	while (len > 0) {
		ssize_t n = write(fd, data, len);

		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			_exit(1);
		data += n;
		len -= n;
	}
}

static void child_log_noop(const char *domain, GLogLevelFlags level, const char *message,
                           gpointer data)
{
}

/* close every fd from LOWFD on */
static void close_from(int lowfd)
{
#if GLIB_CHECK_VERSION(2, 80, 0)
	g_closefrom(lowfd);
#else
	long n, max;

#	if defined(__linux__) && defined(SYS_close_range)
	if (syscall(SYS_close_range, (unsigned int)lowfd, ~0U, 0) == 0)
		return;
#	endif
	max = sysconf(_SC_OPEN_MAX);
	if (max < 0)
		max = 1024;
	for (n = lowfd; n < max; n++)
		close((int)n);
#endif
}

/* Leave everything of the parent alone: the default action for the signals it
   handles or ignores, no terminal, only the pipe (as fd 3) open. Closing the
   copies of the IRC sockets does not touch the connections. The parent forked
   with every signal blocked: the handlers are reset first, then the mask is
   cleared, so a SIGTERM sent right after fork() ends the child. */
static int child_detach(int fd)
{
	static const int sigs[] = { SIGHUP,  SIGINT,  SIGQUIT,  SIGTERM, SIGALRM,
		                    SIGPIPE, SIGUSR1, SIGUSR2,  SIGCHLD, SIGWINCH,
		                    SIGCONT, SIGTSTP, SIGTTIN,  SIGTTOU, SIGTRAP };
	sigset_t none;
	int null;
	gsize i;

	for (i = 0; i < G_N_ELEMENTS(sigs); i++)
		signal(sigs[i], SIG_DFL);
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);
	setsid();
	/* irssi's handler prints, runs signals and scripts and may write logs:
	   none of that in here */
	g_log_set_default_handler(child_log_noop, NULL);

	if (fd != 3) {
		if (dup2(fd, 3) != 3)
			_exit(1);
		fd = 3;
	}
	null = open("/dev/null", O_RDWR);
	if (null >= 0) {
		dup2(null, STDIN_FILENO);
		dup2(null, STDOUT_FILENO);
		dup2(null, STDERR_FILENO);
	}
	close_from(4);
	return fd;
}

/* Searches the files of GLOB and writes the hits (oldest first) and the
   summary into FD. Never returns. Files are searched one at a time, with what
   is left of the hit and byte budgets: the same result as one log_search_run()
   over all of them, but every hit gets a label with its path. */
static void G_GNUC_NORETURN search_child(int fd, const char *glob, Regex *re, int max_hits,
                                         gint64 max_bytes)
{
	GPtrArray *files, *per_file, *count;
	LOG_SEARCH_RESULT total;
	GString *out;
	gboolean truncated;
	char *base;
	int hits = 0, i;

	fd = child_detach(fd);
	alarm(SEARCH_TIMEOUT);

	memset(&total, 0, sizeof(total));
	files = log_search_files(glob, SEARCH_MAX_FILES, &truncated);
	total.file_limit = truncated;
	base = glob_base(glob);
	per_file = g_ptr_array_new();

	for (i = 0; i < (int)files->len && hits < max_hits; i++) {
		GPtrArray *one = g_ptr_array_new();
		LOG_SEARCH_RESULT r;
		char *label;
		guint j;

		g_ptr_array_add(one, files->pdata[i]);
		log_search_run(one, re, max_hits - hits, max_bytes - total.bytes, &r);
		g_ptr_array_free(one, TRUE);

		label = hit_label(base, files->pdata[i]);
		for (j = 0; j < r.hits->len; j++) {
			LOG_SEARCH_HIT *hit = r.hits->pdata[j];

			g_free(hit->file_label);
			hit->file_label = g_strdup(label);
		}
		g_free(label);

		total.files += r.files;
		total.bytes += r.bytes;
		hits += r.hits->len;
		g_ptr_array_add(per_file, r.hits);
		if (r.size_limit) {
			total.size_limit = TRUE;
			break;
		}
	}

	/* the files were newest first: the hits of the last one are the oldest */
	out = g_string_new(NULL);
	for (i = (int)per_file->len - 1; i >= 0; i--) {
		GPtrArray *fh = per_file->pdata[i];
		guint j;

		for (j = 0; j < fh->len; j++) {
			char *line = log_search_encode(fh->pdata[j]);

			g_string_append(out, line);
			g_free(line);
			if (out->len >= 32 * 1024) {
				write_all(fd, out->str, out->len);
				g_string_truncate(out, 0);
			}
		}
	}
	count = g_ptr_array_new();
	g_ptr_array_set_size(count, hits);
	total.hits = count;
	{
		char *line = log_search_encode_summary(&total);

		g_string_append(out, line);
		g_free(line);
	}
	write_all(fd, out->str, out->len);
	_exit(0);
}

/* ---- the parent ---- */

/* valid UTF-8 without control characters (C0, DEL, C1): a log file can hold
   anything */
static char *clean_text(const char *s)
{
	char *valid = g_utf8_make_valid(s, -1);
	GString *out = g_string_sized_new(strlen(valid));
	const char *p;

	for (p = valid; *p != '\0'; p = g_utf8_next_char(p)) {
		gunichar c = g_utf8_get_char(p);

		if (c < 0x20 || (c >= 0x7f && c < 0xa0))
			g_string_append_c(out, '?');
		else
			g_string_append_len(out, p, g_utf8_next_char(p) - p);
	}
	g_free(valid);
	return g_string_free(out, FALSE);
}

static WINDOW_REC *search_window(SEARCH_REC *rec)
{
	return rec->win != NULL ? rec->win : active_win;
}

/* Ends the search: no more input, the child (if it is still there) is sent
   SIGTERM, everything is freed. */
static void search_free(SEARCH_REC *rec)
{
	if (rec->tag != -1)
		g_source_remove(rec->tag);
	if (rec->pid > 0)
		kill(rec->pid, SIGTERM);
	g_io_channel_shutdown(rec->chan, FALSE, NULL);
	g_io_channel_unref(rec->chan);
	g_ptr_array_free(rec->hits, TRUE);
	g_string_free(rec->buf, TRUE);
	g_free(rec->text);
	if (search == rec)
		search = NULL;
	g_free(rec);
}

static void search_print(SEARCH_REC *rec, LOG_SEARCH_RESULT *res, int hits)
{
	WINDOW_REC *win = search_window(rec);
	char *size;
	const char *suffix;
	guint i;

	for (i = 0; i < rec->hits->len; i++) {
		LOG_SEARCH_HIT *hit = rec->hits->pdata[i];
		const char *date = hit->date;
		char *label, *d, *line;

		/* "Log opened Mon Oct 12 ..." -> "Mon Oct 12 ..." */
		if (g_str_has_prefix(date, "Log opened "))
			date += 11;
		else if (g_str_has_prefix(date, "Day changed "))
			date += 12;
		label = clean_text(hit->file_label);
		d = clean_text(date);
		line = clean_text(hit->line);
		printformat_window(win, SEARCH_LEVEL, TXT_SEARCH_HIT, label, d, line);
		g_free(label);
		g_free(d);
		g_free(line);
	}

	size = g_format_size_full(res->bytes, G_FORMAT_SIZE_IEC_UNITS);
	suffix = res->size_limit ? " - stopped at search_max_size" :
	         res->file_limit ? " - too many files" :
	                           "";
	printformat_window(win, SEARCH_LEVEL, TXT_SEARCH_SUMMARY, rec->text, hits, res->files, size,
	                   suffix);
	g_free(size);
}

static void search_failed(SEARCH_REC *rec)
{
	printformat_window(search_window(rec), MSGLEVEL_CLIENTERROR, TXT_SEARCH_FAILED, rec->text);
	search_free(rec);
}

/* One line from the child. FALSE: the search is over (REC is freed). */
static gboolean search_line(SEARCH_REC *rec, const char *line)
{
	if (line[0] == 'H') {
		LOG_SEARCH_HIT *hit = g_new0(LOG_SEARCH_HIT, 1);

		if ((int)rec->hits->len >= rec->max_hits || !log_search_decode(line, hit)) {
			g_free(hit);
			search_failed(rec);
			return FALSE;
		}
		g_ptr_array_add(rec->hits, hit);
		return TRUE;
	}
	if (line[0] == 'S') {
		LOG_SEARCH_RESULT res;
		int hits;

		memset(&res, 0, sizeof(res));
		if (!log_search_decode_summary(line, &hits, &res) || hits != (int)rec->hits->len) {
			search_failed(rec);
			return FALSE;
		}
		search_print(rec, &res, hits);
		/* the child is done and exits by itself; pidwait reaps it */
		rec->pid = 0;
		search_free(rec);
		return FALSE;
	}
	search_failed(rec);
	return FALSE;
}

static void search_input(SEARCH_REC *rec)
{
	char buf[4096];
	ssize_t n;
	char *nl;

	n = read(rec->fd, buf, sizeof(buf));
	if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
		return;
	if (n <= 0) {
		/* the child is gone without its summary */
		search_failed(rec);
		return;
	}
	g_string_append_len(rec->buf, buf, n);

	while ((nl = memchr(rec->buf->str, '\n', rec->buf->len)) != NULL) {
		gsize len = nl - rec->buf->str;

		if (len > SEARCH_LINE_CAP || memchr(rec->buf->str, '\0', len) != NULL) {
			search_failed(rec);
			return;
		}
		*nl = '\0';
		if (!search_line(rec, rec->buf->str))
			return;
		g_string_erase(rec->buf, 0, len + 1);
	}
	if (rec->buf->len > SEARCH_LINE_CAP)
		search_failed(rec);
}

static void search_stop(void)
{
	if (search != NULL)
		search_free(search);
}

/* A -network tag goes into the glob as it is: no glob characters, no path.
   A -target is escaped as autolog escapes it ('/', '*', '?' -> '_'), only
   "." and ".." are left to refuse. */
static const char *bad_scope_name(GHashTable *optlist)
{
	const char *tag = g_hash_table_lookup(optlist, "network");
	const char *target = g_hash_table_lookup(optlist, "target");

	if (tag != NULL && (*tag == '\0' || strpbrk(tag, "/\\*?[") != NULL ||
	                    strcmp(tag, ".") == 0 || strcmp(tag, "..") == 0))
		return tag;
	if (target != NULL &&
	    (*target == '\0' || strcmp(target, ".") == 0 || strcmp(target, "..") == 0))
		return target;
	return NULL;
}

/* the scope of the search: the tag and the target for the glob (NULL: any) */
static gboolean search_scope(GHashTable *optlist, const char **tag, const char **target)
{
	WI_ITEM_REC *item = active_win != NULL ? active_win->active : NULL;
	SERVER_REC *server = active_win != NULL ? active_win->active_server : NULL;
	const char *opt;

	*tag = *target = NULL;
	if (g_hash_table_lookup(optlist, "all") != NULL) {
		*target = g_hash_table_lookup(optlist, "target");
		return TRUE;
	}
	if ((opt = g_hash_table_lookup(optlist, "network")) != NULL) {
		SERVER_REC *net = server_find_tag(opt);

		/* the tag as the log path has it */
		*tag = net != NULL ? net->tag : opt;
		*target = g_hash_table_lookup(optlist, "target");
		return TRUE;
	}
	if ((opt = g_hash_table_lookup(optlist, "target")) != NULL) {
		*target = opt;
		*tag = server != NULL ? server->tag : NULL;
		return TRUE;
	}
	if (item == NULL || (!IS_CHANNEL(item) && !IS_QUERY(item)))
		return FALSE;
	*target = window_item_get_target(item);
	if (item->server != NULL)
		*tag = item->server->tag;
	else if (IS_QUERY(item))
		*tag = QUERY(item)->server_tag;
	return TRUE;
}

static void search_start(const char *glob, Regex *re, const char *text, int max_hits)
{
	SEARCH_REC *rec;
	gint64 max_bytes = settings_get_size("search_max_size");
	sigset_t all, old;
	int fds[2], pid;

	if (pipe(fds) == -1) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR, "SEARCH: pipe() failed: %s",
		          g_strerror(errno));
		return;
	}
	/* /UPGRADE execs a new erssi: the read end must not get into it */
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[1], F_SETFD, FD_CLOEXEC);

	/* the child starts with the parent's handlers (core's SIGTERM handler only
	   sets a flag): every signal waits until it has reset them */
	sigfillset(&all);
	sigprocmask(SIG_BLOCK, &all, &old);
	pid = fork();
	if (pid != 0)
		sigprocmask(SIG_SETMASK, &old, NULL);
	if (pid == -1) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR, "SEARCH: fork() failed: %s",
		          g_strerror(errno));
		close(fds[0]);
		close(fds[1]);
		return;
	}
	if (pid == 0) {
		close(fds[0]);
		search_child(fds[1], glob, re, max_hits, max_bytes);
	}

	close(fds[1]);
	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
	pidwait_add(pid);

	rec = g_new0(SEARCH_REC, 1);
	rec->pid = pid;
	rec->fd = fds[0];
	rec->chan = i_io_channel_new(fds[0]);
	rec->win = active_win;
	rec->text = g_strdup(text);
	rec->max_hits = max_hits;
	rec->buf = g_string_new(NULL);
	rec->hits = g_ptr_array_new_with_free_func((GDestroyNotify) log_search_hit_free);
	rec->tag = i_input_add(rec->chan, I_INPUT_READ, (GInputFunction) search_input, rec);
	search = rec;
}

/* SYNTAX: SEARCH [-all | -network <tag>] [-target <name>] [-max <n>] [-regexp] [-case] <text>
           SEARCH -stop */
static void cmd_search(const char *data)
{
	GHashTable *optlist;
	GError *error = NULL;
	Regex *re;
	char *text, *pattern, *glob;
	const char *opt, *tag, *target;
	void *free_arg;
	int max_hits = SEARCH_DEFAULT_MAX;

	if (!cmd_get_params(data, &free_arg, 1 | PARAM_FLAG_OPTIONS | PARAM_FLAG_GETREST, "search",
	                    &optlist, &text))
		return;

	if (g_hash_table_lookup(optlist, "stop") != NULL) {
		if (search == NULL) {
			printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_SEARCH_IDLE);
		} else {
			search_stop();
			printformat(NULL, NULL, MSGLEVEL_CLIENTNOTICE, TXT_SEARCH_STOPPED);
		}
		cmd_params_free(free_arg);
		return;
	}
	if (*text == '\0')
		cmd_param_error(CMDERR_NOT_ENOUGH_PARAMS);

#ifdef HAVE_CAPSICUM
	if (capsicum_enabled()) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTERROR, TXT_SEARCH_CAPSICUM);
		cmd_params_free(free_arg);
		return;
	}
#endif
	if (search != NULL) {
		printformat(NULL, NULL, MSGLEVEL_CLIENTERROR, TXT_SEARCH_BUSY);
		cmd_params_free(free_arg);
		return;
	}

	if ((opt = g_hash_table_lookup(optlist, "max")) != NULL) {
		gint64 n;

		if (!g_ascii_string_to_signed(opt, 10, 1, G_MAXINT64, &n, NULL)) {
			printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
			          "SEARCH: -max needs a positive number (more than %d is %d)",
			          SEARCH_MAX_HITS, SEARCH_MAX_HITS);
			cmd_params_free(free_arg);
			return;
		}
		max_hits = (int)MIN(n, SEARCH_MAX_HITS);
	}

	if ((opt = bad_scope_name(optlist)) != NULL) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "SEARCH: not a network tag or target name: %s", opt);
		cmd_params_free(free_arg);
		return;
	}
	if (!search_scope(optlist, &tag, &target)) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR,
		          "SEARCH: no channel or query here - use -target, -network or -all");
		cmd_params_free(free_arg);
		return;
	}

	pattern = g_hash_table_lookup(optlist, "regexp") != NULL ? g_strdup(text) :
	                                                          g_regex_escape_string(text, -1);
	re = i_regex_new(pattern,
	                 G_REGEX_OPTIMIZE |
	                     (g_hash_table_lookup(optlist, "case") != NULL ? 0 : G_REGEX_CASELESS),
	                 0, &error);
	g_free(pattern);
	if (re == NULL) {
		printtext(NULL, NULL, MSGLEVEL_CLIENTERROR, "SEARCH: bad regular expression: %s",
		          error != NULL ? error->message : "?");
		if (error != NULL)
			g_error_free(error);
		cmd_params_free(free_arg);
		return;
	}

	glob = log_search_glob(settings_get_str("autolog_path"), g_get_home_dir(), tag, target);
	search_start(glob, re, text, max_hits);
	g_free(glob);
	i_regex_unref(re);
	cmd_params_free(free_arg);
}

static void sig_pidwait(void *pid)
{
	if (search != NULL && search->pid == GPOINTER_TO_INT(pid))
		search->pid = 0; /* reaped: the number may be reused */
}

static void sig_window_destroyed(WINDOW_REC *window)
{
	if (search != NULL && search->win == window)
		search->win = NULL;
}

void fe_log_search_init(void)
{
	settings_add_size("log", "search_max_size", "512M");

	command_bind("search", NULL, (SIGNAL_FUNC) cmd_search);
	command_set_options("search", "all stop regexp case +network +target +max");

	signal_add("pidwait", (SIGNAL_FUNC) sig_pidwait);
	signal_add("window destroyed", (SIGNAL_FUNC) sig_window_destroyed);
}

void fe_log_search_deinit(void)
{
	/* SIGTERM, as fe-exec does; the pid stays in pidwait, so /UPGRADE hands
	   it to the new erssi with the session */
	search_stop();

	command_unbind("search", (SIGNAL_FUNC) cmd_search);

	signal_remove("pidwait", (SIGNAL_FUNC) sig_pidwait);
	signal_remove("window destroyed", (SIGNAL_FUNC) sig_window_destroyed);
}
