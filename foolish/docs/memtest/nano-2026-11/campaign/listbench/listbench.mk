# Scratch: included after the tree's c_base/Makefile (make -C <tree>/c_base -f this).
include Makefile
LB       := /Users/alex/Dev/foolish-bench-results/campaign/listbench
LB_JSRC  := $(C_ROOT)/foolish/server/src/store/journal.c
LB_OBJS  := $(call c_objs,build/obj/cbase,$(filter-out $(LB_JSRC),$(LIB_SRC))) $(call c_objs,build/obj/kernel,$(KERNEL_SRC))
# $(LB_OUT)/listbench_$(LB_NAME) from $(LB)/journal_$(LB_NAME).c, with $(LB_DEFS)
$(LB_OUT)/listbench_$(LB_NAME): $(LB)/listbench.c $(LB)/journal_$(LB_NAME).c $(LB_OBJS)
	@mkdir -p $(LB_OUT)
	$(CC) $(CB_CFLAGS) $(ALL_INC) -I$(C_ROOT)/foolish/server/src/store -DJOURNAL_C='"$(LB)/journal_$(LB_NAME).c"' -DJOURNAL_NAME='"$(LB_NAME)"' $(LB_DEFS) \
	  -Wno-unused-function $(LB)/listbench.c $(LB_OBJS) -o $@ $(LDLIBS)
