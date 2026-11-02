#!/usr/bin/env python3
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Deploys ec-builder Docker image to a Satlab station with persistence."""

import argparse
import os
import shlex
import subprocess
import sys
import tempfile
from typing import List, Optional


DEFAULT_REGISTRY_IMAGE = (
    "us-docker.pkg.dev/chromeos-partner-moblab/satlab/ec-builder"
)
SETUP_SCRIPT_NAME = "setup_satlab.sh"


def run_cmd(
    cmd: List[str], check: bool = True, capture_output: bool = False
) -> subprocess.CompletedProcess:
    """Executes a command and logs failure details if check fails."""
    try:
        return subprocess.run(
            cmd, check=check, text=True, capture_output=capture_output
        )
    except subprocess.CalledProcessError as err:
        print(f"Error executing command: {' '.join(cmd)}", file=sys.stderr)
        if err.stdout:
            print(f"STDOUT:\n{err.stdout}", file=sys.stderr)
        if err.stderr:
            print(f"STDERR:\n{err.stderr}", file=sys.stderr)
        raise


def run_ssh_cmd(
    target_host: str,
    cmd: List[str],
    check: bool = True,
) -> subprocess.CompletedProcess:
    """Executes a single command over SSH on the Satlab host with PATH set."""
    quoted_cmd = [shlex.quote(arg) for arg in cmd]
    return run_cmd(
        ["ssh", target_host, "PATH=/usr/local/bin:$PATH", *quoted_cmd],
        check=check,
    )


def deploy_remote(target_host: str, remote_image: Optional[str]) -> int:
    """Deploys and configures persistence on a Satlab station over SSH."""
    print(f"=== Deploying ec-builder to {target_host} ===")

    if remote_image:
        print(f"[*] Pulling {remote_image} on {target_host}...")
        run_ssh_cmd(target_host, ["docker", "pull", remote_image])
        run_ssh_cmd(
            target_host, ["docker", "tag", remote_image, "ec-builder:latest"]
        )
    else:
        with tempfile.NamedTemporaryFile(
            suffix="_ec-builder.tar.gz", delete=False
        ) as tmp_file:
            tar_path = tmp_file.name

        try:
            print(f"[*] Saving local ec-builder:latest image to {tar_path}...")
            with open(tar_path, "wb") as f_out:
                with (
                    subprocess.Popen(
                        ["docker", "save", "ec-builder:latest"],
                        stdout=subprocess.PIPE,
                    ) as p1,
                    subprocess.Popen(
                        ["gzip"], stdin=p1.stdout, stdout=f_out
                    ) as p2,
                ):
                    if p1.stdout:
                        p1.stdout.close()
                    p2.communicate()
                if p1.returncode != 0 or p2.returncode != 0:
                    raise RuntimeError("Failed to save and compress image")

            remote_tar = "/tmp/ec-builder_image.tar.gz"
            print(f"[*] Transferring image to {target_host}:{remote_tar}...")
            run_cmd(["scp", tar_path, f"{target_host}:{remote_tar}"])

            print(f"[*] Loading image on {target_host}...")
            run_ssh_cmd(target_host, ["docker", "load", "-i", remote_tar])
        finally:
            if os.path.exists(tar_path):
                os.remove(tar_path)

    print(f"[*] Configuring persistence on {target_host}...")
    local_setup_script = os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        SETUP_SCRIPT_NAME,
    )
    remote_setup_script = f"/tmp/{SETUP_SCRIPT_NAME}"
    run_cmd(
        [
            "scp",
            "-p",
            local_setup_script,
            f"{target_host}:{remote_setup_script}",
        ]
    )
    try:
        run_ssh_cmd(target_host, [remote_setup_script, remote_image])
    finally:
        run_ssh_cmd(target_host, ["rm", "-f", remote_setup_script], check=False)
        run_ssh_cmd(target_host, ["rm", "-f", remote_tar], check=False)

    print(f"=== Deployment to {target_host} Complete! ===")
    return 0


def main() -> int:
    """Main CLI entry point."""
    parser = argparse.ArgumentParser(
        description="Deploy ec-builder Docker image to a Satlab station."
    )
    parser.add_argument(
        "hostname",
        help="Satlab hostname or IP address",
    )
    parser.add_argument(
        "--pull",
        action="store_true",
        help="Pull released image from Artifact Registry directly on Satlab",
    )
    parser.add_argument(
        "--tag",
        default="release",
        help="Image tag to pull when --pull is specified (default: release)",
    )

    args = parser.parse_args()

    remote_image = None
    if args.pull:
        remote_image = f"{DEFAULT_REGISTRY_IMAGE}:{args.tag}"

    return deploy_remote(args.hostname, remote_image)


if __name__ == "__main__":
    sys.exit(main())
