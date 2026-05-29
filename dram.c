#include "dram_ctrl.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#define T_RCD    14
#define T_CL     14
#define T_RAS    33
#define T_RP     14
#define T_WR     15
#define T_REFI   7800
#define T_RFC    350
#define T_BURST  4

#define ROW_INVALID  0xFFFFFFFFu
#define QUEUE_DEPTH  64

#define E_ACT_PJ  100
#define E_PRE_PJ   50
#define E_RD_PJ    80
#define E_WR_PJ    90
#define P_IDD_MW    5

typedef enum { BANK_IDLE, BANK_ACTIVE, BANK_PRECHARGING, BANK_REFRESHING } BankState;

typedef struct {
    BankState state;
    uint32_t  open_row;
    uint64_t  ready_at;
    uint64_t  activated_at;
    uint64_t  n_act, n_pre, n_rd, n_wr;
    uint64_t  n_hits, n_misses, n_conflicts;
    double    energy_pJ;
} Bank;

typedef struct {
    uint64_t addr;
    ReqType  type;
    uint64_t arrive_cycle;
    uint32_t bank, row, col;
} MemReq;

typedef struct {
    MemReq entries[QUEUE_DEPTH];
    int    head, tail, count;
} ReqQueue;

struct DRAMCtrl {
    Bank      banks[DRAM_NUM_BANKS];
    ReqQueue  queue;
    uint64_t  cycle;
    uint64_t  next_refresh[DRAM_NUM_BANKS];
    bool      open_page;
    uint64_t  n_completed;
    uint64_t  n_refresh_cycles;
    double    total_latency;
};

static void queue_push(ReqQueue *q, MemReq r)
{
    assert(q->count < QUEUE_DEPTH);
    q->entries[q->tail] = r;
    q->tail = (q->tail + 1) % QUEUE_DEPTH;
    q->count++;
}

static bool queue_empty(const ReqQueue *q) { return q->count == 0; }

static int queue_pick_frcfs(const ReqQueue *q, const Bank banks[])
{
    int      best      = -1;
    bool     best_hit  = false;
    uint64_t best_age  = UINT64_MAX;

    for (int i = 0; i < q->count; ++i) {
        int      idx = (q->head + i) % QUEUE_DEPTH;
        const MemReq *r = &q->entries[idx];
        bool hit = (banks[r->bank].state == BANK_ACTIVE &&
                    banks[r->bank].open_row == r->row);
        uint64_t age = r->arrive_cycle;

        if (best < 0 ||
            (hit && !best_hit) ||
            (hit == best_hit && age < best_age)) {
            best = idx; best_hit = hit; best_age = age;
        }
    }
    return best;
}

static void queue_remove(ReqQueue *q, int idx)
{
    q->entries[idx] = q->entries[q->head];
    q->head = (q->head + 1) % QUEUE_DEPTH;
    q->count--;
}

static void decode_addr(uint64_t addr, uint32_t *bank, uint32_t *row, uint32_t *col)
{
    uint32_t cl = (uint32_t)(addr >> 6);
    *bank = cl & (DRAM_NUM_BANKS - 1);
    *col  = (cl >> 3) & (DRAM_NUM_COLS - 1);
    *row  = (cl >> 13) & (DRAM_NUM_ROWS - 1);
}

static bool try_precharge(DRAMCtrl *c, int b)
{
    Bank *bk = &c->banks[b];
    if (bk->state != BANK_ACTIVE)                    return false;
    if (c->cycle < bk->activated_at + T_RAS)         return false;

    bk->state     = BANK_PRECHARGING;
    bk->ready_at  = c->cycle + T_RP;
    bk->open_row  = ROW_INVALID;
    bk->n_pre++;
    bk->energy_pJ += E_PRE_PJ;
    return true;
}

DRAMCtrl *dram_ctrl_create(bool open_page)
{
    DRAMCtrl *c = calloc(1, sizeof *c);
    c->open_page = open_page;
    for (int b = 0; b < DRAM_NUM_BANKS; ++b) {
        c->banks[b].open_row   = ROW_INVALID;
        c->banks[b].state      = BANK_IDLE;
        c->next_refresh[b]     = T_REFI;
    }
    return c;
}

void dram_ctrl_destroy(DRAMCtrl *c) { free(c); }

bool dram_ctrl_queue_full(const DRAMCtrl *c)
{
    return c->queue.count >= QUEUE_DEPTH - 1;
}

void dram_ctrl_enqueue(DRAMCtrl *c, uint64_t addr, ReqType type)
{
    MemReq r;
    r.addr         = addr;
    r.type         = type;
    r.arrive_cycle = c->cycle;
    decode_addr(addr, &r.bank, &r.row, &r.col);
    queue_push(&c->queue, r);
}

void dram_ctrl_tick(DRAMCtrl *c)
{
    c->cycle++;

    for (int b = 0; b < DRAM_NUM_BANKS; ++b) {
        if (c->cycle >= c->next_refresh[b]) {
            Bank *bk = &c->banks[b];
            if (bk->state == BANK_ACTIVE) try_precharge(c, b);
            if (bk->state == BANK_IDLE) {
                bk->state              = BANK_REFRESHING;
                bk->ready_at           = c->cycle + T_RFC;
                c->next_refresh[b]     = c->cycle + T_REFI;
                c->n_refresh_cycles   += T_RFC;
            }
        }
        Bank *bk = &c->banks[b];
        if ((bk->state == BANK_PRECHARGING || bk->state == BANK_REFRESHING) &&
             c->cycle >= bk->ready_at)
            bk->state = BANK_IDLE;
    }

    if (queue_empty(&c->queue)) return;

    int idx = queue_pick_frcfs(&c->queue, c->banks);
    if (idx < 0) return;

    MemReq *r  = &c->queue.entries[idx];
    Bank   *bk = &c->banks[r->bank];

    if (bk->state == BANK_REFRESHING || bk->state == BANK_PRECHARGING) return;

    if (bk->state == BANK_ACTIVE && bk->open_row == r->row) {
        if (c->cycle < bk->ready_at) return;
        bk->n_hits++;
        bk->n_rd        += (r->type == REQ_READ);
        bk->n_wr        += (r->type == REQ_WRITE);
        bk->energy_pJ   += (r->type == REQ_READ) ? E_RD_PJ : E_WR_PJ;
        bk->ready_at     = c->cycle + T_CL + T_BURST;
        c->total_latency += (double)(c->cycle - r->arrive_cycle);
        c->n_completed++;
        if (!c->open_page) try_precharge(c, (int)(r->bank));
        queue_remove(&c->queue, idx);

    } else if (bk->state == BANK_ACTIVE && bk->open_row != r->row) {
        bk->n_conflicts++;
        bk->n_misses++;
        try_precharge(c, (int)(r->bank));

    } else if (bk->state == BANK_IDLE) {
        bk->state        = BANK_ACTIVE;
        bk->open_row     = r->row;
        bk->activated_at = c->cycle;
        bk->ready_at     = c->cycle + T_RCD;
        bk->n_act++;
        bk->n_misses++;
        bk->energy_pJ   += E_ACT_PJ;
    }
}

void dram_ctrl_drain(DRAMCtrl *c)
{
    while (!queue_empty(&c->queue))
        dram_ctrl_tick(c);
}

DRAMStats dram_ctrl_stats(const DRAMCtrl *c)
{
    DRAMStats s = {0};
    s.total_cycles    = c->cycle;
    s.completed       = c->n_completed;
    s.refresh_cycles  = c->n_refresh_cycles;
    s.avg_latency_cycles = c->n_completed
        ? c->total_latency / (double)c->n_completed : 0.0;

    for (int b = 0; b < DRAM_NUM_BANKS; ++b) {
        s.row_hits      += c->banks[b].n_hits;
        s.row_misses    += c->banks[b].n_misses;
        s.row_conflicts += c->banks[b].n_conflicts;
        s.energy_uJ     += c->banks[b].energy_pJ * 1e-6;
        s.energy_uJ     += (double)c->cycle * 1e-9 * P_IDD_MW * 1e-3 * 1e6;
    }
    return s;
}

static uint64_t xorshift64(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

void dram_workload_run(DRAMCtrl *c, size_t num_requests, uint64_t seed)
{
    uint64_t state = seed;
    for (size_t i = 0; i < num_requests; ++i) {
        while (dram_ctrl_queue_full(c))
            dram_ctrl_tick(c);

        uint64_t addr = (xorshift64(&state) & 3) != 0
            ? (xorshift64(&state) % (1 << 20)) * 64
            : (xorshift64(&state) % (1ull << 32));

        ReqType type = (xorshift64(&state) & 7) ? REQ_READ : REQ_WRITE;
        dram_ctrl_enqueue(c, addr, type);
    }
    dram_ctrl_drain(c);
}

void dram_stats_print(const DRAMStats *s, const char *policy_name)
{
    uint64_t total = s->row_hits + s->row_misses + s->row_conflicts;
    printf("  Policy: %s\n", policy_name);
    printf("    Requests        : %llu\n",    (unsigned long long)s->completed);
    printf("    Total cycles    : %llu\n",    (unsigned long long)s->total_cycles);
    printf("    Avg latency     : %.1f cycles\n", s->avg_latency_cycles);
    printf("    Row hits        : %llu (%.1f%%)\n",
           (unsigned long long)s->row_hits,
           total ? 100.0 * (double)s->row_hits / (double)total : 0.0);
    printf("    Row misses      : %llu\n",    (unsigned long long)s->row_misses);
    printf("    Row conflicts   : %llu\n",    (unsigned long long)s->row_conflicts);
    printf("    Refresh cycles  : %llu\n",    (unsigned long long)s->refresh_cycles);
    printf("    Est. energy     : %.1f µJ\n\n", s->energy_uJ);
}
