CC := gcc
CFLAGS := -Wall -Wextra -O2 -Iinclude
LDFLAGS := -lm

LIB_SRCS := src/approx.c src/sim.c src/experiments.c src/tcp_cubic.c src/bench.c \
            src/rng.c src/workload.c src/pelt.c src/trace.c
SRCS := src/main.c $(LIB_SRCS)
HDRS := $(wildcard include/*.h)

.PHONY: single modular all run check test clean

# Single source of truth: both binary names are built from src/.
# apex_sim_modular is kept only so older notebooks and scripts still run.
single: apex_sim

modular: apex_sim_modular

all: single modular

apex_sim apex_sim_modular: $(SRCS) $(HDRS)
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@

run: apex_sim
	./apex_sim --experiment all

# Fails (non-zero exit) if any analytic error bound in Experiment 4 is violated.
check: apex_sim
	./apex_sim --experiment 4

tests/test_pelt: tests/test_pelt.c $(LIB_SRCS) $(HDRS)
	$(CC) $(CFLAGS) tests/test_pelt.c $(LIB_SRCS) $(LDFLAGS) -o $@

tests/pelt_replay: tests/pelt_replay.c src/pelt.c include/pelt.h
	$(CC) $(CFLAGS) -fsanitize=undefined -fno-sanitize-recover=undefined tests/pelt_replay.c src/pelt.c $(LDFLAGS) -o $@

# Unit tests, then a differential test of the C port against tests/pelt_ref.py.
test: tests/test_pelt tests/pelt_replay
	./tests/test_pelt
	python3 tests/pelt_ref.py ./tests/pelt_replay 300 1

clean:
	rm -f apex_sim apex_sim_modular tests/test_pelt tests/pelt_replay
