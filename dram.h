#ifndef DRAM_CTRL_H
#define DRAM_CTRL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define DRAM_NUM_BANKS   8
#define DRAM_NUM_ROWS    32768
#define DRAM_NUM_COLS    1024

typedef enum { REQ_READ, REQ_WRITE } ReqType;

typedef struct {
    double   avg_latency_cycles;
    uint64_t total_cycles;
    uint64_t row_hits;
    uint64_t row_misses;
    uint64_t row_conflicts;
    uint64_t refresh_cycles;
    double   energy_uJ;
    uint64_t completed;
} DRAMStats;

typedef struct DRAMCtrl DRAMCtrl;

DRAMCtrl *dram_ctrl_create(bool open_page_policy);
void      dram_ctrl_destroy(DRAMCtrl *c);
void      dram_ctrl_enqueue(DRAMCtrl *c, uint64_t addr, ReqType type);
void      dram_ctrl_tick(DRAMCtrl *c);
void      dram_ctrl_drain(DRAMCtrl *c);
bool      dram_ctrl_queue_full(const DRAMCtrl *c);
DRAMStats dram_ctrl_stats(const DRAMCtrl *c);

void      dram_workload_run(DRAMCtrl *c, size_t num_requests, uint64_t seed);
void      dram_stats_print(const DRAMStats *s, const char *policy_name);

#endif
