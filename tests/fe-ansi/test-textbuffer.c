/*
 * textbuffer_destroy() frees every line info still on the buffer's
 * cur_info list (the lines being formatted when the buffer goes away),
 * each one once: it used to free the first one once per entry, a double
 * free as soon as there were two, and the others leaked.
 */
#include <irssi/src/common.h>
#include <irssi/src/fe-ansi/textbuffer.h>

#define MODULE_NAME "tests"

/* The leak half of this test is seen only by LeakSanitizer, so outside an
   AddressSanitizer build it reports itself as skipped instead of passing
   without having checked anything. The body still runs there: glibc's
   allocator catches most double frees on its own. */
#if defined(__SANITIZE_ADDRESS__)
#define HAVE_LEAK_CHECK 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HAVE_LEAK_CHECK 1
#endif
#endif

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
#ifndef HAVE_LEAK_CHECK
	g_test_skip("the leaks are checked only in an AddressSanitizer build "
	            "(-Db_sanitize=address)");
#endif
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/fe-ansi/textbuffer/destroy-frees-each-line-info",
	                test_destroy_frees_each_line_info);

	return g_test_run();
}
