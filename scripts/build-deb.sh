#!/bin/sh
set -eu

if [ "$#" -ne 2 ]; then
    echo "usage: $0 VERSION DIST_SLUG" >&2
    exit 2
fi

VERSION="$1"
DIST_SLUG="$2"

export DEBIAN_FRONTEND=noninteractive

apt_install() {
    attempt=1
    while [ "$attempt" -le 3 ]; do
        rm -rf /var/lib/apt/lists/*
        if timeout 180 apt-get -o Acquire::Retries=3 update &&
           timeout 300 apt-get -o Acquire::Retries=3 install -y --no-install-recommends \
               build-essential \
               debhelper \
               dpkg-dev; then
            return 0
        fi
        echo "apt failed on attempt $attempt, retrying..." >&2
        attempt=$((attempt + 1))
        sleep 5
    done
    return 1
}

apt_install

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
