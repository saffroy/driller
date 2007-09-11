#!/bin/bash
set -x

nprocs=${1:-2}
iter=${2:-1000000}

for i in $(seq 0 $((nprocs-1)) ); do
	#strace -fo strace-$i ./test_mmpi 0 $nprocs $i $iter &
	./test_mmpi 0 $nprocs $i $iter &
done
wait
