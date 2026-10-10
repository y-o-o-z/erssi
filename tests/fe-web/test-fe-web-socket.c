/*
 test-fe-web-socket.c : the checks of fe_web_socket (Unix socket) paths

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 The socket carries the web client's password, so erssi starts on a path
 only when nobody else can swap a directory above it, replaces only stale
 sockets of its own, and accepts only connections from its own user.
*/

#include "fe-web-test.h"

#include <irssi/src/fe-web/fe-web-pacing.h>

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/un.h>

static char *B;

static void setup(void)
{
	B = test_tmpdir();
}

static void teardown(void)
{
	fe_web_server_deinit();
	g_free(fe_web_fuzz_take_printed());
	test_rmtree(B);
	g_free(B);
	B = NULL;
}

static char *under(const char *rel)
{
	return g_build_filename(B, rel, NULL);
}

static int mode_of(const char *path)
{
	struct stat st;

	g_assert_cmpint(lstat(path, &st), ==, 0);
	return st.st_mode & 07777;
}

static gboolean is_socket(const char *path)
{
	struct stat st;

	return lstat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

static ino_t inode_of(const char *path)
{
	struct stat st;

	g_assert_cmpint(lstat(path, &st), ==, 0);
	return st.st_ino;
}

static void make_dir(const char *rel, int mode)
{
	char *dir = under(rel);

	g_assert_cmpint(g_mkdir_with_parents(dir, mode), ==, 0);
	g_assert_cmpint(chmod(dir, mode), ==, 0);
	g_free(dir);
}

static void make_link(const char *target_rel, const char *link_rel)
{
	char *target = under(target_rel), *link = under(link_rel);

	g_assert_cmpint(symlink(target, link), ==, 0);
	g_free(target);
	g_free(link);
}

/* Hold a second name for the file, so that its inode number cannot be
 * given to a file made later */
static void pin_inode(const char *path)
{
	char *pin = under("pin");

	g_assert_cmpint(link(path, pin), ==, 0);
	g_free(pin);
}

static void assert_printed(const char *out, const char *text)
{
	if (strstr(out, text) == NULL)
		g_error("expected \"%s\" in the output:\n%s", text, out);
}

static void assert_listening(const char *out, const char *path)
{
	char *want = g_strdup_printf("WebSocket server listening on unix:%s", path);

	assert_printed(out, want);
	g_free(want);
	g_assert_true(is_socket(path));
}

static void test_creates_private_dir(void)
{
	char *path, *dir, *out;

	setup();
	path = under("new/sub/fe-web.sock");
	dir = g_path_get_dirname(path);
	out = test_start_unix(path);
	assert_listening(out, path);
	g_assert_cmpint(mode_of(dir), ==, 0700);
	g_assert_cmpint(mode_of(path), ==, 0600);
	fe_web_server_deinit();
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_free(out);
	g_free(dir);
	g_free(path);
	teardown();
}

static void test_group_writable_dir_refused(void)
{
	char *path, *out;

	setup();
	make_dir("d", 0770);
	path = under("d/fe-web.sock");
	out = test_start_unix(path);
	assert_printed(out, "is writable by others (mode 0770)");
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_free(out);
	g_free(path);
	teardown();
}

static void test_untrusted_parent_refused(void)
{
	char *path, *mine, *want, *out;

	setup();
	make_dir("open", 0777);
	path = under("open/mine/fe-web.sock");
	mine = under("open/mine");
	out = test_start_unix(path);
	want = g_strdup_printf("%s/open in the fe_web_socket path can be changed by another user", B);
	assert_printed(out, want);
	g_assert_false(g_file_test(mine, G_FILE_TEST_EXISTS));
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_free(want);
	g_free(out);
	g_free(mine);
	g_free(path);
	teardown();
}

static void test_sticky_parent_allowed(void)
{
	char *path, *out;

	setup();
	make_dir("tmp", 01777);
	path = under("tmp/mine/fe-web.sock");
	out = test_start_unix(path);
	assert_listening(out, path);
	g_free(out);
	g_free(path);
	teardown();
}

static void test_dir_symlink_refused(void)
{
	char *path, *real_sock, *out;

	setup();
	make_dir("real", 0700);
	make_link("real", "link");
	path = under("link/fe-web.sock");
	real_sock = under("real/fe-web.sock");
	out = test_start_unix(path);
	assert_printed(out, "is a symbolic link");
	g_assert_false(g_file_test(real_sock, G_FILE_TEST_EXISTS));
	g_free(out);
	g_free(real_sock);
	g_free(path);
	teardown();
}

static void test_own_link_into_untrusted_refused(void)
{
	char *path, *open_dir, *real, *want, *made, *out;

	setup();
	make_dir("open", 0777);
	make_link("open", "ln");
	path = under("ln/d/fe-web.sock");
	open_dir = under("open");
	real = realpath(open_dir, NULL);
	g_assert_nonnull(real);
	made = under("open/d");
	out = test_start_unix(path);
	want = g_strdup_printf("%s in the fe_web_socket path can be changed by another user", real);
	assert_printed(out, want);
	g_assert_false(g_file_test(made, G_FILE_TEST_EXISTS));
	free(real);
	g_free(want);
	g_free(made);
	g_free(open_dir);
	g_free(out);
	g_free(path);
	teardown();
}

static void test_recheck_after_mkdir(void)
{
	char *path, *race, *out;

	setup();
	path = under("race/d/fe-web.sock");
	race = under("race");
	fe_web_fuzz_mkdir_first(race, 0777);
	out = test_start_unix(path);
	assert_printed(out, "it appeared while erssi created the directory");
	g_assert_false(is_socket(path));
	g_free(out);
	g_free(race);
	g_free(path);
	teardown();
}

static void test_not_a_socket_kept(void)
{
	char *path, *content, *out;

	setup();
	path = under("fe-web.sock");
	g_assert_true(g_file_set_contents(path, "x", 1, NULL));
	out = test_start_unix(path);
	assert_printed(out, "exists and is not a socket of yours");
	g_assert_true(g_file_get_contents(path, &content, NULL, NULL));
	g_assert_cmpstr(content, ==, "x");
	g_free(content);
	g_free(out);
	g_free(path);
	teardown();
}

static void test_stale_socket_replaced(void)
{
	char *path, *out;
	ino_t old;

	setup();
	path = under("fe-web.sock");
	test_dead_socket(path);
	old = inode_of(path);
	pin_inode(path);
	out = test_start_unix(path);
	assert_listening(out, path);
	g_assert_true(inode_of(path) != old);
	g_free(out);
	g_free(path);
	teardown();
}

static void test_live_listener_kept(void)
{
	char *path, *want, *out;
	ino_t old;
	int fd;

	setup();
	path = under("fe-web.sock");
	fd = test_listening_socket(path);
	old = inode_of(path);
	out = test_start_unix(path);
	want = g_strdup_printf("another program is listening on unix:%s", path);
	assert_printed(out, want);
	g_assert_cmpuint(inode_of(path), ==, old);
	g_assert_true(is_socket(path));
	close(fd);
	g_free(want);
	g_free(out);
	g_free(path);
	teardown();
}

static void test_peer_uid_other_refused(void)
{
	char *path, *out, *want;
	int fd;

	setup();
	path = under("fe-web.sock");
	out = test_start_unix(path);
	assert_listening(out, path);
	g_free(out);

	fe_web_fuzz_fake_peer_other_uid(TRUE);
	fd = test_connect_unix(path);
	g_assert_cmpint(fd, >=, 0);
	fe_web_fuzz_iterate(100);
	out = fe_web_fuzz_take_printed();
	want = g_strdup_printf("process of uid %lu, not ours", (unsigned long) getuid() + 1);
	assert_printed(out, want);
	g_assert_null(web_clients);
	fe_web_fuzz_fake_peer_other_uid(FALSE);
	close(fd);
	g_free(want);
	g_free(out);
	g_free(path);
	teardown();
}

static void test_peer_uid_own_accepted(void)
{
	WEB_CLIENT_REC *c;
	char *path, *out, *addr;
	int fd;

	setup();
	path = under("fe-web.sock");
	out = test_start_unix(path);
	assert_listening(out, path);
	g_free(out);

	fd = test_connect_unix(path);
	g_assert_cmpint(fd, >=, 0);
	fe_web_fuzz_iterate(100);
	g_assert_cmpuint(g_slist_length(web_clients), ==, 1);
	c = web_clients->data;
	addr = g_strdup_printf("unix:%s", path);
	g_assert_cmpstr(c->addr, ==, addr);
	g_assert_false(c->authenticated);
	/* login pacing counts it as erssi's own user */
	g_free(addr);
	addr = g_strdup_printf("uid:%lu", (unsigned long) getuid());
	g_assert_cmpstr(c->peer_key, ==, addr);
	close(fd);
	g_free(addr);
	g_free(path);
	teardown();
}

static void test_removed_only_if_ours(void)
{
	char *path, *out;
	ino_t other;

	setup();
	path = under("fe-web.sock");
	out = test_start_unix(path);
	assert_listening(out, path);
	g_free(out);
	/* the pin keeps our inode number from being reused for the new file */
	pin_inode(path);
	g_assert_cmpint(unlink(path), ==, 0);
	test_dead_socket(path);
	other = inode_of(path);
	fe_web_server_deinit();
	g_assert_true(is_socket(path));
	g_assert_cmpuint(inode_of(path), ==, other);
	g_free(path);
	teardown();
}

static void test_path_too_long(void)
{
	struct sockaddr_un sa;
	char *base, *path, *want, *out;
	gsize len;

	setup();
	base = under("");
	len = strlen(base);
	g_assert_cmpuint(len, <, sizeof(sa.sun_path));
	/* exactly sizeof(sun_path) bytes, one more than fits */
	path = g_malloc(sizeof(sa.sun_path) + 1);
	memcpy(path, base, len);
	memset(path + len, 'a', sizeof(sa.sun_path) - len);
	path[sizeof(sa.sun_path)] = '\0';
	g_assert_cmpuint(strlen(path), ==, sizeof(sa.sun_path));
	out = test_start_unix(path);
	want = g_strdup_printf("is too long (at most %d bytes)", (int) sizeof(sa.sun_path) - 1);
	assert_printed(out, want);
	g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS));
	g_free(want);
	g_free(out);
	g_free(path);
	g_free(base);
	teardown();
}

/* read what is there on a non-blocking socket; TRUE at end of file,
 * FALSE when nothing more comes now */
static gboolean reads_eof(int fd)
{
	char buf[4096];
	ssize_t n;

	while ((n = read(fd, buf, sizeof(buf))) > 0)
		;
	return n == 0;
}

/* Every connection on the Unix socket is the owner's: when 8 are logging
 * in, the 9th closes the oldest */
static void test_ninth_pending_evicts_oldest(void)
{
	char *path, *out;
	int fds[9];
	int i;

	setup();
	path = under("fe-web.sock");
	out = test_start_unix(path);
	assert_listening(out, path);
	g_free(out);
	/* refusals are logged once a minute: an hour since any before */
	fe_web_fuzz_set_clock(time(NULL) + 3600);

	for (i = 0; i < 9; i++) {
		fds[i] = test_connect_unix(path);
		g_assert_cmpint(fds[i], >=, 0);
		g_assert_cmpint(fcntl(fds[i], F_SETFL, O_NONBLOCK), ==, 0);
		fe_web_fuzz_iterate(20);
		g_assert_cmpuint(g_slist_length(web_clients), ==, i < 8 ? i + 1 : 8);
	}
	g_assert_true(reads_eof(fds[0]));
	for (i = 1; i < 9; i++)
		g_assert_false(reads_eof(fds[i]));
	out = fe_web_fuzz_take_printed();
	assert_printed(out, "too many connections logging in, oldest closed");
	g_free(out);
	fe_web_fuzz_set_clock(0);
	for (i = 0; i < 9; i++)
		close(fds[i]);
	g_free(path);
	teardown();
}

/* Connect 'n' times; each connection is accepted before the next */
static void connect_n(const char *path, int *fds, int n)
{
	int i;

	for (i = 0; i < n; i++) {
		fds[i] = test_connect_unix(path);
		g_assert_cmpint(fds[i], >=, 0);
		g_assert_cmpint(fcntl(fds[i], F_SETFL, O_NONBLOCK), ==, 0);
		fe_web_fuzz_iterate(20);
	}
}

/* Is a client of peer_key connected. (A closed client's record may be
 * reused for the next one, so its pointer says nothing.) */
static gboolean peer_connected(const char *peer_key)
{
	GSList *tmp;

	for (tmp = web_clients; tmp != NULL; tmp = tmp->next) {
		WEB_CLIENT_REC *c = tmp->data;

		if (g_strcmp0(c->peer_key, peer_key) == 0)
			return TRUE;
	}
	return FALSE;
}

/* With another peer waiting, a new connection of the owner closes that
 * peer's, not the owner's oldest */
static void test_full_queue_keeps_owner(void)
{
	char *path, *out;
	int fds[8];
	int i;

	setup();
	path = under("fe-web.sock");
	out = test_start_unix(path);
	assert_listening(out, path);
	g_free(out);

	connect_n(path, fds, 7);
	fe_web_fuzz_client_new_peer("ip:192.0.2.1", FALSE, FALSE);
	g_assert_cmpuint(g_slist_length(web_clients), ==, 8);
	connect_n(path, fds + 7, 1);
	g_assert_false(peer_connected("ip:192.0.2.1"));
	g_assert_cmpuint(g_slist_length(web_clients), ==, 8);
	for (i = 0; i < 8; i++)
		g_assert_false(reads_eof(fds[i]));
	/* only the browser end of the closed client is left */
	fe_web_fuzz_client_free(NULL);
	for (i = 0; i < 8; i++)
		close(fds[i]);
	g_free(path);
	teardown();
}

/* A trusted peer (it logged in before) is kept like the owner: with only
 * the two waiting, the busier one, the owner, loses its oldest */
static void test_full_queue_keeps_trusted(void)
{
	WEB_CLIENT_REC *trusted;
	char *path, *out;
	int fds[8];
	int i;

	setup();
	path = under("fe-web.sock");
	out = test_start_unix(path);
	assert_listening(out, path);
	g_free(out);

	fe_web_pacing_succeeded(fe_web_fuzz_login_pacing(), "ip:192.0.2.1");
	trusted = fe_web_fuzz_client_new_peer("ip:192.0.2.1", FALSE, FALSE);
	connect_n(path, fds, 8);
	g_assert_true(fe_web_fuzz_client_alive(trusted));
	g_assert_true(peer_connected("ip:192.0.2.1"));
	g_assert_cmpuint(g_slist_length(web_clients), ==, 8);
	g_assert_true(reads_eof(fds[0]));
	for (i = 1; i < 8; i++)
		g_assert_false(reads_eof(fds[i]));
	fe_web_fuzz_client_free(trusted);
	fe_web_fuzz_reset_login_state();
	for (i = 0; i < 8; i++)
		close(fds[i]);
	g_free(path);
	teardown();
}

int main(int argc, char **argv)
{
	test_init(&argc, &argv);

	g_test_add_func("/fe-web/socket/creates-private-dir", test_creates_private_dir);
	g_test_add_func("/fe-web/socket/group-writable-dir-refused", test_group_writable_dir_refused);
	g_test_add_func("/fe-web/socket/untrusted-parent-refused", test_untrusted_parent_refused);
	g_test_add_func("/fe-web/socket/sticky-parent-allowed", test_sticky_parent_allowed);
	g_test_add_func("/fe-web/socket/dir-symlink-refused", test_dir_symlink_refused);
	g_test_add_func("/fe-web/socket/own-link-into-untrusted-refused",
	                test_own_link_into_untrusted_refused);
	g_test_add_func("/fe-web/socket/recheck-after-mkdir", test_recheck_after_mkdir);
	g_test_add_func("/fe-web/socket/not-a-socket-kept", test_not_a_socket_kept);
	g_test_add_func("/fe-web/socket/stale-socket-replaced", test_stale_socket_replaced);
	g_test_add_func("/fe-web/socket/live-listener-kept", test_live_listener_kept);
	g_test_add_func("/fe-web/socket/peer-uid-other-refused", test_peer_uid_other_refused);
	g_test_add_func("/fe-web/socket/peer-uid-own-accepted", test_peer_uid_own_accepted);
	g_test_add_func("/fe-web/socket/removed-only-if-ours", test_removed_only_if_ours);
	g_test_add_func("/fe-web/socket/path-too-long", test_path_too_long);
	g_test_add_func("/fe-web/socket/ninth-pending-evicts-oldest",
	                test_ninth_pending_evicts_oldest);
	g_test_add_func("/fe-web/socket/full-queue-keeps-owner", test_full_queue_keeps_owner);
	g_test_add_func("/fe-web/socket/full-queue-keeps-trusted", test_full_queue_keeps_trusted);

	return g_test_run();
}
