#pragma once
#include <stdbool.h>
// Periodic diagnostics in the serial log: heap (internal / DMA / PSRAM), display frame timing
// and lock contention, per-task CPU load and stack headroom. Lines start with "diag:".
void diag_start(int period_s);
void diag_mark(const char *stage);     // log heap at a boot stage
bool diag_bench(void);                 // time full-screen renders (display lock taken inside; needs a ~10 KB stack)
void diag_bench_request(void);         // the same in its own task (from tasks with small stacks)
