#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 VERSION DIST_SLUG" >&2
    exit 2
fi

PACKAGE_VERSION="$1"
DIST_SLUG="$2"

cd /work/source

make clean
make
make test

ARCH="$(dpkg --print-architecture)"
PKGROOT="/tmp/json-graylog-tcp-logger"
OUT="/work/json-graylog-tcp-logger_${PACKAGE_VERSION}_${DIST_SLUG}_${ARCH}.deb"

rm -rf "$PKGROOT"
mkdir -p "$PKGROOT/DEBIAN" "$PKGROOT/usr/bin"
install -m 0755 GELFsender "$PKGROOT/usr/bin/GELFsender"

cat > "$PKGROOT/DEBIAN/control" <<EOF
Package: json-graylog-tcp-logger
Version: ${PACKAGE_VERSION}
Section: utils
Priority: optional
Architecture: ${ARCH}
Depends: libc6
Maintainer: Darek Margas <darek.margas@gmail.com>
Description: STDIN to Graylog TCP logger
 Reads newline-terminated messages from standard input, converts the line
 terminator to the NUL delimiter required by Graylog TCP input, and sends
 messages to one of up to two Graylog servers with failover and buffering.
EOF

dpkg-deb --build --root-owner-group "$PKGROOT" "$OUT"

echo "Package contents:"
dpkg-deb -c "$OUT"
dpkg-deb -c "$OUT" | grep -Eq '[.]?/usr/bin/GELFsender$'

echo "Built: $OUT"
dpkg-deb -f "$OUT" Package Version Architecture Depends
