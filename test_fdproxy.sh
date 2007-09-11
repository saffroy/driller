#!/bin/bash
set -x

nprocs=${1:-2}
niter=${2:-10000}

for i in $(seq 0 $((nprocs-1)) ); do
	#strace -fo strace-$i ./test_fdproxy 0 $nprocs $i $niter &
	./test_fdproxy 0 $nprocs $i $niter &
done
wait
