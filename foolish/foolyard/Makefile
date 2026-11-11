KDIR   := ../c
OBJDIR := out

CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
          -MMD -MP -Iinclude -I$(KDIR)/src -DACCELERATE_NEW_LAPACK -DCD_LEAFBOOK '-D_Thread_local='

ifeq ($(shell uname),Darwin)
  LDFLAGS ?= -lm -framework Accelerate
else
  LDFLAGS ?= -lm
endif

SHARED := ../../shared/c
# …AND THE SHARED HALF. This used to be one wildcard over $(KDIR)/src, which is
# why it broke silently when sha256.c and deal_rng.c moved to shared/: a wildcard
# does not fail when a file leaves it, it just compiles less. Both directories are
# wildcarded so a NEW file in either is picked up the way one in src/ always was.
# THE SOUND TABLE IS NOT THE SIM'S: cue_voice_table.c is the one voice over soundgen's generated take
# table (c/build/gen/cue_takes.c, made by `make sounds`), a host that PLAYS sound links it, and a
# simulated network plays none. Left in, the wildcard fails the link on the table it cannot find.
KERNEL_SRC := $(filter-out $(wildcard $(KDIR)/src/main_*.c) $(KDIR)/src/cue_voice_table.c, $(wildcard $(KDIR)/src/*.c)) \
              $(filter-out $(wildcard $(SHARED)/*_test.c), $(wildcard $(SHARED)/*.c)) \
              $(SHARED)/mqtt/mqtt.c
# shared/c/mqtt is a directory of its own (the kernel's client speaks MQTT,
# netclient.c), which the wildcard above does not reach.
# A shared file's test lives beside it (b32_test.c next to b32.c) and has its
# own main(), so the wildcard leaves every *_test.c out or the link sees two.
SIM_SRC    := $(wildcard src/*.c) $(wildcard clients/*.c)

ALL_SRC := $(SIM_SRC) $(KERNEL_SRC)
OBJS    := $(addprefix $(OBJDIR)/, $(notdir $(ALL_SRC:.c=.o)))

VPATH := $(sort $(dir $(ALL_SRC)))

foolyard: $(OBJS)
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR):
	mkdir -p $(OBJDIR)

-include $(OBJS:.o=.d)

run: foolyard
	./foolyard

clean:
	rm -rf $(OBJDIR) foolyard

.PHONY: run clean
