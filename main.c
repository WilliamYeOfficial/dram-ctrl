#include "dram_ctrl.h"
#include <stdio.h>

#define NUM_REQUESTS 200000

int main(void)
{
    printf("dram-ctrl-sim: DDR4 controller with FR-FCFS + bank interleaving\n\n");

    const struct { bool policy; const char *name; } configs[] = {
        { false, "Closed-page" },
        { true,  "Open-page"   },
    };

    for (size_t i = 0; i < 2; ++i) {
        DRAMCtrl *c = dram_ctrl_create(configs[i].policy);
        dram_workload_run(c, NUM_REQUESTS, 0xBEEF1234CAFE5678ull);
        DRAMStats s = dram_ctrl_stats(c);
        dram_stats_print(&s, configs[i].name);
        dram_ctrl_destroy(c);
    }
    return 0;
}
