CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2
LDFLAGS ?= -lmosquitto -lcjson -lm

TARGET  := fan_daemon
SRCS    := fan_daemon.c

.PHONY: all clean install

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -d /usr/local/bin
	install -m 755 $(TARGET) /usr/local/bin/
