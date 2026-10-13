#!/bin/sh
# Every setting and expando of the fork that has a help text: the source file
# docs/help/in/<t>.in, the generated docs/help/<t> (identical, none of these
# has @SYNTAX@) and the entry in docs/help/meson.build.
# Usage: check-help.sh <source dir>
src=${1:?usage: check-help.sh <source dir>}
rc=0
for t in term_truecolor colors_ansi_24bit print_whois_rpl_in_server_window \
	show_names_on_join_limit nick_mode_color nick_mode_color_owner \
	nick_mode_color_op nick_mode_color_halfop nick_mode_color_voice \
	nick_mode_color_normal nickmode term_notify; do
	in="$src/docs/help/in/$t.in"
	out="$src/docs/help/$t"
	if [ ! -f "$in" ]; then
		echo "FAIL $t: no docs/help/in/$t.in"; rc=1; continue
	fi
	if [ ! -f "$out" ]; then
		echo "FAIL $t: no docs/help/$t (run perl utils/syntax.pl)"; rc=1; continue
	fi
	# syntax.pl appends a blank line; trailing newlines do not count
	if [ "$(cat "$in")" != "$(cat "$out")" ]; then
		echo "FAIL $t: docs/help/$t differs from its .in (run perl utils/syntax.pl)"; rc=1; continue
	fi
	if ! grep -q "'$t'," "$src/docs/help/meson.build"; then
		echo "FAIL $t: not in docs/help/meson.build"; rc=1; continue
	fi
	echo "ok $t"
done
exit $rc
