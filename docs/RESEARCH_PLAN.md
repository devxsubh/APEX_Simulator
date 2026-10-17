# APEX Phase 3 plan: load-tracking fidelity and frequency selection

Status: draft for the three authors to review. Week 2 (PELT port, tick, CSV, seeds) is implemented; see README.

## 1. Why pivot

Phases 1–2 asked whether approximating the *arithmetic* of load decay is worth it. The answer from our own
measurements and from kernel history is mostly no:

- Linux PELT already approximates `y^n` with an integer table and shifts (`decay_load()` in
  `kernel/sched/pelt.c`).
- The CUBIC cube root has been a table plus one Newton step since 2007.
- Our microbenchmarks put the remaining headroom at about 1 ns per call.

What the kernel community actually argues about is the *dynamics* of the load signal: how fast it reacts and
how much it ripples, and therefore which CPU frequency (OPP) schedutil picks. Evidence:

- **WALT**: Qualcomm's 2016 RFC argued PELT takes about 74 ms to reach 80% for a fully busy task.
- **util_est**: merged to fix slow frequency ramp-up after wakeup.
- **PELT multiplier**: RFC in 2022, v2 in Dec 2023, a further RFC in 2024. It offers 32/16/8 ms half-lives,
  "better performance at the cost of more power". Merge status unknown, so check it.

New framing: **every load-tracking signal is an approximation of a task's true demand.** Our Phase 2
toolkit was built for exactly this kind of comparison:

- a shadow tracker
- analytic error bounds
- paired decision-by-decision comparison against a reference run

Phase 2 becomes one section of the new paper: *arithmetic error is orders of magnitude smaller than
dynamic tracking error, so the arithmetic is not where the problem is.*

## 2. Research questions and hypotheses

- **RQ1. Ramp and ripple.** How well do closed-form models predict PELT's ramp-up time and steady-state ripple
  as functions of half-life `H`, including the feedback loop with frequency invariance and schedutil?
  - *H1:* the extended model predicts simulated ramp times within 5% and measured ones within 15%.
- **RQ2. Decision fidelity.** How often does each estimator choose a different OPP than an oracle that knows
  true demand, and in which direction (under- vs over-provisioning)?
  - *H2:* shorter `H` cuts under-provisioning on bursty and phase-changing workloads, but increases OPP
    switching and energy on periodic workloads whose period is comparable to `H`.
- **RQ3. Estimator comparison.** How do PELT at 32/16/8 ms, PELT + util_est and a WALT-style window estimator
  trade energy against performance across workload classes?
  - *H3:* util_est recovers most of the ramp penalty for steady periodic tasks, but not for first bursts
    or phase changes. No single `H` is best across workload classes.
- **RQ4. Arithmetic vs dynamics.** Is the arithmetic approximation error (Phase 2 Theorem 2) negligible next
  to dynamic tracking error?
  - *H4:* yes, by at least two orders of magnitude.

## 3. Theory (contribution 1)

Notation: one PELT period is 1024 x 1024 ns = 1.048576 ms (the kernel shifts ns right by 10, then counts
1024 of those units), and `y = 2^(-1/32)` per period. The PELT multiplier makes the clock run 2x or 4x
faster. Util is normalized to [0, 1] at max capacity.

**Ramp time (open loop, at f_max).** For a task that starts running continuously from util 0,
`u(n) = 1 - y^n`. It reaches fraction `a` after `n_a = 32 * log2(1 / (1 - a))` periods.

| Half-life setting | to 80% | to 95% | simulated (80% / 95%) |
|-------------------|--------|--------|-----------------------|
| 32 periods (x1) | 77.9 ms | 145.0 ms | 79.0 / 146.1 ms |
| 16 periods (x2) | 39.0 ms | 72.5 ms | 39.1 / 72.7 ms |
| 8 periods (x4) | 19.5 ms | 36.3 ms | 20.3 / 37.0 ms |

Qualcomm's ~74 ms / ~139 ms figures are the same formula with a 1 ms period. The simulated column comes from
`./apex_sim trace --workload step` (week 2), sampled every period with PELT updates at HZ = 250.

**Steady-state ripple.** For a task that runs `r` periods and sleeps `s` (period `p = r + s`):
- `u_max = (1 - y^r) / (1 - y^p)`
- `u_min = u_max * y^s`

At the 32-period setting with 25% duty, util swings 0.22–0.28 for a 16-period cycle but 0.01–0.75 for a 256-period
cycle. Simulated with a 256 ms period: 14–752 (out of 1024) against 12–771 predicted.
That swing is what drives OPP flapping. We'll predict flapping from `(p, duty, H)` and the OPP table.

**Closed loop with frequency invariance (new work).** At frequency `f`, a busy period contributes
`f / f_max` instead of 1. schedutil then requests `f' = 1.25 * f_max * u` and rounds up to an OPP. Two things
to derive:
1. A bound on the time for an always-busy task to climb from `f_0` to the OPP its demand needs. The
   1.25 headroom gives a geometric climb, slowed by the PELT lag at each step, with rate-limit and OPP
   quantization on top.
2. The condition on `(H, headroom, OPP spacing)` under which a periodic task flaps between two OPPs
   forever.

This is the part most likely to be new; check the related work in §8 first.

**Arithmetic vs dynamic error.** Reuse Theorem 2: with integer `decay_load()`, per-step arithmetic error is
bounded by the table rounding. That bound goes into the RQ4 comparison.

## 4. Simulator changes

The tick becomes one 1024 µs PELT period, and work is measured in cycles, so running at a lower frequency
really does take longer. Each new piece is a separate module so it can be tested on its own.

| Module | Contents | Validated against |
|--------|----------|-------------------|
| `src/pelt.c` (done) | Integer port of `decay_load()`, `accumulate_sum()` and `__accumulate_pelt_segments()`, with frequency and capacity invariance, and the half-life multiplier (x1/x2/x4; out-of-tree patch, not found in mainline `pelt.c`) | Bit-exact unit tests against values from the kernel source; LISA's PELT simulator |
| `src/util_est.c` | `util_est` update and EWMA as in `kernel/sched/fair.c` (read the current source; it has changed across versions) | Hand-computed traces |
| `src/walt.c` | Window-based, non-decaying demand (window W, last N windows; policies max / avg / max(recent, avg), as in the 2016 RFC) | Hand-computed traces |
| `src/oracle.c` | True demand from the workload generator (the reference everything is compared against) | Definition |
| `src/cpufreq.c` | schedutil: `1.25 * f_max * util / cap`, lowest OPP at or above it, `rate_limit_us`; OPP table from a real SoC | Kernel source |
| `src/energy.c` | Energy model: `P = C * V^2 * f` per OPP (kernel EM `dynamic-power-coefficient` style) plus static power | Published EM / DT values |
| `src/workload.c` | Seeded generators (periodic, bursty, phase change, mixed) and trace replay from CSV | — |

Also needed:
- **CSV output** alongside the markdown tables, so the notebook stops parsing text.
- **Seeds and repeats:** 30 seeds per configuration, with means and 95% confidence intervals.
- **Keep Experiments 1–6** under a `--legacy` flag, as the Phase 2 baseline.

## 5. Experiments

| # | Purpose | Workloads | Metrics |
|---|---------|-----------|---------|
| E7 Step response | Test the ramp model (RQ1) | 0 to 100% busy, and 100% to 0 | Ramp time to 80/95%, decay time; predicted vs simulated |
| E8 Periodic ripple | Test the ripple and flapping model (RQ1) | Sweep period 4–256 ms and duty 10–90% | `u_min / u_max`, OPP switches per second; predicted vs simulated |
| E9 Closed-loop fidelity | RQ2, RQ3 | Periodic, bursty, phase change, mixed; 30 seeds each | % time at wrong OPP (under / over), energy vs oracle, deadline misses or completion-latency p95, switch rate |
| E10 Pareto | RQ3 | Same as E9 | Energy vs p95 latency frontier per estimator and H |
| E11 Arithmetic vs dynamic | RQ4 | Same as E9 | Max/mean error from integer arithmetic vs from tracking lag |
| E12 Hardware validation | H1 and the E9 ranking on real hardware | `rt-app` versions of the E7–E9 workloads | Measured ramp (tracepoints), OPP residency, energy (power meter) |

**Comparison method** (carried over from Phase 2): every estimator runs the *same* seeded workload as the
oracle, and we compare OPP decisions tick by tick.

## 6. Hardware validation plan

- **Board:** Raspberry Pi 5 or any Linux board with cpufreq and schedutil. EAS needs asymmetric CPUs, so on a
  symmetric board we only test frequency selection. An Arm big.LITTLE board would add task placement later.
- **Kernel:** a recent mainline kernel. If the PELT multiplier isn't merged, apply the v2 series to a test
  kernel and say so in the paper.
- **Signals:**
  - PELT and util_est tracepoints (`sched_pelt_se`, `sched_util_est_se`); these need LISA's helper module
    to be readable.
  - OPP changes from `power:cpu_frequency`.
- **Workloads:** `rt-app` JSON matching the simulator's generators.
- **Energy:** USB-C inline power meter, or board PMIC readings if available. Report the method and its
  resolution.
- **Analysis:** LISA notebooks, or our notebook reading `trace-cmd` output.

## 7. Timeline (about 10 weeks)

| Week | Deliverable |
|------|-------------|
| 1 | Literature search (§8), confirm PELT multiplier status, freeze RQs |
| 2–3 | `pelt.c` bit-exact with tests; 1024 µs tick; CSV output; seeds |
| 3–4 | Ramp and ripple theory written up; E7, E8 |
| 4–5 | `util_est.c`, `walt.c`, `oracle.c`, `cpufreq.c`, `energy.c` |
| 5–6 | Closed-loop theory; E9, E10, E11 with confidence intervals |
| 6–8 | Hardware bring-up and E12 |
| 9–10 | Paper draft, artifact cleanup, submission |

Suggested split for three authors: theory and bounds; simulator and experiments; hardware and analysis.

## 8. Related work to check before claiming novelty

- **PELT half-life / multiplier discussion:** LKML (2022 RFC, Dec 2023 v2, 2024 RFC), plus Linux Plumbers
  scheduler microconference and OSPM summit talks on PELT half-life.
- **util_est:** Patrick Bellasi's series (2017–2018) and its evaluation.
- **WALT:** 2016 RFC; current Android Common Kernel and vendor-module documentation.
- **schedutil and EAS evaluations:** Arm and Linaro papers and talks; LISA's PELT simulator (Arm GitLab).
- **Learned or alternative DVFS governors** from academic venues (search terms: DVFS governor learning,
  mobile, energy).
- **Approximate computing with quality control:** Green (PLDI 2010), Pliant (HPCA 2019), for the related-work
  section.

Novelty claim to test: no existing work gives a closed-form, frequency-invariance-aware model of PELT ramp and
flapping under schedutil, validated in simulation and on hardware across half-lives and estimators.

## 9. Risks

| Risk | Mitigation |
|------|------------|
| Someone has already done this (likely inside Arm or Google) | Week 1 search. If found, narrow to the closed-loop theory or to workload classes they skipped |
| Simulator isn't realistic | Bit-exact PELT port, then hardware validation (E12) |
| Multiplier not in mainline | Patched test kernel, stated plainly |
| Energy measurement too coarse | Longer runs, report resolution; fall back to the energy model with that limitation stated |
| Scope creep (EAS task placement) | Out of scope for this paper; future work |

## 10. Target venues

Verify current dates:
- Linux Plumbers Conference (scheduler microconference)
- OSPM summit
- OSPERT workshop
- an ACM Student Research Competition
- a systems workshop paper on arXiv first
