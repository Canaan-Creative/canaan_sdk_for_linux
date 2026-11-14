#!/bin/bash
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

# Install Docker CE and add the caller to the docker group; ./build.sh runs it on Ubuntu when
# Docker is missing or the caller is not in the docker group.
#   docker/install-docker.sh
# A build needs 50 GB of free disk.
set -euo pipefail

if command -v docker >/dev/null 2>&1; then
    echo "==> docker already installed: $(docker --version)"
else
    echo "==> installing Docker CE + BuildKit"
    sudo apt-get update && sudo apt-get install -y ca-certificates curl
    sudo install -m 0755 -d /etc/apt/keyrings
    sudo curl -fsSL https://download.docker.com/linux/ubuntu/gpg -o /etc/apt/keyrings/docker.asc
    sudo chmod a+r /etc/apt/keyrings/docker.asc
    codename="$(. /etc/os-release && echo "$VERSION_CODENAME")"
    echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.asc]" \
        "https://download.docker.com/linux/ubuntu $codename stable" |
        sudo tee /etc/apt/sources.list.d/docker.list > /dev/null
    sudo apt-get update
    sudo apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin
fi

if id -nG "$USER" | grep -qw docker; then
    echo "==> $USER already in docker group"
else
    echo "==> adding $USER to docker group (./build.sh uses it at once; other shells after" \
        "the next login)"
    sudo usermod -aG docker "$USER"
fi

echo "==> verifying"
if id -nG | grep -qw docker; then
    docker run --rm hello-world
else
    sg docker -c "docker run --rm hello-world"
fi

echo "==> done"
