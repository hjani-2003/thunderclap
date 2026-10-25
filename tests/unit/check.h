/* Tiny test helper: CHECK(cond) counts failures; tests return failures. */
#ifndef CHECK_H
#define CHECK_H

#include <stdio.h>

static int g_fail;
#define CHECK(c) do { \
        if (!(c)) { fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } \
    } while (0)
#define DONE() do { \
        if (!g_fail) printf("  ok\n"); \
        return g_fail ? 1 : 0; \
    } while (0)

#endif
