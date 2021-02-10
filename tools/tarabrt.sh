#!/bin/sh
set -eu

USAGE=$(cat <<EOF
Usage:

$ ./tools/tarabrt.sh -e ./src/tarantool -c core
# sysctl -w kernel.core_pattern="|/usr/share/tarantool/tools/tarabrt.sh -d /var/core -p %p -t %t"

EOF
)

# Parse CLI options.
OPTIONS=$(getopt -o c:d:e:hp:t: -n 'tarabrt.sh' -- "$@")
eval set -- "$OPTIONS"

while true; do
	case "$1" in
		-c) COREFILE=$2; shift 2;;
		-d) COREDIR=$2;  shift 2;;
		-e) BINARY=$2;   shift 2;;
		-p) PID=$2;      shift 2;;
		-t) TIME=$2;     shift 2;;
		--) shift; break;;
		-h) printf "%s\n", "$USAGE";
			exit 0;;
		*)  printf "Invalid option: $1\n%s\n", "$USAGE";
			exit 1;;
	esac
done

# Use default values for the remaining parameters.
COREFILE=${COREFILE:-${COREDIR}/tarantool-core.${PID}.${TIME}}
COREDIR=${COREDIR:-${PWD}}
BINARY=${BINARY:-$(/usr/bin/readlink /proc/"${PID}"/exe)}
TIME=${TIME:-$(date +%s)}
PID=${PID:-N}

if [ ! -e "${COREFILE}" ]; then
	cat > "${COREFILE}"
fi

GDBERROR=$(cat <<EOF
gdb is not installed, but it is obligatory for collecting the
loaded shared libraries from the coredump.

You can proceed collecting the artefacts manually later by running
the following command:
$ tarabrt.sh -e $BINARY -c $COREFILE
EOF
)

# Check that gdb is installed.
type gdb &> /dev/null || ([ -t 1 ] && echo "$GDBERROR")

# Resolve hostname if possible.
HOSTNAME=$(timeout 2>/dev/null -s 9 1 hostname -f \
	|| hostname 2>/dev/null                   \
	|| echo hostname)

# Proceed with collecting and packing artefacts.
TMPDIR=$(mktemp -d -p "${COREDIR}")
TARLIST=${TMPDIR}/tarlist
VERSION=${TMPDIR}/version
ARCHIVENAME=${COREDIR}/tarantool-core-${PID}-$(date +%Y%m%d%H%M -d @"${TIME}")-${HOSTNAME%%.*}.tar.gz

# Dump the version to checkout the right commit later.
$BINARY --version > "$VERSION"

# Collect the most important artefacts.
{
	echo "$BINARY"
	echo "$COREFILE"
	echo "$VERSION"
} >> "${TARLIST}"

SEPARATOR1="Shared Object Library"
SEPARATOR2="Shared library is missing debugging information"
# XXX: This is kinda "postmortem ldd": the command below dumps the
# full list of the shared libraries the binary is linked against
# or those loaded via dlopen at the platform runtime.
# This is black woodoo magic. Do not touch. You are warned.
gdb -batch "${BINARY}" -c "${COREFILE}" -ex "info shared" -ex "quit" | \
	sed -n "/${SEPARATOR1}/,/${SEPARATOR2}/p;/${SEPARATOR2}/q"   | \
	awk '{ print $NF }' | grep "^/" >> "${TARLIST}"

# Pack everything listed in TARLIST file into a tarball. To unify
# the archive format BINARY, COREFILE, VERSION and TARLIST are
# renamed while packing.
tar -czhf "${ARCHIVENAME}" -P -T "${TARLIST}" \
	--transform="s|$BINARY|tarantool|"    \
	--transform="s|$COREFILE|coredump|"   \
	--transform="s|$TARLIST|checklist|"   \
	--transform="s|$VERSION|version|"     \
	--add-file="${TARLIST}"

[ -t 1 ] && echo "Archive: ${ARCHIVENAME}"

# Cleanup temporary files.
[ -f "${TARLIST}" ] && rm -f "${TARLIST}"
[ -f "${VERSION}" ] && rm -f "${VERSION}"
[ -d "${TMPDIR}" ] && rmdir "${TMPDIR}"
