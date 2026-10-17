/*
 * textbuffer_destroy() frees every line info still on the buffer's
 * cur_info list (the lines being formatted when the buffer goes away),
 * each one once: it used to free the first one once per entry, a double
 * free as soon as there were two, and the others leaked.
 */
#include <irssi/src/common.h>
#include <irssi/src/fe-ansi/textbuffer.h>

#define MODULE_NAME "tests"

static void test_destroy_frees_each_line_info(void)
{
	TEXT_BUFFER_REC *buffer;
	int i;

	buffer = textbuffer_create(NULL);
	for (i = 0; i < 3; i++) {
		LINE_INFO_REC *info = g_new0(LINE_INFO_REC, 1);

		info->text = g_strdup_printf("line %d", i);
		buffer->cur_info = g_slist_prepend(buffer->cur_info, info);
	}
	textbuffer_destroy(buffer);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/fe-ansi/textbuffer/destroy-frees-each-line-info",
	                test_destroy_frees_each_line_info);

	return g_test_run();
}
