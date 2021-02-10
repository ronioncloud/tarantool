#!/bin/sh
set -eu

# Check that gdb is installed.
type gdb &> /dev/null

VERSION=${PWD}/version

[ -f "$VERSIONFILE" ] || (echo 'Invalid coredump location'; exit 1)

REVISION=$(grep -oP 'Tarantool \d+\.\d+\.\d+-\d+-g\K[a-f0-9]+' "$VERSION")
cat <<EOF
================================================================================

Do not forget to properly setup the environment:
* git clone https://github.com/tarantool/tarantool.git sources
* cd !$
* git checkout $REVISION
* git submodule update --recursive --init

================================================================================
EOF

# Define the build path to be substituted with the source path.
# XXX: Check the absolute path on the function <main> definition
# considering it is located in src/main.cc within Tarantool repo.
SUBPATH=$(gdb -batch ./tarantool -ex 'info line main' | \
	grep -oP 'Line \d+ of \"\K.+(?=\/src\/main\.cc\")')

gdb ./tarantool \
    -ex "set sysroot $(realpath .)" \
    -ex "set substitute-path $SUBPATH sources" \
    -ex 'core coredump'
