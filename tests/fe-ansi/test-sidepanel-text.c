#include <irssi/src/common.h>
#include <irssi/src/fe-common/core/formats.h>
#include <irssi/src/fe-ansi/term.h>
#include <irssi/src/fe-ansi/sidepanels-text.h>
#include <irssi/src/fe-ansi/textbuffer-unformat.h>

/* A side panel line is themed once, by format_get_text_theme*(): the
 * format's colour codes are expanded into \004 sequences and the arguments
 * (prefix, nick, window name) are copied verbatim. format_string_expand()
 * expands codes exactly like that, so expanded() below builds the same
 * string, and sidepanel_text_walk() must draw it without a second pass. */

typedef struct {
	GString *text;
	GArray *fg24; /* 24-bit foreground of every character, or UINT_MAX */
} WALK_REC;

static gboolean collect(unichar chr, int width, int color, unsigned int fg24, unsigned int bg24,
                        void *data)
{
	WALK_REC *rec = data;
	unsigned int fg = (color & ATTR_FGCOLOR24) ? fg24 : UINT_MAX;

	g_string_append_unichar(rec->text, chr);
	g_array_append_val(rec->fg24, fg);
	return TRUE;
}

static void walk(const char *text, WALK_REC *rec)
{
	rec->text = g_string_new(NULL);
	rec->fg24 = g_array_new(FALSE, FALSE, sizeof(unsigned int));
	sidepanel_text_walk(text, TERM_TYPE_UTF8, collect, rec);
}

static void walk_free(WALK_REC *rec)
{
	g_string_free(rec->text, TRUE);
	g_array_free(rec->fg24, TRUE);
}

/* format part + verbatim argument + format part, like format_get_text_args() */
static char *expanded(const char *format1, const char *arg, const char *format2)
{
	char *a, *b, *ret;

	a = format_string_expand(format1, NULL);
	b = format_string_expand(format2, NULL);
	ret = g_strconcat(a, arg, b, NULL);
	g_free(a);
	g_free(b);
	return ret;
}

#define FG(rec, i) g_array_index((rec)->fg24, unsigned int, (i))

/* #0550AE is stored as \004 FORMAT_COLOR_24 '%' 'P' 0xAE '0': expanded a
 * second time, "%P" became a colour code and the trailing '0' was printed
 * in front of every voiced nick */
static void test_channel_byte_05(void)
{
	WALK_REC rec;
	char *text;

	text = expanded(" %Z0969DA%_", "+", "%_%N%Z0550AEnick%N");
	walk(text, &rec);
	g_assert_cmpstr(rec.text->str, ==, " +nick");
	g_assert_cmpuint(FG(&rec, 1), ==, 0x0969DA);
	g_assert_cmpuint(FG(&rec, 2), ==, 0x0550AE);
	g_assert_cmpuint(FG(&rec, 5), ==, 0x0550AE);
	walk_free(&rec);
	g_free(text);
}

/* every value of every channel survives: bytes below 0x21 are stored
 * shifted (0x05 -> '%', 0x04 -> '$'), 0x25 itself is a literal '%' */
static void test_all_channel_bytes(void)
{
	static const int shifts[] = { 16, 8, 0 };
	unsigned int v, color;
	int i;

	for (i = 0; i < 3; i++) {
		for (v = 0; v < 256; v++) {
			WALK_REC rec;
			char *fmt, *text;

			color = 0x808080 & ~(0xffu << shifts[i]);
			color |= v << shifts[i];
			fmt = g_strdup_printf("%%Z%06X", color);
			text = expanded(fmt, "x", "%Ny");
			walk(text, &rec);
			g_assert_cmpstr(rec.text->str, ==, "xy");
			g_assert_cmpuint(FG(&rec, 0), ==, color);
			g_assert_cmpuint(FG(&rec, 1), ==, UINT_MAX);
			walk_free(&rec);
			g_free(text);
			g_free(fmt);
		}
	}
}

/* arguments are text, not formats: the halfop prefix and a channel name
 * with '%' are drawn as they are */
static void test_percent_in_arguments(void)
{
	WALK_REC rec;
	char *text;

	text = expanded("%G", "%", "%N%gnick%N");
	walk(text, &rec);
	g_assert_cmpstr(rec.text->str, ==, "%nick");
	walk_free(&rec);
	g_free(text);

	text = expanded("%Z9A6700", "#100%Zcafe%_x", "%N");
	walk(text, &rec);
	g_assert_cmpstr(rec.text->str, ==, "#100%Zcafe%_x");
	g_assert_cmpuint(FG(&rec, 0), ==, 0x9A6700);
	g_assert_cmpuint(FG(&rec, 12), ==, 0x9A6700);
	walk_free(&rec);
	g_free(text);
}

static gboolean stop_after_two(unichar chr, int width, int color, unsigned int fg24,
                               unsigned int bg24, void *data)
{
	int *count = data;

	return ++*count < 2;
}

static void test_stop(void)
{
	char *text;
	int count = 0;

	text = format_string_expand("%Gabcdef", NULL);
	sidepanel_text_walk(text, TERM_TYPE_UTF8, stop_after_two, &count);
	g_assert_cmpint(count, ==, 2);
	g_free(text);
}


/* textbuffer_unformat(): the decoder shared with the text view */

static void test_unformat_bold(void)
{
	const unsigned char data[] = {FORMAT_STYLE_BOLD, 'x', 0};
	const unsigned char *ptr = data;
	int color = 0;
	unsigned int fg24 = UINT_MAX, bg24 = UINT_MAX;

	textbuffer_unformat(&ptr, &color, &fg24, &bg24);
	g_assert_cmpint(color, ==, ATTR_BOLD);
	g_assert(ptr == data + 1);
}

static void test_unformat_fg_bg(void)
{
	const unsigned char data[] = {'1', '2', 'x', 0};
	const unsigned char *ptr = data;
	int color = 0;
	unsigned int fg24 = UINT_MAX, bg24 = UINT_MAX;

	textbuffer_unformat(&ptr, &color, &fg24, &bg24);
	g_assert_cmpint(color, ==, 1 | (2 << BG_SHIFT));
	g_assert(ptr == data + 2);
}

static void test_unformat_nochange_reset(void)
{
	const unsigned char nochange[] = {FORMAT_COLOR_NOCHANGE, '3', 0};
	const unsigned char reset[] = {0xff, 0xff, 0};
	const unsigned char *ptr = nochange;
	int color = 5;
	unsigned int fg24 = UINT_MAX, bg24 = UINT_MAX;

	textbuffer_unformat(&ptr, &color, &fg24, &bg24);
	g_assert_cmpint(color, ==, 5 | (3 << BG_SHIFT));

	ptr = reset;
	color = 0;
	textbuffer_unformat(&ptr, &color, &fg24, &bg24);
	g_assert_cmpint(color, ==, ATTR_RESETFG | ATTR_RESETBG);
}

static void test_unformat_ext1(void)
{
	const unsigned char data[] = {FORMAT_COLOR_EXT1, FORMAT_COLOR_NOCHANGE + 5, 0};
	const unsigned char *ptr = data;
	int color = 0;
	unsigned int fg24 = UINT_MAX, bg24 = UINT_MAX;

	textbuffer_unformat(&ptr, &color, &fg24, &bg24);
	g_assert_cmpint(color, ==, 0x15);
}

static void test_unformat_24bit(void)
{
	char *s = format_string_expand("%Z0550AE", NULL);
	const unsigned char *ptr = (const unsigned char *) s + 1;
	int color = 0;
	unsigned int fg24 = UINT_MAX, bg24 = UINT_MAX;

	g_assert_cmpint((unsigned char) s[0], ==, 4);
	textbuffer_unformat(&ptr, &color, &fg24, &bg24);
	g_assert_cmpuint(fg24, ==, 0x0550AE);
	g_assert(color & ATTR_FGCOLOR24);
	g_free(s);
}

static void test_unformat_24bit_truncated(void)
{
	/* heap copy, so that ASan sees a read past the NUL */
	unsigned char *data = g_malloc(3);
	const unsigned char *ptr;
	int color = 0;
	unsigned int fg24 = UINT_MAX, bg24 = UINT_MAX;

	data[0] = FORMAT_COLOR_24;
	data[1] = 'a';
	data[2] = 0;
	ptr = data;
	textbuffer_unformat(&ptr, &color, &fg24, &bg24);
	g_assert_cmpuint(fg24, ==, UINT_MAX);
	g_assert_cmpint(color, ==, 0);
	g_free(data);
}

/* decode one sequence at data, starting from color 'start' */
static int unformat_one(int start, const unsigned char **end, const unsigned char *data)
{
	int color = start;
	unsigned int fg24 = UINT_MAX, bg24 = UINT_MAX;

	*end = data;
	textbuffer_unformat(end, &color, &fg24, &bg24);
	return color;
}

static void test_unformat_styles(void)
{
	static const struct {
		int code;
		int attr;
	} styles[] = {
		{FORMAT_STYLE_BLINK, ATTR_BLINK},
		{FORMAT_STYLE_UNDERLINE, ATTR_UNDERLINE},
		{FORMAT_STYLE_REVERSE, ATTR_REVERSE},
		{FORMAT_STYLE_ITALIC, ATTR_ITALIC},
	};
	unsigned int i;

	for (i = 0; i < G_N_ELEMENTS(styles); i++) {
		const unsigned char data[] = {styles[i].code, 'x', 0};
		const unsigned char *end;

		/* toggles on, then off again; other bits stay */
		g_assert_cmpint(unformat_one(3, &end, data), ==, 3 | styles[i].attr);
		g_assert(end == data + 1);
		g_assert_cmpint(unformat_one(3 | styles[i].attr, &end, data), ==, 3);
	}
	{
		const unsigned char data[] = {FORMAT_STYLE_DEFAULTS, 'x', 0};
		const unsigned char *end;

		g_assert_cmpint(unformat_one(ATTR_BOLD | 7, &end, data), ==, ATTR_RESET);
		g_assert(end == data + 1);
	}
}

static void test_unformat_ext23(void)
{
	static const struct {
		int code;
		int base;
		gboolean bg;
	} ext[] = {
		{FORMAT_COLOR_EXT2, 0x60, FALSE},    {FORMAT_COLOR_EXT2_BG, 0x60, TRUE},
		{FORMAT_COLOR_EXT3, 0xb0, FALSE},    {FORMAT_COLOR_EXT3_BG, 0xb0, TRUE},
		{FORMAT_COLOR_EXT1_BG, 0x10, TRUE},
	};
	unsigned int i;

	for (i = 0; i < G_N_ELEMENTS(ext); i++) {
		const unsigned char data[] = {ext[i].code, FORMAT_COLOR_NOCHANGE + 5, 0};
		const unsigned char *end;
		int color = unformat_one(ATTR_BOLD | (ext[i].bg ? 2 : 3 << BG_SHIFT), &end, data);

		if (ext[i].bg)
			g_assert_cmpint(color, ==, ATTR_BOLD | 2 | ((ext[i].base + 5) << BG_SHIFT));
		else
			g_assert_cmpint(color, ==, ATTR_BOLD | (3 << BG_SHIFT) | (ext[i].base + 5));
		g_assert(end == data + 2);
	}
}

static void test_unformat_24bit_bg(void)
{
	char *s = format_string_expand("%z0550AE", NULL);
	const unsigned char *ptr = (const unsigned char *) s + 1;
	int color = ATTR_FGCOLOR24 | ATTR_BOLD;
	unsigned int fg24 = 0x123456, bg24 = UINT_MAX;

	textbuffer_unformat(&ptr, &color, &fg24, &bg24);
	g_assert_cmpuint(bg24, ==, 0x0550AE);
	g_assert_cmpuint(fg24, ==, 0x123456);
	g_assert_cmpint(color, ==, ATTR_BOLD | ATTR_FGCOLOR24 | ATTR_BGCOLOR24);
	g_free(s);
}

static void test_unformat_reset_one_side(void)
{
	const unsigned char fgreset[] = {0xff, '4', 0};
	const unsigned char bgreset[] = {'4', 0xff, 0};
	const unsigned char *end;

	g_assert_cmpint(unformat_one(ATTR_BOLD | 7, &end, fgreset), ==,
	                ATTR_BOLD | ATTR_RESETFG | (4 << BG_SHIFT));
	g_assert(end == fgreset + 2);
	g_assert_cmpint(unformat_one(ATTR_BOLD | (7 << BG_SHIFT), &end, bgreset), ==,
	                ATTR_BOLD | 4 | ATTR_RESETBG);
}

static void test_read_unichar_invalid(void)
{
	const unsigned char data[] = {0xC3, 0x28, 0};
	const unsigned char *next = NULL;
	int width = 0;

	g_assert_cmpuint(textbuffer_read_unichar(data, &next, &width), ==, 0xFFFD);
	g_assert(next == data + 1);
	g_assert_cmpint(width, ==, 1);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	g_test_add_func("/test/sidepanel_text/channel_byte_05", test_channel_byte_05);
	g_test_add_func("/test/sidepanel_text/all_channel_bytes", test_all_channel_bytes);
	g_test_add_func("/test/sidepanel_text/percent_in_arguments", test_percent_in_arguments);
	g_test_add_func("/test/sidepanel_text/stop", test_stop);
	g_test_add_func("/textbuffer/unformat/bold", test_unformat_bold);
	g_test_add_func("/textbuffer/unformat/fg-bg", test_unformat_fg_bg);
	g_test_add_func("/textbuffer/unformat/nochange-reset", test_unformat_nochange_reset);
	g_test_add_func("/textbuffer/unformat/ext1", test_unformat_ext1);
	g_test_add_func("/textbuffer/unformat/24bit", test_unformat_24bit);
	g_test_add_func("/textbuffer/unformat/styles", test_unformat_styles);
	g_test_add_func("/textbuffer/unformat/ext23", test_unformat_ext23);
	g_test_add_func("/textbuffer/unformat/24bit-bg", test_unformat_24bit_bg);
	g_test_add_func("/textbuffer/unformat/reset-one-side", test_unformat_reset_one_side);
	g_test_add_func("/textbuffer/unformat/24bit-truncated", test_unformat_24bit_truncated);
	g_test_add_func("/textbuffer/read-unichar/invalid", test_read_unichar_invalid);

#if GLIB_CHECK_VERSION(2,38,0)
	g_test_set_nonfatal_assertions();
#endif
	return g_test_run();
}
