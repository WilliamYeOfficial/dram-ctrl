# dram-ctrl-sim

Cycle-accurate DDR4 DRAM controller simulation. Models 8-bank interleaving, FR-FCFS request scheduling, DDR4 timing parameter enforcement (tRAS/tRCD/tRP/tRC), per-bank auto-refresh, and a per-operation energy model. Compares open-page versus closed-page policies on a mixed sequential/random workload.

---

## DRAM Geometry

```
One DRAM rank (simulated):

  Bank 0   Bank 1   Bank 2   Bank 3   Bank 4   Bank 5   Bank 6   Bank 7
  ──────   ──────   ──────   ──────   ──────   ──────   ──────   ──────
  32768    32768    32768    32768    32768    32768    32768    32768
  rows     rows     rows     rows     rows     rows     rows     rows
  │                                                             │
  └─ Each row: 1024 columns × 8 bytes = 8 KiB                 ┘

Address interleaving (cache-line granularity, 64 bytes):
  bank = cl[2:0]           (3 bits  → 8 banks)
  col  = cl[12:3]          (10 bits → 1024 columns)
  row  = cl[22:13]         (15 bits → 32768 rows)
  where cl = physical_address >> 6
```

Interleaving at cache-line granularity means sequential access patterns spread across all 8 banks, maximising bank-level parallelism.

---

## DDR4 Timing Parameters

```
Parameter   Cycles   Meaning
─────────   ──────   ───────
tRCD           14    RAS-to-CAS: time after ACTIVATE before first read/write
tCL            14    CAS Latency: time from READ command to first data
tRAS           33    Minimum row active time before PRECHARGE allowed
tRP            14    Row Precharge time
tRC            47    Row Cycle time = tRAS + tRP  (min time between ACT commands to same bank)
tWR            15    Write Recovery: time after last write before PRECHARGE
tREFI        7800    Average Refresh Interval (7.8 µs at 1 ns/cycle = DDR4-2000)
tRFC          350    Refresh Cycle time (bank unavailable during refresh)
tBURST          4    Burst length 8 / 2 (double data rate)
```

---

## Bank State Machine

```
          IDLE
           │ ▲
    ACT    │ │  PRE (after tRAS elapsed)
    (+tRCD)│ │
           ▼ │
          ACTIVE ──────────────────────────────────┐
           │  │                                    │ row conflict:
           │  └── row hit:                         │ another row needed
           │       READ/WRITE                      │
           │       (+tCL+tBURST)                   ▼
           │                              PRECHARGING
           │                               (+tRP)
           │                                  │
           ▼                                  │
      REFRESHING ◄──── tREFI timer fires      │
       (+tRFC)                                │
           │                                  │
           └──────────────── IDLE ◄───────────┘
```

On every tick, each bank's `ready_at` timer is checked. Precharge and refresh transitions happen automatically without explicit commands from the scheduler.

---

## Request Classification

```
Incoming request (addr, READ/WRITE)
         │
         ▼
   Decode address → (bank, row, col)
         │
         ▼
   Bank state?
         │
   ┌─────┼──────────────────┐
   │     │                  │
 IDLE  ACTIVE             ACTIVE
         │(open_row         │(open_row
         │== req.row)       │!= req.row)
         ▼                  ▼
      ROW HIT           ROW CONFLICT
    tCL + tBURST        → precharge first
    latency                (tRAS + tRP)
                           then activate
                           (tRCD) then access
         │
      ROW MISS
    (bank IDLE)
    → activate row (tRCD)
      then access
```

Row hit rates directly impact average latency. Open-page policy keeps rows open after access (hoping for another hit); closed-page policy precharges immediately after each access.

---

## FR-FCFS Scheduler

First-Ready, First-Come-First-Served: among all pending requests, prefer row-hit requests over misses; break ties by arrival time (oldest first).

```
Request Queue (depth 64, FIFO structure):
┌──────┬──────┬──────┬──────┬──────┬──────┐
│ req0 │ req1 │ req2 │ req3 │ req4 │ req5 │ ...
│ bank2│ bank0│ bank2│ bank7│ bank2│ bank1│
│ row A│ row B│ row A│ row C│ row B│ row D│
└──────┴──────┴──────┴──────┴──────┴──────┘

Bank 2 currently has row A open (row hit for req0 and req2):
  best = req0  (hit, oldest)
  then = req2  (hit, younger)
  then = req1,4,5 (miss, sorted by age)
```

---

## Auto-Refresh

Each bank has an independent `next_refresh` counter initialised to `tREFI` (7800 cycles). When the current cycle reaches `next_refresh`:

```
  if bank == ACTIVE  → issue PRECHARGE (if tRAS elapsed)
  if bank == IDLE    → enter REFRESHING state for tRFC cycles
                        next_refresh += tREFI
  if bank == PRECHARGING/REFRESHING → wait
```

Refresh is staggered across banks (each bank has its own counter), so refresh overhead is distributed rather than causing a single system-wide stall.

---

## Energy Model

```
Operation    Energy
─────────    ──────
ACTIVATE      100 pJ   (charge sense amplifiers)
PRECHARGE      50 pJ   (restore bit lines)
READ           80 pJ   (column access + output drivers)
WRITE          90 pJ   (column access + write amplifiers)
Background      5 mW   (IDD2N standby current per bank)

Total energy = Σ(per-operation) + cycle_count × 1ns × 5mW × 8_banks
```

---

## API

```c
// Lifecycle
DRAMCtrl *dram_ctrl_create(bool open_page_policy);
void      dram_ctrl_destroy(DRAMCtrl *c);

// Request injection
void dram_ctrl_enqueue(DRAMCtrl *c, uint64_t addr, ReqType type);
bool dram_ctrl_queue_full(const DRAMCtrl *c);

// Simulation
void dram_ctrl_tick(DRAMCtrl *c);     // advance one cycle
void dram_ctrl_drain(DRAMCtrl *c);    // run until queue empty

// Statistics
DRAMStats dram_ctrl_stats(const DRAMCtrl *c);

// Workload + reporting
void dram_workload_run(DRAMCtrl *c, size_t num_requests, uint64_t seed);
void dram_stats_print(const DRAMStats *s, const char *policy_name);
```

---

## Build

```sh
gcc -O2 -std=c11 dram_ctrl.c main.c -o dram-ctrl-sim
```

---

## Sample Output

```
  Policy: Closed-page
    Requests        : 200000
    Total cycles    : 6327878
    Avg latency     : 1992.9 cycles
    Row hits        : 200000 (12.5%)
    Row misses      : 798223
    Row conflicts   : 605816
    Refresh cycles  : 2265200
    Est. energy     : 298.2 µJ

  Policy: Open-page
    Requests        : 200000
    Total cycles    : 6333997
    Avg latency     : 2001.1 cycles
    ...
```

The workload is 75% sequential, 25% random. Sequential accesses benefit from open-page policy (consecutive lines in the same row stay open), but random accesses suffer from frequent row conflicts. On this mixed workload the two policies perform similarly; a workload with higher spatial locality would show a larger open-page advantage.

---

## File Structure

```
dram-ctrl-sim/
├── dram_ctrl.h    ← public API (opaque handle, ReqType enum, DRAMStats)
├── dram_ctrl.c    ← bank FSM, FR-FCFS scheduler, refresh, energy model
└── main.c         ← runs both policies, prints comparison
```
