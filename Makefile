CC = gcc
CFLAGS = -Wall -Wextra -g

all: oss worker

oss: oss.o
	$(CC) $(CFLAGS) -o oss oss.o

worker: worker.o
	$(CC) $(CFLAGS) -o worker worker.o

oss.o: oss.c
	$(CC) $(CFLAGS) -c oss.c

worker.o: worker.c
	$(CC) $(CFLAGS) -c worker.c

clean:
	rm -f *.o oss worker oss.log
