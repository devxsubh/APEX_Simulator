# APEX-CFS Simulator

A userspace research simulator for approximate arithmetic in OS-kernel internals. It models:

- CFS-style load-tracking decay (`e^(-lambda t)`) and its approximations
- the cube root used to compute `K` in TCP CUBIC

There is one codebase (`src/*.c`, `include/*.h`). `apex_sim` and `apex_sim_modular` are built from the
same sources; the second name exists only for older notebooks.

## Build and run

```bash
make all                          # builds apex_sim and apex_sim_modular
./apex_sim --experiment all       # Phase 2 experiments: all, or 1-6
make check                        # exits non-zero if any Experiment 4 bound is violated
make test                         # PELT unit tests + differential test (needs python3)
./apex_sim trace --workload step  # Phase 3 PELT trace engine; run with no options for help
```

## Phase 3: PELT trace engine (in progress)

The project is pivoting to load-tracking fidelity and its effect on frequency selection; see
[docs/RESEARCH_PLAN.md](docs/RESEARCH_PLAN.md). Week 2 of that plan is in place:

- **`src/pelt.c`**: integer port of Linux PELT (`decay_load`, `accumulate_sum`, `__accumulate_pelt_segments`,
  `___update_load_sum`, `___update_load_avg`, rq `clock_pelt` with capacity and frequency invariance, idle
  sync and `lost_idle_time`). It was checked line by line against torvalds/linux master on 2026-10-18. The
  x2/x4 PELT multiplier follows the out-of-tree v2 patch; it is not in mainline `pelt.c`.
- **`src/trace.c`**: single-CPU discrete-event engine. PELT updates happen where the kernel does them
  (scheduler tick, enqueue, dequeue, context switch). Signals are sampled every PELT period
  (1024 x 1024 ns) without changing state. Work is measured at full capacity, so lower frequency means
  longer run time.
- **`src/workload.c`, `src/rng.c`**: seeded `step`, `periodic`, `bursty` and `phase` workloads using
  SplitMix64, so a seed reproduces exactly on any platform. `--seeds K` reports mean and 95% CI.
- **CSV**: `--csv FILE` writes one row per sample for the CPU and each task: `on_rq`, `running`,
  pending work, `util_avg`, `load_avg`, `runnable_avg`.

```bash
./apex_sim trace --workload step --start-ms 50 --stop-ms 1050 --duration-ms 1100 --halflife 16
./apex_sim trace --workload bursty --tasks 3 --seeds 30 --csv bursty.csv
./apex_sim trace --workload periodic --period-ms 16 --duty 0.25 --freq-scale 512
```

How the port is tested (`make test`):

| Test | What it shows |
|------|---------------|
| Table and `LOAD_AVG_MAX` regenerated with the kernel's own generator formula | Constants are the kernel's |
| `decay_load` edge cases and monotonicity; segment arithmetic | Integer behaviour, including truncation, matches the kernel's |
| Saturation (util reaches 1020–1024), one half-life of running gives about 512, one half-life of sleep halves it | Basic PELT dynamics |
| Ramp to 80% / 95% within 1 sample + 3% of `32 log2(1/(1-a))` periods, for x1/x2/x4 | Simulator agrees with the ramp model |
| 25%-duty task reads about 256 at full and at half frequency | Frequency invariance works through the idle sync |
| Same seed reproduces exactly; different seeds differ | Seeding |
| Differential test: about 45k random updates, bit-identical to `tests/pelt_ref.py`, with UBSan aborting on undefined behaviour; a single table entry off by one is detected | No integer-width or overflow bugs in the C port |

Not yet validated: comparison with real kernel traces (PELT tracepoints on hardware, plan week 6–8) and with
LISA's PELT simulator. util_est, WALT-style tracking, schedutil and the energy model are weeks 4–5.

## Approximation modes

| Mode | Decay `d(t)` | Per-step error bound (analytic, `x = lambda t`) |
|------|--------------|--------------------------------------------------|
| `EXACT` | `exp(-lambda t)` | 0 |
| `LINEAR` | `1 - x` (clamped) | `x^2 / 2` |
| `LUT256` | 256-entry table, index `round(t / dt)`, values rounded to 4 decimals | `lambda dt / 2 + 5.1e-5` |
| `POLY2` | `1 - x + x^2/2` (clamped) | `x^3 / 6` |
| `ADAPTIVE` | controller picks POLY2 (SAFE), LUT256 (CAUTION), EXACT (STRICT) | worst of its states |

The controller samples every 8 ticks (256 ms at 32 ms/tick) and moves between states using thresholds
`0.05 / 0.10 / 0.02 / 0.04 * w_min` on a fairness signal (see Known limitations).

## Experiments

| # | What it measures |
|---|------------------|
| 1 | Equal-weight fairness for N = 10, 50, 100: vruntime std-dev %, max pairwise vruntime gap, starvation |
| 2 | One nice -10 task vs ten nice 0 tasks. Each mode is run on the same workload as EXACT and compared tick by tick: % of scheduling decisions that differ, first divergence tick, final vruntime gap in nice-0 slices |
| 2b | Same comparison at load coupling 0.02 / 0.2 / 1.0, to show how much the result depends on that modeling assumption |
| 3 | ADAPTIVE controller over a phased load (5, 55, 15 runnable tasks), reaction time and state transitions |
| 4 | Theorem 2: analytic per-step and cumulative load-error bounds vs a shadow exact tracker, on two workloads |
| 5 | CUBIC cube root: LUT + one Newton step vs LUT only. Accuracy against `cbrt()` and measured ns/op |
| 6 | Measured ns/op of each decay variant, plus max error on the windows the simulator uses |

### Theorem 2 (cumulative load error)

Let `L' = d L + (1-d) c` and `L~' = d~ L~ + (1-d~) c` run on the same inputs `c in [0,1]`, starting at the
same value. If `|d~ - d| <= delta` and `d~ <= D < 1` at every step, then `|L~ - L| <= delta / (1 - D)` at
every step.

Proof: `e' = d~ e + (d~ - d)(L - c)`. Since `L, c in [0,1]`, `|L - c| <= 1`, so `|e'| <= D|e| + delta`.
Unrolling from `e_0 = 0` gives `|e_k| <= delta (1 - D^k) / (1 - D)`.

`delta` comes from the per-step bounds in the table above, evaluated at the largest decay window the run
used. `D` is the largest approximate decay value on that window range; every variant is monotone or convex
there, so the largest value is at an endpoint. Each task carries `load_avg_exact`, an exact tracker fed the
same inputs, so the observed error is pure arithmetic error.

## What changed in this iteration (integrity fixes)

| Before | Now |
|--------|-----|
| CUBIC "2.67x speedup" from hardcoded 80 / 30 cycles and an unrelated citation | Measured ns/op with a call-only baseline subtracted |
| Experiment 2 separated modes through an `if (n == 11)` branch that scaled decay error by weight x 32 | Branch removed. Modes are compared against a paired EXACT run on the same workload |
| Per-task decay window in that branch was always 32 ms (wait counter reset before use) | Removed together with the branch |
| Experiment 4 checked one fixed value (decay at t = 32 ms) against bounds chosen after the fact | Analytic per-step and cumulative bounds checked against a shadow exact tracker over the whole run |
| Reaction time stored in `avg_wait_ticks`; "8 ticks = 8 ms"; "reacted within 1 interval" always printed | Separate `reaction_ticks`, correct 256 ms interval, conditional message |
| "Max Fairness Violation" in Experiment 1 was the last tick's value | Max over all ticks |
| Two copies of the simulator (`apex_sim.c` and `src/`) that had already drifted apart | One codebase |

What the honest numbers show (arm64 laptop, `-O2`):

- Approximation hardly changes scheduling unless load is fed back into weights strongly. At coupling 0.02,
  POLY2 and ADAPTIVE make the same decisions as EXACT. LINEAR differs on 0.9% of ticks; LUT256 on 14%.
- LUT256 has the largest per-step error (about 1.1e-2), not the smallest. Rounding a fractional window to
  the nearest table index costs up to `lambda dt / 2`. The old single-point check could not see this.
- Every analytic bound holds. For the fixed modes the per-step bounds are tight (observed is 94% to 97% of the
  bound); the cumulative bound is loose (observed is 0.1% to 5% of the bound).
- `exp()` costs about 1.7 ns net. POLY2 costs about 0.3 ns. LINEAR and LUT256 are below what this
  benchmark can resolve.
- Dropping the Newton step from the cube root saves about 0.3 ns per call. The old 2.67x figure is not supported.

## Known limitations

- **Load coupling is invented.** Effective weight is `weight * (1 + 0.02 * (load_avg - 0.5))`. Real CFS does
  not feed PELT into vruntime; PELT matters for load balancing and frequency selection (schedutil / EAS).
  Experiment 2b shows the conclusions depend heavily on this constant.
- **Controller signal and units.** Thresholds scaled by `w_min` are compared against vruntime gaps or, in
  Experiment 3, a percentage. The signal reflects contention, not approximation error, so ADAPTIVE goes to
  STRICT exactly when the load is heaviest.
- **Floating point, not kernel fixed point.** Linux PELT already uses an integer table
  (`runnable_avg_yN_inv`, `y^32 = 1/2`, 1024 us periods). The CUBIC routine here is not the kernel's
  `cubic_root()`, which runs about once per congestion epoch.
- **Throughput-style microbenchmark** through a function pointer. Functions cheaper than a call are
  reported as `n/a`. It does not yet measure latency, use cycle counters, or run on x86.
- **Synthetic, deterministic workloads.** There is no randomness, so there are no seeds or confidence
  intervals yet.
- **Stale documents.** `ieee_report.md`, `apex_complete_analysis.ipynb`, `generate_notebook.py` and
  `notebook_source.py` still describe the old results and table headers.

## Roadmap

1. Kernel-faithful baselines: integer PELT `decay_load()` and the real `cubic_root()`, with approximations
   that are cheaper than *those*.
2. Better cost measurement: latency and throughput, cycle counters / `perf stat`, x86 and arm64, and how
   often the kernel calls these paths.
3. Randomized workloads with seeds and confidence intervals, and real `perf sched` traces.
4. Follow the error to where PELT matters: frequency selection (schedutil) and energy.
5. Controller driven by an error estimate (periodic shadow-exact sampling) instead of contention.
6. Prototypes: approximate BPF CUBIC with `netem`, and a `sched_ext` scheduler with its own load tracking.

## File structure

```text
.
├── Makefile
├── docs/      RESEARCH_PLAN.md
├── include/   apex_types.h approx.h sim.h experiments.h tcp_cubic.h bench.h
│              pelt.h trace.h workload.h rng.h
├── src/       main.c approx.c sim.c experiments.c tcp_cubic.c bench.c
│              pelt.c trace.c workload.c rng.c
├── tests/     test_pelt.c pelt_replay.c pelt_ref.py
├── ieee_report.md                 (stale, see Known limitations)
├── apex_complete_analysis.ipynb   (stale)
├── generate_notebook.py           (stale)
└── notebook_source.py             (stale)
```
