#!/bin/bash
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Configures a Satlab station with a keepalive container labeled to survive
# Satlab's boot-time docker prune.

set -euo pipefail

IMAGE_TAG="${1:-ec-builder:latest}"
CONTAINER_NAME="ec-builder-keepalive"
PRUNE_LABEL="skip.while.pruning.docker.system=yes"

echo "[*] Setting up reboot-persistent container for ${IMAGE_TAG}..."
if [[ -n "$(docker ps -aq --filter "name=^/${CONTAINER_NAME}$")" ]]; then
  docker rm -f "${CONTAINER_NAME}"
fi

docker create \
  --name "${CONTAINER_NAME}" \
  --label "${PRUNE_LABEL}" \
  "${IMAGE_TAG}"

echo "[*] Verifying Docker images and keepalive container:"
docker images "${IMAGE_TAG}"
docker ps -a --filter "name=^/${CONTAINER_NAME}$"

echo "[*] ec-builder is now persistent across Satlab reboots."
