CROSS ?= aarch64-none-linux-gnu-
CC      = $(CROSS)gcc
CFLAGS  = -O2 -Wall -static
BINDIR  = /usr/sbin
DESTDIR ?=

SRCS = src/unisoc-tee-ipc.c src/unisoc-ims-bridge.c
BINS = $(notdir $(SRCS:.c=))

.PHONY: all clean install

all: $(BINS)

unisoc-tee-ipc: src/unisoc-tee-ipc.c
	$(CC) $(CFLAGS) -o $@ $<

unisoc-ims-bridge: src/unisoc-ims-bridge.c
	$(CC) $(CFLAGS) -o $@ $<

install: all
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(BINS) $(DESTDIR)$(BINDIR)/

clean:
	rm -f $(BINS)