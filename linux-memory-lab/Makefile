CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra -g

all: target_sim

target_sim: target_sim.c
	$(CC) $(CFLAGS) -o $@ $< -lm

# Terminal 1: the process that owns the memory.
run-target: target_sim
	./target_sim

# Terminal 2: the reader plus its web interface on http://localhost:7100
run-inspector:
	python3 inspector.py

clean:
	rm -f target_sim

.PHONY: all run-target run-inspector clean
