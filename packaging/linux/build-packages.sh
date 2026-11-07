#!/usr/bin/env bash
# Builds ewr's .deb and .rpm from a finished build (the binary CMake pins to
# the repository root). Run from the repository root:
#
#   packaging/linux/build-packages.sh <version> <deb-arch> <rpm-arch> <out-dir>
#
#   deb-arch: amd64 | arm64 | armhf      rpm-arch: x86_64 | aarch64 | armv7hl
#
# Layout: /usr/bin/ewr, the database as a read-only seed in /usr/share/ewr
# (ewr copies it into the user's folder and keeps that copy updated), the
# udev rule that lets the logged-in user reach the printer without sudo.
# The C ABI is not packaged: it ships in the release's one EWR-SDK archive.
set -euo pipefail

version="$1"; deb_arch="$2"; rpm_arch="$3"; out="$4"
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"

case "$deb_arch" in
    amd64|arm64|armhf) ;;
    *) echo "unknown deb arch $deb_arch" >&2; exit 2 ;;
esac

mkdir -p "$out"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

summary="Resets the waste ink pad counters of Epson inkjet printers"
description="EWR reads a printer's counters, asks once, writes them over USB or the
 network, and reads them back. It keeps a backup of every byte it overwrites,
 restores from it, and runs head cleanings and nozzle checks."

# ---- the tool ---------------------------------------------------------------
tool="$work/ewr"
install -Dm755 "$root/ewr" "$tool/usr/bin/ewr"
install -Dm644 "$root/database.json" "$tool/usr/share/ewr/database.json"
install -Dm644 "$here/70-ewr.rules" "$tool/usr/lib/udev/rules.d/70-ewr.rules"
install -Dm644 "$root/README.md" "$tool/usr/share/doc/ewr/README.md"
install -Dm644 "$root/LICENSE" "$tool/usr/share/doc/ewr/copyright"

# ---- .deb ---------------------------------------------------------------------
reload='if command -v udevadm >/dev/null 2>&1; then
    udevadm control --reload-rules || true
    udevadm trigger --subsystem-match=usb --attr-match=idVendor=04b8 || true
fi'

mkdir -p "$tool/DEBIAN"
cat > "$tool/DEBIAN/control" <<EOF
Package: ewr
Version: $version
Architecture: $deb_arch
Maintainer: EWR <https://github.com/RxNaison/Epson-Waste-Reset>
Homepage: https://github.com/RxNaison/Epson-Waste-Reset
Section: utils
Priority: optional
Depends: libc6 (>= 2.31), libstdc++6, libcurl4
Description: $summary
 $description
EOF
printf '#!/bin/sh\nset -e\n%s\n' "$reload" > "$tool/DEBIAN/postinst"
chmod 755 "$tool/DEBIAN/postinst"
dpkg-deb --root-owner-group --build "$tool" "$out/ewr_${version}_${deb_arch}.deb"
rm -rf "$tool/DEBIAN"

# ---- .rpm ---------------------------------------------------------------------
# A spec that installs the staged tree as it is.
rpmtop="$work/rpm"
mkdir -p "$rpmtop"/{BUILD,RPMS,SPECS,SRPMS,SOURCES}
cat > "$rpmtop/SPECS/ewr.spec" <<EOF
Name: ewr
Version: $version
Release: 1
Summary: $summary
License: Apache-2.0
URL: https://github.com/RxNaison/Epson-Waste-Reset
Requires: libcurl

%description
$description

%install
mkdir -p %{buildroot}
cp -a $tool/. %{buildroot}/

%post
$reload

%files
/usr/bin/ewr
/usr/share/ewr/database.json
/usr/lib/udev/rules.d/70-ewr.rules
/usr/share/doc/ewr
EOF
rpmbuild --quiet --define "_topdir $rpmtop" --define "__strip /bin/true" --target "$rpm_arch" -bb "$rpmtop/SPECS/ewr.spec"
find "$rpmtop/RPMS" -name '*.rpm' -exec cp {} "$out/" \;

ls -l "$out"
