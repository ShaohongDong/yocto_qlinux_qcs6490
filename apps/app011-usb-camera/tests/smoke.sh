#!/bin/sh
# SPDX-License-Identifier: MIT
set -eu
binary=${1:?binary required}
"$binary" --self-test
"$binary" --headless --test-source --duration 2
if "$binary" --headless --device /dev/q6a-usb-camera-nonexistent --duration 2; then
    echo 'Missing camera unexpectedly succeeded' >&2
    exit 1
fi
if "$binary" --headless --duration nan; then exit 1; fi
