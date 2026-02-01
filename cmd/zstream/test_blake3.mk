# Simple Makefile for building test_blake3
# Usage: make -f test_blake3.mk

CC = gcc
CFLAGS = -Wall -Wextra -g -O2 -I../../include -I../../lib/libspl/include
LDFLAGS = -pthread -L../../lib/libzpool/.libs -lzpool

test_blake3: test_blake3.o blake3_team.o zstream_shared.o
	$(CC) -o $@ $^ $(LDFLAGS)

test_blake3.o: test_blake3.c blake3_team.h
	$(CC) $(CFLAGS) -c test_blake3.c

blake3_team.o: blake3_team.c blake3_team.h
	$(CC) $(CFLAGS) -c blake3_team.c

zstream_shared.o: zstream_shared.c zstream_shared.h
	$(CC) $(CFLAGS) -c zstream_shared.c

clean:
	rm -f test_blake3 *.o

run: test_blake3
	./test_blake3

.PHONY: clean run
