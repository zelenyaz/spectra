CC = gcc
CFLAGS = -std=gnu11 -O2 -Wall -Wextra -fPIC -pthread -Iinclude -D_GNU_SOURCE -MMD -MP
LDFLAGS = -shared -ldl -lnuma -lpthread -lrt -lm

# Opt-in debug: dump each object's stride histograms per epoch to
# /tmp/spectra-stride-bins.csv. Enable with:
#   make DEBUG_REGULARITY=1
ifeq ($(DEBUG_REGULARITY),1)
CFLAGS += -DTIERMEM_DEBUG_REGULARITY
endif

SRCS = hook_main.c hook_alloc.c obj_store.c monitor.c migrate.c thread.c shm.c swpf_cache.c cha_lat.c congest_ctrl.c
OBJS = $(SRCS:.c=.o)
DEPS = $(OBJS:.o=.d)
TARGET = libtiermem.so

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET)

-include $(DEPS)
