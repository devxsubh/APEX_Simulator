CC := gcc
CFLAGS := -Wall -Wextra -O2 -Iinclude
LDFLAGS := -lm

SRCS := src/main.c src/approx.c src/sim.c src/experiments.c src/tcp_cubic.c src/bench.c
HDRS := $(wildcard include/*.h)

.PHONY: single modular all run check clean

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

clean:
	rm -f apex_sim apex_sim_modular
