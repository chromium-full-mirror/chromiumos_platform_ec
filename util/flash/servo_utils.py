# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Servo communication wrapper utilities for EC flashing."""

import logging
import os
import re
import signal
import subprocess
import time
from typing import List, Optional, Tuple


logger = logging.getLogger(__name__)

SKIP_PROCESS_REGEX = re.compile(r"(servod|grpc_server_setup|/sbin/init)")


class ServoError(Exception):
    """Exception raised for errors communicating with servod."""


class ServoDriver:
    """Wrapper around dut-control to interact with a Servo instance."""

    def __init__(
        self,
        host: str = "localhost",
        port: int = 9999,
        dry_run: bool = False,
    ):
        self.host = host
        self.port = port
        self.dry_run = dry_run
        self._saved_state: List[Tuple[str, str]] = []
        self._effective_servo_type: Optional[str] = None
        self.prefix = ""
        self._detect_prefix()
        self._frozen_pids: List[int] = []

    def __enter__(self) -> "ServoDriver":
        return self

    def __exit__(self, exc_type, exc_val, exc_tb) -> None:
        self.restore_state()

    @property
    def frozen_pids(self) -> List[int]:
        """Return list of PIDs currently frozen by claim_pty."""
        return list(self._frozen_pids)

    def _build_cmd(self, args: List[str]) -> List[str]:
        """Construct the base dut-control command."""
        cmd = ["dut-control", f"--port={self.port}"]
        if self.host and self.host != "localhost":
            cmd.append(f"--host={self.host}")
        cmd.extend(args)
        return cmd

    def _run_dut_control(
        self, args: List[str], check: bool = True
    ) -> subprocess.CompletedProcess:
        """Execute dut-control and return the completed process."""
        cmd = self._build_cmd(args)
        logger.debug("Executing: %s", " ".join(cmd))

        if self.dry_run and any(":" in arg for arg in args):
            logger.info("[DRY RUN] Would execute: %s", " ".join(cmd))
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        try:
            return subprocess.run(
                cmd,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                encoding="utf-8",
                check=check,
            )
        except subprocess.CalledProcessError as err:
            logger.debug(
                "dut-control failed: %s (stderr: %s)",
                err,
                err.stderr.strip(),
            )
            raise ServoError(
                f"Failed to execute dut-control {args}: {err.stderr.strip()}"
            ) from err

    def _detect_prefix(self) -> None:
        """Determine if a control prefix is required for dual-v4 setups."""
        try:
            servo_type = self.get("servo_type")
            if "servo_v4" in servo_type and "_and_" in servo_type:
                active = self.get("active_dut_controller")
                self._effective_servo_type = f"servo_v4_with_{active}"
                second_dev = servo_type.split("_and_")[-1]
                if active == second_dev:
                    self.prefix = f"{active}."
                    logger.debug(
                        "Detected dual servo setup. Using prefix: %s",
                        self.prefix,
                    )
        except ServoError:
            logger.debug(
                "Could not auto-detect servo_type for prefixing. "
                "Assuming none."
            )

    def has_control(self, control: str) -> bool:
        """Check if the connected servo supports a specific control."""
        prefixed_ctrl = f"{self.prefix}{control}" if self.prefix else control
        res = self._run_dut_control(["-i", prefixed_ctrl], check=False)
        return res.returncode == 0

    def get(self, control: str, default: Optional[str] = None) -> str:
        """Query the value of a single servo control."""
        if control == "servo_type" and self._effective_servo_type is not None:
            return self._effective_servo_type
        prefixed_ctrl = f"{self.prefix}{control}" if self.prefix else control
        try:
            res = self._run_dut_control(["-o", prefixed_ctrl], check=True)
            return res.stdout.strip()
        except ServoError:
            if default is not None:
                return default
            raise

    def set(self, control: str, value: str) -> None:
        """Set the value of a single servo control."""
        prefixed_ctrl = f"{self.prefix}{control}" if self.prefix else control
        self._run_dut_control([f"{prefixed_ctrl}:{value}"], check=True)

    def set_many(self, controls: List[Tuple[str, str]]) -> None:
        """Set multiple servo controls in a single dut-control invocation."""
        args = []
        for ctrl, val in controls:
            prefixed_ctrl = f"{self.prefix}{ctrl}" if self.prefix else ctrl
            args.append(f"{prefixed_ctrl}:{val}")
        self._run_dut_control(args, check=True)

    def save_state(
        self, control: str, override_restore_val: Optional[str] = None
    ) -> None:
        """Save the current value of a control to be restored on exit."""
        if override_restore_val is not None:
            self._saved_state.insert(0, (control, override_restore_val))
        else:
            try:
                current_val = self.get(control)
                if current_val is not None:
                    self._saved_state.insert(0, (control, current_val))
            except ServoError as err:
                logger.warning(
                    "Could not save state for control %s: %s", control, err
                )

    def _get_process_cmd(self, pid: int) -> str:
        """Inspect command line of a process by PID."""
        try:
            res = subprocess.run(
                ["ps", "-o", "cmd=", str(pid)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                encoding="utf-8",
                check=False,
            )
            if res.returncode == 0 and res.stdout.strip():
                return res.stdout.strip()
        except (subprocess.SubprocessError, FileNotFoundError):
            pass

        try:
            with open(f"/proc/{pid}/cmdline", "rb") as f:
                return (
                    f.read()
                    .replace(b"\x00", b" ")
                    .decode("utf-8", errors="ignore")
                    .strip()
                )
        except OSError:
            return ""

    def claim_pty(self, pty_path: str, mcu: str = "ec") -> List[int]:
        """Claim a PTY by disconnecting EC-3PO and freezing competing processes.

        Processes matching servod, grpc_server_setup (fission gRPC server in
        hdctools-fission-2025, b/577188307), or /sbin/init are skipped to prevent
        the 20s keepalive timeout between the servod frontend and backend. Other processes
        holding the PTY receive SIGSTOP and are saved to be resumed upon exit.

        Args:
            pty_path: Path to the serial/pty device (e.g. /dev/pts/12).
            mcu: Microcontroller prefix (defaults to 'ec').

        Returns:
            List of PIDs that were sent SIGSTOP.
        """
        try:
            with open(
                "/proc/1/cmdline", "r", encoding="utf-8", errors="ignore"
            ) as f:
                if "cros_sdk" in f.read():
                    logger.warning(
                        "You might want to run this tool in a chroot that was "
                        "entered with 'cros_sdk --no-ns-pid' "
                        "(see crbug.com/444931 for details)"
                    )
        except OSError:
            pass

        if not pty_path:
            logger.warning("No parameter passed to claim_pty()")
            return []

        ctrl = f"{mcu}_ec3po_interp_connect"
        try:
            self.save_state(ctrl)
            self.set(ctrl, "off")
        except ServoError as err:
            logger.warning(
                "hdctools cannot disconnect the EC-3PO interpreter from "
                "the UART: %s",
                err,
            )

        try:
            res = subprocess.run(
                ["lsof", "-FR", "--", pty_path],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                encoding="utf-8",
                check=False,
            )
            raw_output = res.stdout
        except (subprocess.SubprocessError, FileNotFoundError) as err:
            logger.debug("lsof failed: %s", err)
            return []

        pids: List[int] = []
        for line in raw_output.splitlines():
            line = line.strip()
            if not line or line.startswith("f"):
                continue
            if line.startswith(("p", "R")):
                val = line[1:].strip()
                if val.isdigit():
                    pid = int(val)
                    if pid not in pids:
                        pids.append(pid)

        # Reverse order to SIGSTOP parents before children, matching tac in bash
        pids.reverse()

        claimed_pids: List[int] = []
        for pid in pids:
            cmd = self._get_process_cmd(pid)
            if pid <= 1 or SKIP_PROCESS_REGEX.search(cmd):
                logger.info("Skip stopping servod or init: process %d.", pid)
            else:
                logger.info("Sending SIGSTOP to process %d!", pid)
                if pid not in self._frozen_pids:
                    self._frozen_pids.append(pid)
                claimed_pids.append(pid)
                time.sleep(0.02)
                if not self.dry_run:
                    try:
                        os.kill(pid, signal.SIGSTOP)
                    except (ProcessLookupError, PermissionError) as err:
                        logger.warning(
                            "Failed to send SIGSTOP to process %d: %s",
                            pid,
                            err,
                        )

        return claimed_pids

    def release_pty(self) -> None:
        """Send SIGCONT to processes stopped during claim_pty()."""
        if not self._frozen_pids:
            return
        for pid in self._frozen_pids:
            logger.info("Sending SIGCONT to process %d!", pid)
            if not self.dry_run:
                try:
                    os.kill(pid, signal.SIGCONT)
                except ProcessLookupError:
                    pass
                except OSError as err:
                    logger.warning(
                        "Failed to send SIGCONT to process %d: %s", pid, err
                    )
        self._frozen_pids.clear()

    def restore_state(self) -> None:
        """Restore all saved controls in LIFO order."""
        self.release_pty()
        if not self._saved_state:
            return
        logger.info("Restoring servo settings...")
        for control, val in self._saved_state:
            try:
                self.set(control, val)
            except ServoError as err:
                logger.error(
                    "Failed to restore %s to %s: %s", control, val, err
                )
        self._saved_state.clear()


def claim_pty(
    pty_path: str,
    servo: Optional[ServoDriver] = None,
    mcu: str = "ec",
) -> List[int]:
    """Claim a PTY by disconnecting EC-3PO and freezing competing processes.

    Convenience wrapper around ServoDriver.claim_pty().
    """
    if servo is not None:
        return servo.claim_pty(pty_path, mcu=mcu)
    driver = ServoDriver()
    return driver.claim_pty(pty_path, mcu=mcu)


def release_pty(servo: Optional[ServoDriver] = None) -> None:
    """Release frozen processes by sending SIGCONT.

    Convenience wrapper around ServoDriver.release_pty().
    """
    if servo is not None:
        servo.release_pty()
