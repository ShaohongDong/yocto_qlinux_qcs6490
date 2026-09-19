# SPDX-License-Identifier: MIT
SUMMARY = "Common maintenance and board diagnostics tools for Radxa Dragon"
LICENSE = "MIT"

inherit packagegroup

RDEPENDS:${PN} = " \
    net-tools \
    iproute2-ip \
    iproute2-ss \
    iputils-ping \
    iputils-arping \
    iputils-tracepath \
    bind-utils \
    traceroute \
    ethtool \
    iw \
    iperf3 \
    tcpdump \
    curl \
    wget \
    ca-certificates \
    rsync \
    htop \
    lsof \
    strace \
    procps \
    file \
    less \
    nano \
    tmux \
    jq \
    findutils \
    diffutils \
    tar \
    gzip \
    bzip2 \
    xz \
    zip \
    unzip \
    usbutils \
    pciutils \
    i2c-tools \
    picocom \
"
