/*
 test-signal-abi.c : the calling convention irssi's signals rely on

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

/* signal_emit() calls every handler through one function type, SIGNAL_FUNC,
 * with six pointer arguments, whatever the handler declares: fewer
 * parameters, or an int where the signal passes GINT_TO_POINTER(). ISO C
 * leaves that undefined; it works where the caller passes the arguments
 * and cleans them up and the callee reads only what it declares, as in
 * every C calling convention erssi builds for (docs/shellter/security.md,
 * "Known limitations"). This test is that assumption, checked on the
 * machine the build is for: handlers of every shape get their arguments
 * right through the real signal_emit(), and the caller's state is intact
 * after them - on an ABI where the pattern breaks, it fails or crashes. */

#define MODULE_NAME "test-signal-abi"

#include <irssi/src/common.h>
#include <irssi/src/core/core.h>
#include <irssi/src/core/signals.h>

#include "../test-home.h"

#define SIGNAL_NAME "erssi test signal abi"

static const char *got[SIGNAL_MAX_ARGUMENTS];
static int got_int, calls;

static void reset(void)
{
	memset(got, 0, sizeof(got));
	got_int = 0;
	calls = 0;
}

static void handler0(void)
{
	calls++;
}

static void handler1(const char *a)
{
	calls++;
	got[0] = a;
}

static void handler2_int(const char *a, int n)
{
	calls++;
	got[0] = a;
	got_int = n;
}

static void handler3(const char *a, const char *b, const char *c)
{
	calls++;
	got[0] = a;
	got[1] = b;
	got[2] = c;
}

static void handler6(const char *a, const char *b, const char *c, const char *d,
                     const char *e, const char *f)
{
	calls++;
	got[0] = a;
	got[1] = b;
	got[2] = c;
	got[3] = d;
	got[4] = e;
	got[5] = f;
}

static const char *const ARGS[SIGNAL_MAX_ARGUMENTS] = { "a", "b", "c", "d", "e", "f" };

/* locals of the caller that a callee cleaning up arguments it never had
 * would overwrite, or that a wrong stack pointer would lose */
static void emit_six_and_check_frame(void)
{
	volatile guint64 guard1 = G_GUINT64_CONSTANT(0x0123456789abcdef);
	volatile guint64 guard2 = G_GUINT64_CONSTANT(0xfedcba9876543210);
	int i;

	for (i = 0; i < 100; i++) {
		signal_emit(SIGNAL_NAME, 6, ARGS[0], ARGS[1], ARGS[2], ARGS[3], ARGS[4], ARGS[5]);
		g_assert_cmphex(guard1, ==, G_GUINT64_CONSTANT(0x0123456789abcdef));
		g_assert_cmphex(guard2, ==, G_GUINT64_CONSTANT(0xfedcba9876543210));
	}
}

static void test_fewer_parameters(void)
{
	reset();
	signal_add(SIGNAL_NAME, (SIGNAL_FUNC) handler0);
	emit_six_and_check_frame();
	g_assert_cmpint(calls, ==, 100);
	signal_remove(SIGNAL_NAME, (SIGNAL_FUNC) handler0);

	reset();
	signal_add(SIGNAL_NAME, (SIGNAL_FUNC) handler1);
	emit_six_and_check_frame();
	g_assert_cmpint(calls, ==, 100);
	g_assert_true(got[0] == ARGS[0]);
	signal_remove(SIGNAL_NAME, (SIGNAL_FUNC) handler1);

	reset();
	signal_add(SIGNAL_NAME, (SIGNAL_FUNC) handler3);
	emit_six_and_check_frame();
	g_assert_true(got[0] == ARGS[0] && got[1] == ARGS[1] && got[2] == ARGS[2]);
	signal_remove(SIGNAL_NAME, (SIGNAL_FUNC) handler3);
}

static void test_all_parameters(void)
{
	int i;

	reset();
	signal_add(SIGNAL_NAME, (SIGNAL_FUNC) handler6);
	emit_six_and_check_frame();
	for (i = 0; i < SIGNAL_MAX_ARGUMENTS; i++)
		g_assert_true(got[i] == ARGS[i]);

	/* a signal with fewer arguments: the rest arrive as NULL */
	reset();
	signal_emit(SIGNAL_NAME, 2, ARGS[0], ARGS[1]);
	g_assert_true(got[0] == ARGS[0] && got[1] == ARGS[1]);
	for (i = 2; i < SIGNAL_MAX_ARGUMENTS; i++)
		g_assert_null(got[i]);
	signal_remove(SIGNAL_NAME, (SIGNAL_FUNC) handler6);
}

/* an int parameter where the signal passes GINT_TO_POINTER() */
static void test_int_parameter(void)
{
	reset();
	signal_add(SIGNAL_NAME, (SIGNAL_FUNC) handler2_int);
	signal_emit(SIGNAL_NAME, 2, ARGS[0], GINT_TO_POINTER(-12345));
	g_assert_true(got[0] == ARGS[0]);
	g_assert_cmpint(got_int, ==, -12345);
	signal_emit(SIGNAL_NAME, 2, ARGS[0], GINT_TO_POINTER(G_MAXINT));
	g_assert_cmpint(got_int, ==, G_MAXINT);
	signal_remove(SIGNAL_NAME, (SIGNAL_FUNC) handler2_int);
}

int main(int argc, char **argv)
{
	g_test_init(&argc, &argv, NULL);

	test_home_setup();
	core_preinit(*argv);
	irssi_gui = IRSSI_GUI_NONE;
	core_init();

	g_test_add_func("/signal-abi/fewer-parameters", test_fewer_parameters);
	g_test_add_func("/signal-abi/all-parameters", test_all_parameters);
	g_test_add_func("/signal-abi/int-parameter", test_int_parameter);

	return g_test_run();
}
