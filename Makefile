CC = gcc
CFLAGS = -std=c11 -pedantic -Wall -Wextra
LIBS = -pthread -lcurl

all: crawler

crawler: crawler.c
	$(CC) $(CFLAGS) crawler.c -o crawler $(LIBS)

clean:
	rm -f crawler

# run: reads URLs and depth from urls.txt
run: crawler
	./crawler

