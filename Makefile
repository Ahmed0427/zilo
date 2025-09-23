CC = gcc
CFLAGS = -Wall -Wextra -pedantic

zilo: main.c
	$(CC) $(CFLAGS) main.c -o zilo
