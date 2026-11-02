#!/usr/bin/env python3
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Builds and publishes ec-builder Docker image to Google Artifact Registry."""

import argparse
import datetime
import os
import subprocess
import sys
from typing import List


DEFAULT_REGISTRY = "us-docker.pkg.dev/chromeos-partner-moblab/satlab"


def run_cmd(cmd: List[str], check: bool = True) -> subprocess.CompletedProcess:
    """Executes a subprocess command, displaying it on failure."""
    try:
        return subprocess.run(cmd, check=check, text=True)
    except subprocess.CalledProcessError as err:
        print(f"Error executing command: {' '.join(cmd)}", file=sys.stderr)
        raise err


def get_git_sha() -> str:
    """Returns short git SHA or 'unknown' if git fails."""
    try:
        res = subprocess.run(
            ["git", "rev-parse", "--short", "HEAD"],
            check=True,
            capture_output=True,
            text=True,
        )
        return res.stdout.strip()
    except subprocess.SubprocessError:
        return "unknown"


def main() -> int:
    """Main CLI entry point."""
    parser = argparse.ArgumentParser(
        description=(
            "Build and publish ec-builder image to Google Artifact Registry."
        )
    )
    parser.add_argument(
        "--registry",
        default=os.environ.get("REGISTRY_URI", DEFAULT_REGISTRY),
        help=f"Artifact Registry URI (default: {DEFAULT_REGISTRY})",
    )
    parser.add_argument(
        "--tag",
        default=os.environ.get("IMAGE_TAG", "release"),
        help="Release tag to publish (default: release)",
    )
    parser.add_argument(
        "--build",
        action="store_true",
        help="Build ec-builder:latest before tagging and publishing",
    )

    args = parser.parse_args()

    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)

    git_sha = get_git_sha()
    timestamp = datetime.datetime.now(datetime.timezone.utc).strftime(
        "%Y%m%d%H%M%S"
    )
    version_tag = f"{timestamp}-{git_sha}"
    image_base = f"{args.registry}/ec-builder"

    print("==================================================")
    print("Publishing ec-builder Docker image:")
    print(f"  Registry:    {args.registry}")
    print(f"  Image Name:  {image_base}")
    print(f"  Tags:        {args.tag}, {version_tag}, latest")
    print("==================================================")

    if args.build:
        print("[*] Building ec-builder:latest...")
        run_cmd(["docker", "build", "-t", "ec-builder:latest", "."])

    # Tag images
    for tag_name in [args.tag, version_tag, "latest"]:
        run_cmd(
            ["docker", "tag", "ec-builder:latest", f"{image_base}:{tag_name}"]
        )

    # Push images
    print("[*] Pushing images to Artifact Registry...")
    for tag_name in [args.tag, version_tag, "latest"]:
        run_cmd(["docker", "push", f"{image_base}:{tag_name}"])

    print("==================================================")
    print("Successfully published:")
    print(f"  - {image_base}:{args.tag}")
    print(f"  - {image_base}:{version_tag}")
    print(f"  - {image_base}:latest")
    print("==================================================")
    return 0


if __name__ == "__main__":
    sys.exit(main())
