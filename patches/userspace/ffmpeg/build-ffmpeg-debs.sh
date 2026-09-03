#!/bin/bash
# SPDX-License-Identifier: BSD-2-Clause-Patent
set -ex
export DEBIAN_FRONTEND=noninteractive
sed -i 's/^Types: deb$/Types: deb deb-src/' /etc/apt/sources.list.d/debian.sources
apt-get update -q
apt-get install -y -q --no-install-recommends dpkg-dev devscripts quilt fakeroot build-essential ca-certificates >/dev/null
cd /w
rm -rf ffmpeg-7.1.5* ffmpeg_7.1.5*
apt-get source ffmpeg
apt-get build-dep -y -q ffmpeg >/dev/null
cd ffmpeg-7.1.5*/
mkdir -p debian/patches
cp /w/vim3-ffmpeg-v4l2m2m.patch debian/patches/
echo vim3-ffmpeg-v4l2m2m.patch > debian/patches/series
DEBEMAIL="gus@bourg.net" DEBFULLNAME="Gus Bourg" dch --local +vim3 "v4l2m2m: graft jc-kynesim stateful-decoder/DRMPRIME rework (test/7.1.5/main subset) so Amlogic meson-vdec (and other stateful V4L2 decoders) work; adds SOURCE_CHANGE handshake and drm_prime output."
DEB_BUILD_OPTIONS="nocheck noautodbgsym parallel=4" dpkg-buildpackage -b -uc -us
echo BUILD-DONE
ls -la /w/*.deb | head -30
