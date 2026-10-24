#ifndef IRSSI_FE_FUZZ_FUZZ_TARGET_H
#define IRSSI_FE_FUZZ_FUZZ_TARGET_H

#include <stddef.h>
#include <stdint.h>

/* What every fuzz target defines: libFuzzer calls them, and replay.c in a
 * build without it */
int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

#endif
