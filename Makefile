# Compiler and flags
CC = gcc
CFLAGS = -Wall -g

# Build both oss and worker executables
all: oss worker

# Build oss executable from oss.c
oss: oss.c
	$(CC) $(CFLAGS) -o oss oss.c

# Build worker executable from worker.c
worker: worker.c
	$(CC) $(CFLAGS) -o worker worker.c

# Clean up generated files
clean:
	rm -f oss worker *.o oss.log
