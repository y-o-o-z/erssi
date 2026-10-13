/*
 * Tests for the bounded autolog search (log-search.c).
 */
#include <irssi/src/common.h>
#include <irssi/src/core/iregex.h>
#include <irssi/src/fe-common/core/log-search.h>

#include <glib/gstdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

static char *tmp_dir;

static void tmp_setup(void)
{
	GError *err = NULL;

	tmp_dir = g_dir_make_tmp("log-search-XXXXXX", &err);
	g_assert_no_error(err);
}

static void rm_tree(const char *path)
{
	GDir *dir = g_dir_open(path, 0, NULL);
	const char *name;

	if (dir == NULL) {
		g_remove(path);
		return;
	}
	while ((name = g_dir_read_name(dir)) != NULL) {
		char *child = g_build_filename(path, name, NULL);

		if (g_file_test(child, G_FILE_TEST_IS_DIR) &&
		    !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
			rm_tree(child);
		else
			g_remove(child);
		g_free(child);
	}
	g_dir_close(dir);
	g_rmdir(path);
}

static void tmp_teardown(void)
{
	rm_tree(tmp_dir);
	g_free(tmp_dir);
	tmp_dir = NULL;
}

static char *write_file(const char *name, const char *content, gsize len)
{
	char *path = g_build_filename(tmp_dir, name, NULL);
	GError *err = NULL;

	g_file_set_contents(path, content, len, &err);
	g_assert_no_error(err);
	return path;
}

static void set_mtime(const char *path, time_t t)
{
	struct utimbuf ub = {t, t};

	g_assert_cmpint(utime(path, &ub), ==, 0);
}

static Regex *re_new(const char *pattern)
{
	GError *err = NULL;
	Regex *re = i_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, &err);

	g_assert_no_error(err);
	return re;
}

static GPtrArray *one_file(const char *path)
{
	GPtrArray *a = g_ptr_array_new_with_free_func(g_free);

	g_ptr_array_add(a, g_strdup(path));
	return a;
}

static void test_glob(void)
{
	char *g;

	g = log_search_glob("~/irclogs/$tag/$0.log", "/home/u", "ircnet", "#Erssi");
	g_assert_cmpstr(g, ==, "/home/u/irclogs/ircnet/#erssi.log");
	g_free(g);
	g = log_search_glob("~/l/$tag/$0/%Y-%m.log", "/h", NULL, NULL);
	g_assert_cmpstr(g, ==, "/h/l/*/*/*-*.log");
	g_free(g);
	g = log_search_glob("/l/${tag}/$1-$0-$foo%%.log", "/h", "t", "a/b");
	g_assert_cmpstr(g, ==, "/l/t/t-a_b-*%.log");
	g_free(g);
}

static void test_files_order(void)
{
	GPtrArray *files;
	char *a, *b, *c, *glob;
	gboolean trunc;

	tmp_setup();
	a = write_file("a.log", "x\n", 2);
	b = write_file("b.log", "x\n", 2);
	c = write_file("c.log", "x\n", 2);
	g_free(write_file("skip.txt", "x\n", 2));
	set_mtime(a, 100);
	set_mtime(b, 300);
	set_mtime(c, 200);
	glob = g_build_filename(tmp_dir, "*.log", NULL);

	files = log_search_files(glob, 10, &trunc);
	g_assert_cmpuint(files->len, ==, 3);
	g_assert_false(trunc);
	g_assert_cmpstr(files->pdata[0], ==, b);
	g_assert_cmpstr(files->pdata[1], ==, c);
	g_assert_cmpstr(files->pdata[2], ==, a);
	g_ptr_array_free(files, TRUE);

	files = log_search_files(glob, 2, &trunc);
	g_assert_cmpuint(files->len, ==, 2);
	g_assert_true(trunc);
	g_ptr_array_free(files, TRUE);

	g_free(glob);
	g_free(a);
	g_free(b);
	g_free(c);
	tmp_teardown();
}

static void test_files_safety(void)
{
	GPtrArray *files;
	char *glob, *outside, *real, *p;
	gboolean trunc;

	tmp_setup();
	outside = g_build_filename(tmp_dir, "outside", NULL);
	g_assert_cmpint(g_mkdir_with_parents(outside, 0700), ==, 0);
	p = g_build_filename(outside, "secret.log", NULL);
	g_file_set_contents(p, "x\n", 2, NULL);
	g_free(p);

	real = g_build_filename(tmp_dir, "base", "net", NULL);
	g_assert_cmpint(g_mkdir_with_parents(real, 0700), ==, 0);
	p = g_build_filename(real, "ok.log", NULL);
	g_file_set_contents(p, "x\n", 2, NULL);
	g_free(p);
	/* symlinked directory and symlinked file inside the base; FIFO; too deep */
	p = g_build_filename(tmp_dir, "base", "link", NULL);
	g_assert_cmpint(symlink(outside, p), ==, 0);
	g_free(p);
	p = g_build_filename(tmp_dir, "base", "net", "lnk.log", NULL);
	g_assert_cmpint(symlink("ok.log", p), ==, 0);
	g_free(p);
	p = g_build_filename(tmp_dir, "base", "net", "fifo.log", NULL);
	g_assert_cmpint(mkfifo(p, 0600), ==, 0);
	g_free(p);
	p = g_build_filename(tmp_dir, "base", "a", "b", "c", "d", "e", NULL);
	g_assert_cmpint(g_mkdir_with_parents(p, 0700), ==, 0);
	g_free(p);
	p = g_build_filename(tmp_dir, "base", "a", "b", "c", "d", "e", "deep.log", NULL);
	g_file_set_contents(p, "x\n", 2, NULL);
	g_free(p);

	glob = g_build_filename(tmp_dir, "base", "*", "*", NULL);
	files = log_search_files(glob, 50, &trunc);
	g_assert_cmpuint(files->len, ==, 1);
	p = g_build_filename(real, "ok.log", NULL);
	g_assert_cmpstr(files->pdata[0], ==, p);
	g_free(p);
	g_ptr_array_free(files, TRUE);
	g_free(glob);

	/* '*' stays inside one path component; more than 4 wildcard levels refused */
	glob = g_build_filename(tmp_dir, "base", "*.log", NULL);
	files = log_search_files(glob, 50, &trunc);
	g_assert_cmpuint(files->len, ==, 0);
	g_ptr_array_free(files, TRUE);
	g_free(glob);
	glob = g_build_filename(tmp_dir, "base", "*", "*", "*", "*", "*", NULL);
	files = log_search_files(glob, 50, &trunc);
	g_assert_cmpuint(files->len, ==, 0);
	g_ptr_array_free(files, TRUE);
	g_free(glob);
	/* relative globs are refused, not resolved against the cwd or "/" */
	files = log_search_files("base/*/*", 50, &trunc);
	g_assert_cmpuint(files->len, ==, 0);
	g_ptr_array_free(files, TRUE);

	/* a FIFO handed straight to the scanner must not block */
	{
		Regex *re = re_new("x");
		LOG_SEARCH_RESULT res;

		p = g_build_filename(real, "fifo.log", NULL);
		files = one_file(p);
		log_search_run(files, re, 5, 1000000, &res);
		g_assert_cmpuint(res.hits->len, ==, 0);
		g_assert_cmpint(res.files, ==, 0);
		log_search_result_clear(&res);
		g_ptr_array_free(files, TRUE);
		g_free(p);
		i_regex_unref(re);
	}

	g_free(outside);
	g_free(real);
	tmp_teardown();
}

static LOG_SEARCH_HIT *hit_at(const LOG_SEARCH_RESULT *res, guint i)
{
	g_assert_cmpuint(i, <, res->hits->len);
	return res->hits->pdata[i];
}

static void test_scan_dates(void)
{
	const char *txt = "--- Log opened Fri Oct 09 10:00:00 2026\n"
	                  "10:00 <bob> hello world\n"
	                  "--- Day changed Sat Oct 10 2026\n"
	                  "11:00 <bob> HELLO again\n"
	                  "11:01 <carol> nothing\n";
	char *path = NULL;
	GPtrArray *files;
	LOG_SEARCH_RESULT res;
	Regex *re = re_new("hello");

	tmp_setup();
	path = write_file("t.log", txt, strlen(txt));
	files = one_file(path);
	log_search_run(files, re, 10, 1000000, &res);
	g_assert_cmpuint(res.hits->len, ==, 2);
	g_assert_cmpstr(hit_at(&res, 0)->date, ==, "Log opened Fri Oct 09 10:00:00 2026");
	g_assert_cmpstr(hit_at(&res, 1)->date, ==, "Day changed Sat Oct 10 2026");
	g_assert_cmpstr(hit_at(&res, 1)->line, ==, "11:00 <bob> HELLO again");
	g_assert_cmpstr(hit_at(&res, 1)->file_label, ==, "t.log");
	g_assert_cmpint(res.files, ==, 1);
	g_assert_cmpint(res.bytes, ==, (gint64)strlen(txt));
	log_search_result_clear(&res);
	g_ptr_array_free(files, TRUE);
	g_free(path);
	i_regex_unref(re);
	tmp_teardown();
}

static void test_ring(void)
{
	GString *s = g_string_new(NULL);
	char *path, *expect;
	GPtrArray *files;
	LOG_SEARCH_RESULT res;
	Regex *re = re_new("^n[0-9]+$");
	int i;

	tmp_setup();
	for (i = 1; i <= 100; i++)
		g_string_append_printf(s, "n%d\n", i);
	path = write_file("r.log", s->str, s->len);
	files = one_file(path);
	log_search_run(files, re, 5, 1000000, &res);
	g_assert_cmpuint(res.hits->len, ==, 5);
	for (i = 0; i < 5; i++) {
		expect = g_strdup_printf("n%d", 96 + i);
		g_assert_cmpstr(hit_at(&res, i)->line, ==, expect);
		g_free(expect);
	}
	log_search_result_clear(&res);
	g_ptr_array_free(files, TRUE);
	g_free(path);
	g_string_free(s, TRUE);
	i_regex_unref(re);
	tmp_teardown();
}

static void test_newest_file_first_stops(void)
{
	const char *five = "hello 1\nhello 2\nhello 3\nhello 4\nhello 5\n";
	char *newer, *older;
	GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
	LOG_SEARCH_RESULT res;
	Regex *re = re_new("hello");

	tmp_setup();
	newer = write_file("new.log", five, strlen(five));
	older = write_file("old.log", five, strlen(five));
	g_ptr_array_add(files, newer);
	g_ptr_array_add(files, older);
	log_search_run(files, re, 5, 1000000, &res);
	g_assert_cmpuint(res.hits->len, ==, 5);
	g_assert_cmpint(res.files, ==, 1);
	g_assert_false(res.size_limit);
	log_search_result_clear(&res);

	/* with room left the older file is scanned and comes first */
	log_search_run(files, re, 8, 1000000, &res);
	g_assert_cmpuint(res.hits->len, ==, 8);
	g_assert_cmpint(res.files, ==, 2);
	g_assert_cmpstr(hit_at(&res, 0)->file_label, ==, "old.log");
	g_assert_cmpstr(hit_at(&res, 2)->line, ==, "hello 5");
	g_assert_cmpstr(hit_at(&res, 3)->file_label, ==, "new.log");
	g_assert_cmpstr(hit_at(&res, 7)->line, ==, "hello 5");
	log_search_result_clear(&res);

	g_ptr_array_free(files, TRUE);
	i_regex_unref(re);
	tmp_teardown();
}

static void test_size_limit(void)
{
	char blob[4096];
	char *a, *b;
	GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
	LOG_SEARCH_RESULT res;
	Regex *re = re_new("zzz");

	memset(blob, 'a', sizeof(blob));
	tmp_setup();
	a = write_file("a.log", blob, sizeof(blob));
	b = write_file("b.log", blob, sizeof(blob));
	g_ptr_array_add(files, a);
	g_ptr_array_add(files, b);
	log_search_run(files, re, 5, 5000, &res);
	g_assert_true(res.size_limit);
	g_assert_false(res.file_limit);
	g_assert_cmpint(res.files, ==, 1);
	g_assert_cmpint(res.bytes, ==, 4096);
	log_search_result_clear(&res);
	g_ptr_array_free(files, TRUE);
	i_regex_unref(re);
	tmp_teardown();
}

static void test_long_line(void)
{
	GString *s = g_string_new("hello ");
	char *path;
	GPtrArray *files;
	LOG_SEARCH_RESULT res;
	Regex *re = re_new("hello");
	const LOG_SEARCH_HIT *h;

	tmp_setup();
	while (s->len < 200 * 1024)
		g_string_append_c(s, 'x');
	g_string_append(s, "\nnext hello\n");
	path = write_file("l.log", s->str, s->len);
	files = one_file(path);
	log_search_run(files, re, 10, 1000000, &res);
	g_assert_cmpuint(res.hits->len, ==, 2);
	h = hit_at(&res, 0);
	g_assert_true(g_utf8_validate(h->line, -1, NULL));
	g_assert_cmpint(g_utf8_strlen(h->line, -1), ==, 401);
	g_assert_true(g_str_has_suffix(h->line, "\xe2\x80\xa6"));
	g_assert_cmpstr(hit_at(&res, 1)->line, ==, "next hello");
	log_search_result_clear(&res);
	g_ptr_array_free(files, TRUE);
	g_free(path);
	g_string_free(s, TRUE);
	i_regex_unref(re);
	tmp_teardown();
}

static void test_invalid_utf8(void)
{
	const char *txt = "\xff\xfe hello\n";
	char *path;
	GPtrArray *files;
	LOG_SEARCH_RESULT res;
	Regex *re = re_new("hello");

	tmp_setup();
	path = write_file("u.log", txt, strlen(txt));
	files = one_file(path);
	log_search_run(files, re, 10, 1000000, &res);
	g_assert_cmpuint(res.hits->len, ==, 1);
	g_assert_true(g_utf8_validate(hit_at(&res, 0)->line, -1, NULL));
	log_search_result_clear(&res);
	g_ptr_array_free(files, TRUE);
	g_free(path);
	i_regex_unref(re);
	tmp_teardown();
}

static void test_percent_literal(void)
{
	const char *txt = "100%done hello\n";
	char *path;
	GPtrArray *files;
	LOG_SEARCH_RESULT res;
	Regex *re = re_new("hello");

	tmp_setup();
	path = write_file("p.log", txt, strlen(txt));
	files = one_file(path);
	log_search_run(files, re, 10, 1000000, &res);
	g_assert_cmpuint(res.hits->len, ==, 1);
	g_assert_nonnull(strstr(hit_at(&res, 0)->line, "100%done"));
	log_search_result_clear(&res);
	g_ptr_array_free(files, TRUE);
	g_free(path);
	i_regex_unref(re);
	tmp_teardown();
}

static void test_files_cap_keeps_newest(void)
{
	GPtrArray *files;
	char *glob, *p, *name;
	gboolean trunc = FALSE;
	int i, saved = log_search_match_cap;

	tmp_setup();
	for (i = 0; i < 9; i++) {
		name = g_strdup_printf("f%d.log", i);
		p = write_file(name, "x\n", 2);
		set_mtime(p, 1000 + (i * 7) % 9); /* mtimes scrambled w.r.t. names */
		g_free(p);
		g_free(name);
	}
	glob = g_build_filename(tmp_dir, "*.log", NULL);
	log_search_match_cap = 3;
	files = log_search_files(glob, 50, &trunc);
	log_search_match_cap = saved;
	g_assert_true(trunc);
	g_assert_cmpuint(files->len, ==, 3);
	for (i = 0; i < 3; i++) {
		struct stat st;

		g_assert_cmpint(stat(files->pdata[i], &st), ==, 0);
		g_assert_cmpint(st.st_mtime, ==, 1008 - i); /* the three newest of 1000..1008 */
	}
	g_ptr_array_free(files, TRUE);
	g_free(glob);
	tmp_teardown();
}

static void test_nul_in_line(void)
{
	const char txt[] = "a\0hello\nnext hello\nlast\n";
	char *path;
	GPtrArray *files;
	LOG_SEARCH_RESULT res;
	Regex *re = re_new("hello");

	tmp_setup();
	path = write_file("n.log", txt, sizeof(txt) - 1);
	files = one_file(path);
	log_search_run(files, re, 10, 1000000, &res);
	g_assert_cmpuint(res.hits->len, ==, 2);
	g_assert_cmpstr(hit_at(&res, 0)->line, ==, "a hello");
	g_assert_cmpstr(hit_at(&res, 1)->line, ==, "next hello");
	log_search_result_clear(&res);
	g_ptr_array_free(files, TRUE);
	g_free(path);
	i_regex_unref(re);
	tmp_teardown();
}

/* lines of 65534..65537 bytes, with and without a final newline */
static void test_buffer_boundary(void)
{
	static const int lens[] = {65534, 65535, 65536, 65537};
	Regex *re = re_new("^hello|^tail");
	guint k;
	int with_nl;

	tmp_setup();
	for (k = 0; k < G_N_ELEMENTS(lens); k++) {
		for (with_nl = 0; with_nl < 2; with_nl++) {
			GString *s = g_string_new("hello");
			char *path;
			GPtrArray *files;
			LOG_SEARCH_RESULT res;

			while ((int)s->len < lens[k])
				g_string_append_c(s, 'x');
			g_string_append(s, "\ntail one\nhello two\ntail");
			if (with_nl)
				g_string_append_c(s, '\n');
			path = write_file("b.log", s->str, s->len);
			files = one_file(path);
			log_search_run(files, re, 10, G_MAXINT64, &res);
			g_assert_cmpuint(res.hits->len, ==, 4);
			g_assert_cmpstr(hit_at(&res, 1)->line, ==, "tail one");
			g_assert_cmpstr(hit_at(&res, 2)->line, ==, "hello two");
			g_assert_cmpstr(hit_at(&res, 3)->line, ==, "tail");
			log_search_result_clear(&res);
			g_ptr_array_free(files, TRUE);
			g_free(path);
			g_string_free(s, TRUE);
		}
	}
	i_regex_unref(re);
	tmp_teardown();
}

static void test_cut_length(void)
{
	Regex *re = re_new("hello");
	int lens[] = {400, 401};
	int k;

	tmp_setup();
	for (k = 0; k < 2; k++) {
		GString *s = g_string_new("hello");
		char *path;
		GPtrArray *files;
		LOG_SEARCH_RESULT res;

		while (s->len < (gsize)lens[k] - 1)
			g_string_append_c(s, 'x');
		g_string_append(s, "\xc3\xa9\n"); /* the last character is 2 bytes */
		path = write_file("c.log", s->str, s->len);
		files = one_file(path);
		log_search_run(files, re, 10, 1000000, &res);
		g_assert_cmpuint(res.hits->len, ==, 1);
		g_assert_true(g_utf8_validate(hit_at(&res, 0)->line, -1, NULL));
		g_assert_cmpint(g_utf8_strlen(hit_at(&res, 0)->line, -1), ==, k == 0 ? 400 : 401);
		g_assert_true(g_str_has_suffix(hit_at(&res, 0)->line, k == 0 ? "\xc3\xa9" : "\xe2\x80\xa6"));
		log_search_result_clear(&res);
		g_ptr_array_free(files, TRUE);
		g_free(path);
		g_string_free(s, TRUE);
	}
	i_regex_unref(re);
	tmp_teardown();
}

static void test_tail_scan(void)
{
	GString *s = g_string_new("hello start\n");
	char *path;
	GPtrArray *files;
	LOG_SEARCH_RESULT res;
	Regex *re = re_new("hello");
	const gint64 max = 100000;
	int i;

	tmp_setup();
	for (i = 0; s->len < 3 * max; i++)
		g_string_append_printf(s, "%06d filler filler filler filler filler filler filler\n", i);
	g_string_append(s, "hello end\n");
	path = write_file("big.log", s->str, s->len);
	files = one_file(path);
	log_search_run(files, re, 10, max, &res);
	g_assert_true(res.size_limit);
	g_assert_cmpint(res.files, ==, 1);
	g_assert_cmpint(res.bytes, ==, max);
	g_assert_cmpuint(res.hits->len, ==, 1);
	g_assert_cmpstr(hit_at(&res, 0)->line, ==, "hello end");
	log_search_result_clear(&res);
	g_ptr_array_free(files, TRUE);
	g_free(path);
	g_string_free(s, TRUE);
	i_regex_unref(re);
	tmp_teardown();
}

static void test_vanished_file(void)
{
	GPtrArray *files = one_file("/nonexistent-dir-for-test/x.log");
	LOG_SEARCH_RESULT res;
	Regex *re = re_new("x");

	log_search_run(files, re, 10, 1000, &res);
	g_assert_cmpuint(res.hits->len, ==, 0);
	g_assert_cmpint(res.files, ==, 0);
	log_search_result_clear(&res);
	g_ptr_array_free(files, TRUE);
	i_regex_unref(re);
}

static void test_encode_roundtrip(void)
{
	LOG_SEARCH_HIT in = {(char *)"f\t.log", (char *)"Day", (char *)"a\tb\nc"}, out;
	LOG_SEARCH_RESULT r = {NULL, 3, 12345, TRUE, FALSE}, r2 = {NULL, 0, 0, FALSE, TRUE};
	int hits = -1;
	char *enc;

	enc = log_search_encode(&in);
	g_assert_cmpstr(enc, ==, "H\tf .log\tDay\ta b c\n");
	g_assert_true(log_search_decode(enc, &out));
	g_assert_cmpstr(out.file_label, ==, "f .log");
	g_assert_cmpstr(out.date, ==, "Day");
	g_assert_cmpstr(out.line, ==, "a b c");
	g_free(out.file_label);
	g_free(out.date);
	g_free(out.line);
	g_free(enc);
	g_assert_true(log_search_decode("H\tlabel\t\t\n", &out));
	g_assert_cmpstr(out.file_label, ==, "label");
	g_assert_cmpstr(out.date, ==, "");
	g_assert_cmpstr(out.line, ==, "");
	g_free(out.file_label);
	g_free(out.date);
	g_free(out.line);
	g_assert_true(log_search_decode("H\tl\td\ttrail  \r\n", &out));
	g_assert_cmpstr(out.line, ==, "trail  ");
	g_free(out.file_label);
	g_free(out.date);
	g_free(out.line);
	g_assert_false(log_search_decode("S\t1\t2\t3\t0\t0\n", &out));
	g_assert_false(log_search_decode("H\tonly\ttwo\n", &out));

	enc = log_search_encode_summary(&r);
	g_assert_cmpstr(enc, ==, "S\t0\t3\t12345\t1\t0\n");
	r.hits = g_ptr_array_new();
	g_ptr_array_add(r.hits, NULL);
	g_free(enc);
	enc = log_search_encode_summary(&r);
	g_assert_cmpstr(enc, ==, "S\t1\t3\t12345\t1\t0\n");
	g_assert_true(log_search_decode_summary(enc, &hits, &r2));
	g_assert_cmpint(hits, ==, 1);
	g_assert_cmpint(r2.files, ==, 3);
	g_assert_cmpint(r2.bytes, ==, 12345);
	g_assert_true(r2.size_limit);
	g_assert_false(r2.file_limit);
	g_assert_false(log_search_decode_summary("S\tx\n", &hits, &r2));
	g_free(enc);
	g_ptr_array_free(r.hits, TRUE);
}

static void perf_run(int every)
{
	const gsize line_len = 120, total = (gsize)1 << 30;
	char line[120], hitline[120], *path;
	struct rusage ru0, ru1;
	GPtrArray *files;
	LOG_SEARCH_RESULT res;
	Regex *re = re_new("needleinhaystack");
	gint64 t0;
	double secs;
	FILE *f;
	gsize written;
	gsize i;
	gboolean wrote_ok;

	for (i = 0; i < line_len - 1; i++)
		line[i] = "abcdefghij klmnopqrst uvwxyz 0123456789 <nick> "[i % 46];
	line[line_len - 1] = '\n';
	memcpy(hitline, line, sizeof(line));
	memcpy(hitline, "needleinhaystack", 16);

	tmp_setup();
	path = g_build_filename(tmp_dir, "big.log", NULL);
	f = g_fopen(path, "wb");
	for (written = 0, i = 0; f != NULL && written + line_len <= total; written += line_len, i++)
		if (fwrite(every > 0 && i % every == 0 ? hitline : line, 1, line_len, f) != line_len)
			break;
	wrote_ok = f != NULL && written + line_len > total && fclose(f) == 0;

	files = one_file(path);
	memset(&res, 0, sizeof(res));
	getrusage(RUSAGE_SELF, &ru0);
	t0 = g_get_monotonic_time();
	if (wrote_ok)
		log_search_run(files, re, 100, G_MAXINT64, &res);
	secs = (g_get_monotonic_time() - t0) / 1e6;
	getrusage(RUSAGE_SELF, &ru1);

	/* clean up first: an assertion failure must not leave 1 GiB behind */
	g_unlink(path);
	tmp_teardown();

	g_assert_true(wrote_ok);
	g_assert_cmpuint(res.hits->len, ==, every > 0 ? 100 : 0);
	g_assert_cmpint(res.files, ==, 1);
	g_test_minimized_result(secs, "1 GiB scan (match every %d lines): %.2f s, %.0f MB/s, peak RSS %ld KB (growth %ld KB)",
	                        every, secs, res.bytes / 1e6 / secs, ru1.ru_maxrss,
	                        ru1.ru_maxrss - ru0.ru_maxrss);
	log_search_result_clear(&res);
	g_ptr_array_free(files, TRUE);
	g_free(path);
	i_regex_unref(re);
}

static void test_perf_1g(void)
{
	perf_run(0);
}

static void test_perf_1g_hits(void)
{
	perf_run(10);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/log_search/glob", test_glob);
	g_test_add_func("/log_search/files_order", test_files_order);
	g_test_add_func("/log_search/files_safety", test_files_safety);
	g_test_add_func("/log_search/files_cap_keeps_newest", test_files_cap_keeps_newest);
	g_test_add_func("/log_search/nul_in_line", test_nul_in_line);
	g_test_add_func("/log_search/buffer_boundary", test_buffer_boundary);
	g_test_add_func("/log_search/cut_length", test_cut_length);
	g_test_add_func("/log_search/tail_scan", test_tail_scan);
	g_test_add_func("/log_search/vanished_file", test_vanished_file);
	g_test_add_func("/log_search/scan_dates", test_scan_dates);
	g_test_add_func("/log_search/ring", test_ring);
	g_test_add_func("/log_search/newest_file_first_stops", test_newest_file_first_stops);
	g_test_add_func("/log_search/size_limit", test_size_limit);
	g_test_add_func("/log_search/long_line", test_long_line);
	g_test_add_func("/log_search/invalid_utf8", test_invalid_utf8);
	g_test_add_func("/log_search/percent_literal", test_percent_literal);
	g_test_add_func("/log_search/encode_roundtrip", test_encode_roundtrip);
	if (g_test_perf()) {
		g_test_add_func("/log_search/perf_1g", test_perf_1g);
		g_test_add_func("/log_search/perf_1g_hits", test_perf_1g_hits);
	}

	return g_test_run();
}
