#!/bin/bash
set -x

nprocs=${1:-2}

for i in $(seq 0 $((nprocs-1)) ); do
        ./test_fdproxy 0 $nprocs $i &
	if [ $i = 0 ]; then
		sleep 1;
	fi
done
wait
