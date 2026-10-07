#pragma once
#include <stdio.h>
static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)
#define RUN(f) do { int b = g_fail; f(); printf("%s %s\n", g_fail == b ? "ok  " : "FAIL", #f); } while (0)
#define DONE() do { printf("%d checks, %d failed\n", g_checks, g_fail); return g_fail ? 1 : 0; } while (0)
