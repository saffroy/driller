CC := gcc
CFLAGS := -Wall -O0 -g
CPPFLAGS := -D DEBUG -D _GNU_SOURCE

LD := gcc
LDFLAGS :=

progs := test_mmpi test_fdproxy test_driller test_dlmalloc

all: $(progs)

test_mmpi: test_mmpi.o mmpi.o
test_fdproxy: test_fdproxy.o fdproxy.o mmpi.o
test_driller: test_driller.o driller.o dlmalloc.o
test_dlmalloc: test_dlmalloc.o dlmalloc.o

test_dlmalloc.o: CPPFLAGS += -D NODRILL
dlmalloc.o: CPPFLAGS += -D DEFAULT_GRANULARITY='((size_t)1U<<20)'
dlmalloc.o driller.o: CPPFLAGS += -D MSPACES

test_driller: LDFLAGS += -ldl

clean:
	$(RM) *.o $(progs)

check: $(progs)
	for p in $(progs); do \
		sh $$p.sh ; \
	done

