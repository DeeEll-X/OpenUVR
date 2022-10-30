
CFLAGS=-std=c11 -fPIC -Wall -Wextra -D_GNU_SOURCE=1 -O3 -I$(shell pwd)/../include -I/usr/include/python3.5m

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

sender: main.o 
	$(CC) $(CFLAGS) -o $@ main.o -ldatachannel

