# (Un)comment the lines below if needed
#GCOV_FLAGS := -fprofile-arcs -ftest-coverage -O0
#DEBUG_FLAGS := -D DEBUG -U NDEBUG -O0

CC := gcc
CFLAGS := -Wall -O3 -g $(GCOV_FLAGS)
CPPFLAGS := -D _GNU_SOURCE -D NDEBUG $(DEBUG_FLAGS)

LD := gcc
LDFLAGS := $(GCOV_FLAGS)

progs := test_mmpi test_fdproxy test_driller test_dlmalloc
libobjs :=  mmpi.o fdproxy.o driller.o dlmalloc.o

all: $(progs)

test_mmpi: test_mmpi.o $(libobjs)
test_fdproxy: test_fdproxy.o $(libobjs)
test_driller: test_driller.o driller.o dlmalloc.o
test_dlmalloc: test_dlmalloc.o dlmalloc.o

test_%.o: CPPFLAGS += -U NDEBUG
test_dlmalloc.o: CPPFLAGS += -D NODRILL
dlmalloc.o: CPPFLAGS += -D DEFAULT_GRANULARITY='((size_t)1U<<20)'
dlmalloc.o driller.o: CPPFLAGS += -D MSPACES

test_driller test_mmpi test_fdproxy: LDFLAGS += -ldl

clean:
	$(RM) *.o $(progs) *.gcov *.gcda *.gcno core.*

check: $(progs)
	for p in $(progs); do \
		sh $$p.sh ; \
	done

.PHONY: clean check
