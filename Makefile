CC      = gcc
CFLAGS  = -Wall -Wextra -g -Wno-format-truncation

# All targets to build
all: jms_coord jms_console jms_pool

# ── jms_coord ──────────────────────────────────────────
jms_coord: jms_coord.o jms_common.o
	$(CC) $(CFLAGS) -o jms_coord jms_coord.o jms_common.o

# ── jms_console ────────────────────────────────────────
jms_console: jms_console.o jms_common.o
	$(CC) $(CFLAGS) -o jms_console jms_console.o jms_common.o

# ── jms_pool ───────────────────────────────────────────
jms_pool: jms_pool.o jms_common.o
	$(CC) $(CFLAGS) -o jms_pool jms_pool.o jms_common.o

# ── Object files ───────────────────────────────────────
# The %.o pattern rule: each .c file depends on itself AND jms_common.h
# so that changing the header forces a recompile of all files.
%.o: %.c jms_common.h
	$(CC) $(CFLAGS) -c $<

# ── Clean ──────────────────────────────────────────────
clean:
	rm -f *.o jms_coord jms_console jms_pool
	rm -f jms_in jms_out
	rm -f jms_pool_*

.PHONY: all clean