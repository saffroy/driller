# (Un)comment the lines below if needed
#GCOV_FLAGS := -fprofile-arcs -ftest-coverage -O0
#DEBUG_FLAGS := -O0 -D DEBUG
ASSERT_FLAGS := -D NDEBUG
LFS_FLAGS := $(shell getconf LFS_CFLAGS)
#M64FLAG := -m64

UNAME := $(shell uname)
CC := gcc
export CC
CFLAGS := $(M64FLAG) -Wall -O2 -g $(GCOV_FLAGS) $(DEBUG_FLAGS)
ifeq ($(UNAME),Linux)
CPPFLAGS := -D _GNU_SOURCE $(LFS_FLAGS)
else
CPPFLAGS := -D _XOPEN_SOURCE=500
endif
CPPFLAGS += $(ASSERT_FLAGS)

LD := gcc
LDFLAGS := $(M64FLAG) $(GCOV_FLAGS)
ifneq ($(UNAME),Linux)
LDLIBS := -lrt -lsocket -lnsl
endif

progs := test_mmpi test_fdproxy test_driller test_dlmalloc test_spinlock
libobjs := fdproxy.o driller.o dlmalloc.o map_cache.o
ifeq ($(UNAME),Linux)
libobjs += linux.o
else
libobjs += solaris.o hsearch_r.o
endif

objs := $(progs:%=%.o) mmpi.o $(libobjs)
deps := $(objs:%.o=%.d)

all: $(progs)

-include $(deps)

%.d: %.c
	./depend.sh $@ $(CPPFLAGS) $<

driller.a: $(libobjs)
	$(AR) r $@ $^

test_mmpi test_fdproxy: mmpi.o
test_dlmalloc: dlmalloc.o

test_driller test_mmpi test_fdproxy: driller.a
test_driller test_mmpi test_fdproxy: LDLIBS += -ldl

test_%.o: CPPFLAGS += -U NDEBUG
test_dlmalloc.o: CPPFLAGS += -D NODRILL
dlmalloc.o: CPPFLAGS += -D DEFAULT_GRANULARITY='((size_t)1U<<20)'
dlmalloc.o driller.o: CPPFLAGS += -D MSPACES
ifneq ($(UNAME),Linux)
dlmalloc.o driller.o: CPPFLAGS += -D USE_DL_PREFIX
endif
solaris.o: CPPFLAGS += -U _XOPEN_SOURCE

test_dlmalloc.c:
	ln -s test_driller.c $@

clean:
	$(RM) *.o driller.a $(progs) *.gcov *.gcda *.gcno core.* strace-* $(deps)

check: $(progs)
	set -x; \
	for p in $(progs); do \
		./$$p.sh ; \
	done

htags:
	htags -Fgns

.PHONY: clean check htags
