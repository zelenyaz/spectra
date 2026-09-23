CC ?= gcc
CFLAGS ?= -Wall -Wextra -O2
LDFLAGS ?=

TARGET := ctl
SRCS := ctl.c

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f $(TARGET)
