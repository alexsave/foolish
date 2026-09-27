KDIR   := ../c
OBJDIR := out

CFLAGS ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-deprecated-declarations \
          -MMD -MP -Iinclude -I$(KDIR)/src -DACCELERATE_NEW_LAPACK -DCD_LEAFBOOK '-D_Thread_local='

ifeq ($(shell uname),Darwin)
  LDFLAGS ?= -lm -framework Accelerate
else
  LDFLAGS ?= -lm
endif

SHARED := ../shared/c
# …AND THE SHARED HALF. This used to be one wildcard over $(KDIR)/src, which is
# why it broke silently when sha256.c and deal_rng.c moved to shared/: a wildcard
# does not fail when a file leaves it, it just compiles less. Both directories are
# wildcarded so a NEW file in either is picked up the way one in src/ always was.
KERNEL_SRC := $(filter-out $(wildcard $(KDIR)/src/main_*.c), $(wildcard $(KDIR)/src/*.c)) \
              $(wildcard $(SHARED)/*.c)
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
