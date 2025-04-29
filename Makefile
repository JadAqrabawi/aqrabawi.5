CC = gcc
CFLAGS = -std=gnu99 -Wall -Wextra -g

TARGETS = oss worker

all: $(TARGETS)

oss: oss.c
	$(CC) $(CFLAGS) oss.c -o oss

worker: worker.c
	$(CC) $(CFLAGS) worker.c -o worker

clean:
	rm -f $(TARGETS) *.o

.PHONY: all clean
