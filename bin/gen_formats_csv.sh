#!/bin/sh

set -eu

DIR="$(cd -- "$(dirname "$0")" >/dev/null 2>&1 ; pwd -P)"
SRC_ROOT=${1:-${DIR%/bin}}

# Input
u_formats_h=$SRC_ROOT/src/util/format/u_formats.h
u_format_csv=$SRC_ROOT/src/util/format/u_format.csv

if [ ! -e "$u_formats_h" ]; then
  echo "No such file or directory: $u_formats_h"
  exit 1
fi

if [ ! -e "$u_format_csv" ]; then
  echo "No such file or directory: $u_format_csv"
  exit 1
fi

# Intermediate
_pipe_format_enums=$(mktemp -t FMT.enums.XXXXXXXXXX)
_util_format_csv=$(mktemp -t FMT.csv.XXXXXXXXXX)

# Purify source A
sed -n '/^enum pipe_format {/, /^   PIPE_FORMAT_COUNT/{/PIPE_FORMAT_COUNT/!p}' $u_formats_h |
  sed -n '/^   PIPE_FORMAT_/p' |
    tr -d ' \t' |
      awk -F',' '{printf "%d, 0x%x,%s\n", NR-1, NR-1, $1}' |
        sort -t',' -k3 >"$_pipe_format_enums"

# Purify source B
sed -n '/^PIPE_FORMAT_/p' $u_format_csv |
  sed 's/ \{1,\}//' |
    sort -t',' -k1 >"$_util_format_csv"

# Output

# Join A and B. Select source A file FIELD 3 (name per enum pipe_format) as join field
# Hence the order of input files matters
join -i -a 1 -1 3 -2 1 -t',' "$_pipe_format_enums" "$_util_format_csv" |
  sort -t',' -k2 -n
