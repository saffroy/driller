#!/bin/bash

# usage: CC=... ./depend.sh <foo/bar.d> [flags] <foo/bar.c>
#set -x

if [ -z "$CC" ]; then
    echo "error: CC is not set" >&2
    exit 1
fi

target="$1"
shift
dir=$(dirname "$target")

case "$dir" in
    ""|".")
	subst="s|\(.*\).o:|\1.d \1.o:|"
	;;
    *)
	subst="s|\(.*\).o:|$dir/\1.d $dir/\1.o:|"
	[ -d "$dir" ] || mkdir -p "$dir"
	;;
esac

"$CC" -MM -MG "$@" | sed -e "$subst" > "$target"
