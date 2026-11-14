# syntax=docker/dockerfile:1
# SPDX-License-Identifier: MIT
# Copyright (c) 2026, Canaan Creative Co., Ltd.
#
# For the licensing terms, see the LICENSE file (MIT License).
#
# NO WARRANTY. The Software is provided "AS IS", without warranty of any kind, express or
# implied, including but not limited to the warranties of merchantability, fitness for a
# particular purpose and non-infringement. In no event shall the authors or copyright
# holders be liable for any claim, damages or other liability arising from, out of or in
# connection with the Software or the use or other dealings in the Software.

# Ubuntu 22.04: the pinned Yocto pseudo fails do_package on glibc 2.39.

FROM ubuntu@sha256:0e0a0fc6d18feda9db1590da249ac93e8d5abfea8f4c3c0c849ce512b5ef8982 AS toolchain
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        curl wget ca-certificates xz-utils python3 file \
    && rm -rf /var/lib/apt/lists/*

# XUANTIE_YOCTO_* must match build.sh; the toolchains are fetched only here.
ARG XUANTIE_YOCTO_URL="https://github.com/XUANTIE-RV/xuantie-yocto/archive/0ff6ba270ee1838da932d20c7e20352d15e984a0.tar.gz"
ARG XUANTIE_YOCTO_SHA256="7b63e9eb8f51f25462c2e3df7e338306b0099e8b31b0b1045a73d79b8989f71b"
ARG XUANTIE_YOCTO_DIR="xuantie-yocto-0ff6ba270ee1838da932d20c7e20352d15e984a0"
ARG XUANTIE_TOOLCHAIN_URL="https://yocbook.oss-cn-hangzhou.aliyuncs.com/linux_image/XuanTie_V1.3.2"
WORKDIR /stage
RUN curl -fL --retry 8 --retry-all-errors --retry-delay 5 \
        --speed-limit 10240 --speed-time 60 -o yocto.tar.gz "$XUANTIE_YOCTO_URL" \
    && echo "$XUANTIE_YOCTO_SHA256  yocto.tar.gz" | sha256sum -c - \
    && tar -xzf yocto.tar.gz \
    && cp -a "$XUANTIE_YOCTO_DIR/meta-xuantie" /stage/meta-xuantie \
    && rm -rf yocto.tar.gz "$XUANTIE_YOCTO_DIR"
RUN mkdir -p /stage/xuantie-build/downloads && cd /stage/xuantie-build/downloads \
    && printf '%s  %s\n' \
        0c7baf2e9fcd202487775730f11e8357d444e5a149aab20c53586094c0aae7ec \
        Xuantie-900-gcc-linux-6.6.36-glibc-x86_64-V3.4.1.tar.gz \
        b3ded2166a06fdc84686c3fb48ba72f9ee71aa302c953b516c5fa3bbf89571dc \
        Xuantie-900-llvm-linux-6.6.36-glibc-x86_64-V2.4.2.tar.gz \
        f60dfafd842827d9dda4185220fa1d042a0460bbfb4c3104f1c511d38bd70c04 \
        Xuantie-900-gcc-linux-6.6.36-musl64-x86_64-V3.4.1.tar.gz \
        c5d02b622fbde15bcecba69f1d882ce9344808da6f7c2ff5c699ef93e3076828 \
        Xuantie-900-gcc-linux-6.6.36-musl32-x86_64-V3.4.1.tar.gz \
        > SHA256SUMS \
    && while read -r sum name; do \
        curl -fL --retry 8 --retry-all-errors --retry-delay 5 \
            --speed-limit 10240 --speed-time 60 -o "$name" \
            "$XUANTIE_TOOLCHAIN_URL/$name" || exit 1; \
    done < SHA256SUMS \
    && sha256sum -c SHA256SUMS
# Without XUANTIE_TOOLCHAIN_CONFIG the script leaves the gcc and clang trees empty.
RUN XUANTIE_TOOLCHAIN_CONFIG=6.6 \
    bash /stage/meta-xuantie/conf/distro/include/xuantie-toolchain-init.sh \
    && test -d /stage/riscv-toolchain-gcc/bin \
    && test -d /stage/riscv-toolchain-clang/bin \
    && rm -rf /stage/xuantie-build/downloads

FROM ubuntu@sha256:0e0a0fc6d18feda9db1590da249ac93e8d5abfea8f4c3c0c849ce512b5ef8982
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        gawk wget git diffstat texinfo gcc build-essential chrpath \
        socat cpio python3 python3-pip python3-venv python3-pexpect \
        python3-git python3-jinja2 python3-subunit python3-cryptography \
        xz-utils debianutils \
        iputils-ping libacl1 file zstd lz4 liblz4-tool bzip2 \
        ca-certificates locales gosu curl \
        bison flex quilt repo dwarves python-is-python3 \
        liblzo2-2 cmake libssl-dev fakeroot busybox-static \
    && rm -rf /var/lib/apt/lists/*
RUN locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8

# Ubuntu's python3-pycryptodome provides Cryptodome, not Crypto.
RUN pip3 install --no-cache-dir pycryptodome==3.23.0 ubi_reader==0.8.10 lzallright==0.2.6

ARG SHELLCHECK_VERSION=v0.10.0
ARG SHELLCHECK_SHA256=6c881ab0698e4e6ea235245f22832860544f17ba386442fe7e9d629f8cbedf87
RUN curl -fsSL --retry 8 --retry-all-errors --retry-delay 5 -o /tmp/shellcheck.tar.xz \
        "https://github.com/koalaman/shellcheck/releases/download/$SHELLCHECK_VERSION/shellcheck-$SHELLCHECK_VERSION.linux.x86_64.tar.xz" \
    && echo "$SHELLCHECK_SHA256  /tmp/shellcheck.tar.xz" | sha256sum -c - \
    && tar -xJf /tmp/shellcheck.tar.xz -C /tmp \
    && install -m 0755 "/tmp/shellcheck-$SHELLCHECK_VERSION/shellcheck" /usr/local/bin/shellcheck \
    && rm -rf /tmp/shellcheck.tar.xz "/tmp/shellcheck-$SHELLCHECK_VERSION"

ARG NODE_VERSION=v24.22.1
ARG NODE_SHA256=6c172a9ab6718f7b801fb5a7bd486af2ee5e32758086a04b84fcce2aa77095e6
RUN curl -fsSL --retry 8 --retry-all-errors --retry-delay 5 -o /tmp/node.tar.xz \
        "https://nodejs.org/dist/$NODE_VERSION/node-$NODE_VERSION-linux-x64.tar.xz" \
    && echo "$NODE_SHA256  /tmp/node.tar.xz" | sha256sum -c - \
    && tar -xJf /tmp/node.tar.xz -C /usr/local/bin --strip-components=2 \
        "node-$NODE_VERSION-linux-x64/bin/node" \
    && rm -f /tmp/node.tar.xz

# XuanTie clang links libtinfo.so.5; libtinfo.so.6 is ABI-compatible.
RUN ln -sf /lib/x86_64-linux-gnu/libtinfo.so.6 /lib/x86_64-linux-gnu/libtinfo.so.5

COPY --from=toolchain /stage/riscv-toolchain-gcc /opt/canaan/toolchains/riscv-toolchain-gcc
COPY --from=toolchain /stage/riscv-toolchain-clang /opt/canaan/toolchains/riscv-toolchain-clang
COPY --from=toolchain /stage/riscv-toolchain-gcc-muslc \
    /opt/canaan/toolchains/riscv-toolchain-gcc-muslc

COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh
ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD ["bash"]
