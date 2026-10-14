CC     = gcc
CFLAGS = -std=c11 -D_POSIX_C_SOURCE=200809L \
         -Wall -Wextra -Wpedantic -O2 -g -pthread
LDFLAGS = -pthread

SRCS = main.c trace.c cache.c index.c pool.c
OBJS = $(SRCS:.c=.o)
TARGET = cachelab

.PHONY: all clean check pool-check

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

main.o: main.c cachelab.h
trace.o: trace.c cachelab.h
cache.o: cache.c cachelab.h
index.o: index.c cachelab.h
pool.o: pool.c cachelab.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

tests/pool_check: tests/pool_check.c pool.c cachelab.h
	$(CC) $(CFLAGS) -I. -o $@ tests/pool_check.c pool.c $(LDFLAGS)

pool-check: tests/pool_check
	./tests/pool_check

check: pool-check $(TARGET)
	python3 tests/check.py

clean:
	rm -f $(OBJS) $(TARGET) tests/pool_check
