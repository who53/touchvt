CC = gcc
CFLAGS = -O3 -Wall -Wextra $(shell pkg-config --cflags libinput libudev libtsm)
LDFLAGS = $(shell pkg-config --libs libinput libudev libtsm) -lm -lutil
TARGET = touchvt

all: $(TARGET)

$(TARGET): main.c stb_truetype.h
	$(CC) $(CFLAGS) -o $@ main.c $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -Dm755 $(TARGET) /usr/local/bin/$(TARGET)
	install -Dm644 touchvt.service /etc/systemd/system/touchvt@.service

.PHONY: all clean install