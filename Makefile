CFLAGS = -Wall
CC = gcc

build: git2

git2: git2.o mktag.o usage.o
	$(CC) $(CFLAGS) -o $@ $^ -lgit2

git2.o: git2.c

mktag.o: mktag.c

usage.o: usage.c

clean:
	rm -f *.o git2
