CC ?= gcc
PREFIX ?= /usr
DESTDIR ?=

CFLAGS = -O3 -Wall -Wextra -D_GNU_SOURCE -DSTB_TRUETYPE_IMPLEMENTATION \
         $(shell pkg-config --cflags libinput libudev libtsm)
LDFLAGS = $(shell pkg-config --libs libinput libudev libtsm) -lm -lutil

TARGET = touchvt
BINDIR = $(PREFIX)/bin
SYSTEMDDIR = $(PREFIX)/lib/systemd/system

all: $(TARGET)

$(TARGET): main.c stb_truetype.h
	$(CC) $(CFLAGS) -o $@ main.c $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -Dm644 touchvt.service \
		$(DESTDIR)$(SYSTEMDDIR)/touchvt@.service

.PHONY: all clean install
