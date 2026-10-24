/*
 * /CLEAR and closing a window free every line of a buffer without a
 * "gui textbuffer line removed" for each. Whoever keeps LINE_REC pointers
 * (the image preview does) learns it from "gui textbuffer cleared", sent
 * before the lines are freed; otherwise it keeps pointers whose addresses
 * the next printed lines get.
 */
#include <irssi/src/common.h>
#include <irssi/src/core/args.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/levels.h>
#include <irssi/src/core/signals.h>
#include <irssi/src/fe-ansi/textbuffer.h>
#include <irssi/src/fe-ansi/textbuffer-view.h>
#include "../test-home.h"

#define MODULE_NAME "tests"

static TEXT_BUFFER_REC *cleared_buffer;
static int cleared_count, lines_at_signal;

static void sig_cleared(TEXT_BUFFER_REC *buffer)
{
	cleared_buffer = buffer;
	cleared_count++;
	/* sent while the lines are still there */
	lines_at_signal = buffer->lines_count;
}

static void add_line(TEXT_BUFFER_VIEW_REC *view, const char *text)
{
	LINE_INFO_REC info = { 0 };
	GString *data;
	LINE_REC *line;

	info.level = MSGLEVEL_CLIENTCRAP;
	data = g_string_new(text);
	g_string_append_c(data, 0);
	g_string_append_c(data, LINE_CMD_EOL);
	/* into the buffer only: a view would format it, for a window */
	line = textbuffer_insert(view->buffer, textbuffer_line_last(view->buffer),
	                         (const unsigned char *) data->str, data->len, &info);
	g_assert_nonnull(line);
	g_string_free(data, TRUE);
}

static void test_cleared_signal(void)
{
	TEXT_BUFFER_REC *buffer;
	TEXT_BUFFER_VIEW_REC *view;

	signal_add("gui textbuffer cleared", (SIGNAL_FUNC) sig_cleared);

	buffer = textbuffer_create(NULL);
	view = textbuffer_view_create(buffer, 80, 10, FALSE, TRUE);
	add_line(view, "one https://example.org/a.png");
	add_line(view, "two");
	g_assert_cmpint(buffer->lines_count, >, 0);

	/* /CLEAR */
	cleared_count = 0;
	textbuffer_view_remove_all_lines(view);
	g_assert_cmpint(cleared_count, ==, 1);
	g_assert_true(cleared_buffer == buffer);
	g_assert_cmpint(lines_at_signal, >, 0);
	g_assert_cmpint(buffer->lines_count, ==, 0);

	/* the window closed */
	add_line(view, "three");
	cleared_count = 0;
	textbuffer_view_destroy(view);
	g_assert_cmpint(cleared_count, ==, 1);
	g_assert_true(cleared_buffer == buffer);
	g_assert_cmpint(lines_at_signal, >, 0);

	signal_remove("gui textbuffer cleared", (SIGNAL_FUNC) sig_cleared);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/textbuffer/cleared-signal", test_cleared_signal);

	test_home_setup();

	core_preinit(argv[0]);
	core_init();

	return g_test_run();
}
