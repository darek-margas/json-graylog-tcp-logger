#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 VERSION DIST_SLUG" >&2
    exit 2
fi

VERSION="$1"
DIST_SLUG="$2"

apt-get update
apt-get install -y --no-install-recommends \
    build-essential \
    debhelper \
    devscripts \
    dpkg-dev \
    ca-certificates

cd /work/source

make clean
make
make test

cat > debian/changelog <<EOF
json-graylog-tcp-logger (${VERSION}) unstable; urgency=medium

  * Build ${VERSION} for ${DIST_SLUG}.

 -- Darek Margas <darek.margas@gmail.com>  $(date -R)
EOF

dpkg-buildpackage -us -uc -b

DEB="$(find .. -maxdepth 1 -type f -name 'json-graylog-tcp-logger_*.deb' -print -quit)"
test -n "$DEB"

echo "Package contents:"
dpkg-deb -c "$DEB"

if ! dpkg-deb -c "$DEB" | grep -Eq '[.]?/usr/bin/GELFsender$'; then
    echo "ERROR: package does not contain /usr/bin/GELFsender" >&2
    exit 1
fi

ARCH="$(dpkg-deb -f "$DEB" Architecture)"
OUT="../json-graylog-tcp-logger_${VERSION}_${DIST_SLUG}_${ARCH}.deb"
cp "$DEB" "$OUT"

echo "Built: $OUT"
dpkg-deb -f "$OUT" Package Version Architecture Depends
