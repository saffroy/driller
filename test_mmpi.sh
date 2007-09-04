#!/bin/bash
set -x

nprocs=${1:-2}
iter=${2:-1000000}

for i in $(seq 0 $((nprocs-1)) ); do
        ./test_mmpi 0 $nprocs $i $iter &
	if [ $i = 0 ]; then
		sleep 1;
	fi
done
wait
