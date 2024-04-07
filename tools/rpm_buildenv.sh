#!/bin/bash
# This script works with Fedora. While not tested, it may also work with its
# derivatives (CentOS, RHEL) or even other RPM-based distros, contributions
# fixing any deviations in package naming are welcome here.
# shellcheck disable=SC1091
set -o pipefail

case "$1" in
    name)
        echo "No build environment name required for RPM-based distros." >&2
        echo "This script installs the build dependencies via dnf using the \"setup\" option." >&2
        ;;

    setup)
        sudo dnf install -y \
            appstream \
            ccache \
            chrpath \
            cmake \
            desktop-file-utils \
            faad2 \
            ffmpeg-devel \
            fftw-devel \
            flac-devel \
            gcc-c++ \
            gmock-devel \
            google-benchmark-devel \
            gtest-devel \
            guidelines-support-library-devel \
            hidapi-devel \
            lame-devel \
            libchromaprint-devel \
            libebur128-devel \
            libid3tag-devel \
            libmad-devel \
            libmodplug-devel \
            libmp4v2-devel \
            libsndfile-devel \
            libusb1-devel \
            libvorbis-devel \
            lilv-devel \
            mesa-libGL-devel \
            mesa-libGLU-devel \
            ninja-build \
            opus-devel \
            opusfile-devel \
            portaudio-devel \
            portmidi-devel \
            protobuf-compiler \
            protobuf-lite-devel \
            qt6-qt{5compat,base,base-private,declarative,shadertools,svg}-devel \
            qtkeychain-qt6-devel \
            rubberband-devel \
            soundtouch-devel \
            sqlite-devel \
            taglib-devel \
            upower-devel \
            wavpack-devel \
            zlib-devel
        ;;
    *)
        echo "Usage: $0 [options]"
        echo ""
        echo "options:"
        echo "   help       Displays this help."
        echo "   name       Displays the name of the required build environment."
        echo "   setup      Installs the build environment."
        ;;
esac
