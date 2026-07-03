# CubeSat — userspace client for the MCU RadioService (SX1262 over rpmsg).
# The radio hardware is owned by the RISC-V MCU (RT-Thread); this tool talks
# to it through /dev/rpmsg (see docs/riscv-migration/40-diseno-migracion.md).
CC      ?= gcc
CFLAGS  ?= -Os -Wall
LDFLAGS ?= -static

radio_test: radio_test.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<

install: radio_test
	install -d $(DESTDIR)/usr/bin
	install -m 755 radio_test $(DESTDIR)/usr/bin/radio_test

clean:
	rm -f radio_test
