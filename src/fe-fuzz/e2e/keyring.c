/*
 keyring.c : fuzz keyring.json and /e2e import files

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

 The input is a JSON file. A keyring may come from rpe2e.pl, repartee's
 export or the migration tool, an import file from anywhere. What parses
 must write back to the same document (compact and pretty), and importing
 or reading an identity out of it must not crash.
*/

#include <irssi/src/fe-fuzz/e2e/e2e-fuzz.h>

#include <irssi/src/e2e/e2e-json.h>
#include <irssi/src/e2e/e2e-keyring.h>
#include <irssi/src/e2e/e2e-proto.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char rpe2e_name[] = "{\"#\xc3\x85\xc2\xbc" "aba\":1}";

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
	const char *error = NULL;
	E2E_JSON *j;

	(void) argc;
	(void) argv;
	/* rpe2e.pl's byte strings: "#Å¼aba" is the UTF-8 "#żaba" */
	j = e2e_json_parse(rpe2e_name, strlen(rpe2e_name), &error);
	if (j == NULL || e2e_json_get(j, "#\xc5\xbc" "aba") == NULL) {
		fprintf(stderr, "the JSON byte strings are not rpe2e.pl's\n");
		abort();
	}
	e2e_json_free(j);
	return 0;
}

static char *reencode(const char *text, gboolean pretty)
{
	const char *error = NULL;
	E2E_JSON *j = e2e_json_parse(text, strlen(text), &error);
	char *out;

	if (j == NULL) {
		fprintf(stderr, "own output does not parse (%s): %s\n", error, text);
		abort();
	}
	out = e2e_json_encode(j, pretty);
	e2e_json_free(j);
	return out;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	const char *error = NULL;
	E2E_JSON *doc, *kr, *exported;
	E2E_IDENTITY id;
	gboolean created;
	char *compact, *pretty, *again, *err = NULL;

	doc = e2e_json_parse((const char *) data, size, &error);
	if (doc == NULL)
		return 0;

	compact = e2e_json_encode(doc, FALSE);
	again = reencode(compact, FALSE);
	if (strcmp(again, compact) != 0)
		abort();
	g_free(again);
	pretty = e2e_json_encode(doc, TRUE);
	again = reencode(pretty, FALSE);
	if (strcmp(again, compact) != 0)
		abort();
	g_free(again);
	g_free(pretty);
	g_free(compact);

	/* as a keyring: its identity */
	if (e2e_identity_get(doc, &id, FALSE, &created, &err))
		e2e_identity_wipe(&id);
	g_free(err);
	err = NULL;

	/* as an /e2e import file, and exported again */
	kr = e2e_keyring_import(doc, &err);
	g_free(err);
	if (kr != NULL) {
		exported = e2e_keyring_export(kr);
		e2e_json_free(exported);
		e2e_json_free(kr);
	}
	e2e_json_free(doc);
	return 0;
}
