CC ?= gcc
PREFIX ?= /usr
DESTDIR ?=
BINDIR = $(PREFIX)/bin
SYSTEMDDIR = $(PREFIX)/lib/systemd/system

CFLAGS = -O3 -Wall -Wextra $(shell pkg-config --cflags libinput libudev libtsm)
LDFLAGS = $(shell pkg-config --libs libinput libudev libtsm) -lm -lutil

TARGET = touchvt
OBJS = main.o def_font.o stb_truetype.o

all: $(TARGET)

main.o: main.c
	$(CC)$(CFLAGS_COMMON) -D_GNU_SOURCE -c main.c

def_font.o: def_font.h
	$(CC)$(CFLAGS_COMMON) -x c -c $< -o $@

stb_truetype.o: stb_truetype.h
	$(CC)$(CFLAGS_COMMON) -DSTB_TRUETYPE_IMPLEMENTATION -x c -c $< -o $@

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDFLAGS)

clean:
	rm -f $(TARGET) *.o

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	install -Dm644 touchvt.droidian.service \
		$(DESTDIR)$(SYSTEMDDIR)/touchvt@.service

.PHONY: all clean install
