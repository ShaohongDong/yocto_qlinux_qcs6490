# SPDX-License-Identifier: MIT
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI:append:radxa-dragon-q6a = " file://0001-glamor-import-gbm-pixmaps-through-dma-buf.patch \
    file://0002-glx-invalidate-context-after-provider-probe.patch"
