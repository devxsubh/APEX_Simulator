"""Reference PELT in arbitrary-precision Python, for differential testing.

Written separately from src/pelt.c with explicit u32/u64 masking, so it
catches integer-width, overflow and undefined-behaviour bugs in the C port
(Python ints never overflow). It encodes the same reading of the kernel
source, so it cannot catch a misreading of the kernel itself.

Usage: python3 tests/pelt_ref.py ./tests/pelt_replay [cases] [seed]
"""
import random
import subprocess
import sys

U32 = (1 << 32) - 1
U64 = (1 << 64) - 1
PERIOD, LOAD_AVG_MAX = 32, 47742
MIN_DIVIDER = LOAD_AVG_MAX - 1024
Y = 0.5 ** (1 / PERIOD)
TABLE = [int(U32 * Y ** i) for i in range(PERIOD)]


def decay_load(val, n):
    if n > PERIOD * 63:
        return 0
    if n >= PERIOD:
        val >>= n // PERIOD
        n %= PERIOD
    return ((val & U64) * TABLE[n]) >> 32


def segments(periods, d1, d3):
    c1 = decay_load(d1, periods) & U32
    c2 = (LOAD_AVG_MAX - decay_load(LOAD_AVG_MAX, periods) - 1024) & U32
    return (c1 + c2 + d3) & U32


class Avg:
    def __init__(self):
        self.last = self.load_sum = self.runnable_sum = 0
        self.util_sum = self.period_contrib = 0
        self.load_avg = self.runnable_avg = self.util_avg = 0

    def update(self, now, load, runnable, running, avg_load):
        delta = (now - self.last) & U64
        if delta >> 63:  # (s64)delta < 0
            self.last = now
            return
        delta >>= 10
        if not delta:
            return
        self.last = (self.last + (delta << 10)) & U64
        if not load:
            runnable = running = 0
        contrib = delta & U32
        delta += self.period_contrib
        periods = delta // 1024
        if periods:
            self.load_sum = decay_load(self.load_sum, periods)
            self.runnable_sum = decay_load(self.runnable_sum, periods)
            self.util_sum = decay_load(self.util_sum, periods) & U32
            delta %= 1024
            if load:
                contrib = segments(periods, 1024 - self.period_contrib, delta)
        self.period_contrib = delta & U32
        if load:
            self.load_sum = (self.load_sum + load * contrib) & U64
        if runnable:
            self.runnable_sum = (self.runnable_sum + ((runnable * contrib) << 10)) & U64
        if running:
            self.util_sum = (self.util_sum + (contrib << 10)) & U32
        if (periods & U32) == 0:
            return
        divider = (MIN_DIVIDER + self.period_contrib) & U32
        self.load_avg = (avg_load * self.load_sum & U64) // divider
        self.runnable_avg = self.runnable_sum // divider
        self.util_avg = self.util_sum // divider

    def state(self):
        return (self.last, self.load_sum, self.runnable_sum, self.util_sum, self.period_contrib,
                self.load_avg, self.runnable_avg, self.util_avg)


def random_case(rng, steps):
    """Event sequence mixing sub-period, multi-period and very long gaps, sleeps and weights."""
    now, lines = 0, []
    weight = rng.choice([15, 1024, 9548, 88761])
    for _ in range(steps):
        # Mostly gaps inside one half-life so every table index is exercised;
        # occasionally long sleeps that cross the 63-half-life cutoff.
        r = rng.random()
        if r < 0.15:
            gap = rng.randrange(1, 2048)              # sub-"us": often no update at all
        elif r < 0.85:
            gap = rng.randrange(1, 64 << 20)          # 0..64 periods
        elif r < 0.97:
            gap = rng.randrange(64 << 20, 2100 << 20)  # up to the cutoff
        else:
            gap = rng.randrange(2100 << 20, 4 << 30)  # beyond it
        now += gap
        on_rq = rng.random() < 0.7
        running = 1 if on_rq and rng.random() < 0.6 else 0
        lines.append((now, int(on_rq), int(on_rq), running, weight))
    return lines


def main():
    binary = sys.argv[1]
    cases = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    seed = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    rng = random.Random(seed)
    steps_total = 0
    for case in range(cases):
        lines = random_case(rng, rng.randrange(1, 300))
        inp = "".join(f"{n} {l} {r} {u} {w}\n" for n, l, r, u, w in lines)
        out = subprocess.run([binary], input=inp, text=True, capture_output=True, check=True).stdout.split("\n")
        ref = Avg()
        for i, (n, l, r, u, w) in enumerate(lines):
            ref.update(n, l, r, u, w)
            got = tuple(int(x) for x in out[i].split())
            if got != ref.state():
                print(f"MISMATCH case {case} step {i}: input {lines[i]}\n  C   {got}\n  ref {ref.state()}")
                sys.exit(1)
        steps_total += len(lines)
    print(f"differential test: {cases} cases, {steps_total} updates, all bit-identical")


if __name__ == "__main__":
    main()
