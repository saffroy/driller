#!/bin/bash
set -x

nprocs=${1:-2}
niter=${2:-1000}

for i in $(seq 0 $((nprocs-1)) ); do
        ./test_fdproxy 0 $nprocs $i $niter &
done
wait
