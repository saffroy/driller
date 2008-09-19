# (Un)comment the lines below if needed
#GCOV_FLAGS := -fprofile-arcs -ftest-coverage -O0
#DEBUG_FLAGS := -O0 -D DEBUG
ASSERT_FLAGS := -D NDEBUG
LFS_FLAGS := $(shell getconf LFS_CFLAGS)

CC := gcc
export CC
CFLAGS := -Wall -O3 -g $(GCOV_FLAGS) $(DEBUG_FLAGS)
CPPFLAGS := -D _GNU_SOURCE $(ASSERT_FLAGS) $(LFS_FLAGS)

LD := gcc
LDFLAGS := $(GCOV_FLAGS)

progs := test_mmpi test_fdproxy test_driller test_dlmalloc test_spinlock
libobjs := fdproxy.o driller.o dlmalloc.o map_cache.o
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
