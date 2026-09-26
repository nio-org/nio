#!/bin/sh
# Builds NioTranslation.swift into native/libNioTranslation.dylib, which
# translation.load() opens at run time. This is the only step that needs
# swiftc. A program that imports the library builds with clang alone, and
# answers "unavailable" until this dylib exists. The framework needs macOS 15,
# so that is also the deployment target.
#
# The dylib carries swiftc's ad-hoc linker signature, which a notarized bundle
# rejects. `nio package` signs each Mach-O it copies into Resources with the
# bundle's identity, so a rebuilt dylib does not break a release.
set -e
cd "$(dirname "$0")"
arch="$(uname -m)"
swiftc -O -emit-library -module-name NioTranslation \
    -target "$arch-apple-macos15.0" \
    native/NioTranslation.swift \
    -o native/libNioTranslation.dylib
echo "built native/libNioTranslation.dylib"
