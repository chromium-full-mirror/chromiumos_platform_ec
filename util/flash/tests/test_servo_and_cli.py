#!/usr/bin/env vpython3
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Unit tests for ServoDriver and flash_ec CLI entrypoint."""

import os
import signal
import subprocess
import unittest
from unittest.mock import MagicMock
from unittest.mock import patch

from flash.flash_ec import main
from flash.flash_ec import parse_args
from flash.servo_utils import claim_pty
from flash.servo_utils import release_pty
from flash.servo_utils import ServoDriver
from flash.servo_utils import ServoError


class TestServoDriver(unittest.TestCase):
    """Tests for ServoDriver wrapper around dut-control."""

    @patch("flash.servo_utils.subprocess.run")
    def test_dual_servo_prefix_and_effective_type(
        self, mock_run: MagicMock
    ) -> None:
        """Normalize servo_type and set prefix when dual-v4 second controller active."""
        mock_run.side_effect = [
            subprocess.CompletedProcess(
                ["dut-control"],
                0,
                stdout="servo_v4_with_servo_micro_and_ccd_cr50\n",
                stderr="",
            ),
            subprocess.CompletedProcess(
                ["dut-control"], 0, stdout="ccd_cr50\n", stderr=""
            ),
        ]
        servo = ServoDriver(host="localhost", port=9999)
        self.assertEqual(servo.prefix, "ccd_cr50.")
        self.assertEqual(servo.get("servo_type"), "servo_v4_with_ccd_cr50")

    @patch("flash.servo_utils.subprocess.run")
    def test_save_and_restore_state(self, mock_run: MagicMock) -> None:
        """Save controls and restore them in LIFO order."""
        mock_run.side_effect = [
            subprocess.CompletedProcess(
                ["dut-control"], 0, stdout="ccd_cr50\n", stderr=""
            ),
            subprocess.CompletedProcess(
                ["dut-control"], 0, stdout="off\n", stderr=""
            ),
            subprocess.CompletedProcess(
                ["dut-control"], 0, stdout="", stderr=""
            ),
            subprocess.CompletedProcess(
                ["dut-control"], 0, stdout="", stderr=""
            ),
        ]
        servo = ServoDriver(host="127.0.0.1", port=9998)
        servo.save_state("cold_reset")
        servo.save_state("ec_flash_select", override_restore_val="off")
        servo.restore_state()
        mock_run.reset_mock()
        servo.restore_state()
        mock_run.assert_not_called()

    @patch("flash.servo_utils.subprocess.run")
    def test_dry_run_skips_mutations(self, mock_run: MagicMock) -> None:
        """Do not execute mutating dut-control calls when dry_run is True."""
        mock_run.return_value = subprocess.CompletedProcess(
            ["dut-control"], 0, stdout="ccd_cr50\n", stderr=""
        )
        servo = ServoDriver(dry_run=True)
        mock_run.reset_mock()
        servo.set("cold_reset", "on")
        mock_run.assert_not_called()

    @patch("flash.servo_utils.os.kill")
    @patch("flash.servo_utils.subprocess.run")
    def test_claim_pty_skips_fission_grpc_server_setup(
        self, mock_run: MagicMock, mock_kill: MagicMock
    ) -> None:
        """Skip stopping fission grpc_server_setup in claim_pty (b/577188307).

        hdctools-fission-2025 runs the servod data plane in a separate process
        (grpc_server_setup.py) whose cmdline does not contain 'servod'. Freezing
        it triggers bols_servod 20s keepalive watchdog timeouts.
        """

        def fake_run(cmd, **_kwargs):
            if cmd[0] == "dut-control":
                if "-o" in cmd and "servo_type" in cmd:
                    return subprocess.CompletedProcess(
                        cmd, 0, stdout="", stderr=""
                    )
                if "-o" in cmd and "ec_ec3po_interp_connect" in cmd:
                    return subprocess.CompletedProcess(
                        cmd, 0, stdout="on\n", stderr=""
                    )
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            if cmd[0] == "lsof":
                # Output 4 processes: fission grpc, servod, init, and third-party terminal
                output = "p101\nR1\np102\nR1\np103\nR0\np104\nR1\n"
                return subprocess.CompletedProcess(
                    cmd, 0, stdout=output, stderr=""
                )
            if cmd[0] == "ps":
                pid = cmd[3]
                if pid == "101":
                    cmdline = (
                        "/usr/bin/python3 "
                        "/usr/lib/python3/dist-packages/servo/data/grpc_server/"
                        "grpc_server_setup.py --port=9999"
                    )
                elif pid == "102":
                    cmdline = "/usr/local/bin/servod -b veluza -p 9999"
                elif pid == "103":
                    cmdline = "/sbin/init splash"
                elif pid == "104":
                    cmdline = "/usr/bin/minicom -D /dev/pts/12"
                else:
                    cmdline = ""
                return subprocess.CompletedProcess(
                    cmd, 0, stdout=cmdline, stderr=""
                )
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        mock_run.side_effect = fake_run

        servo = ServoDriver(host="localhost", port=9999)
        claimed = servo.claim_pty("/dev/pts/12")

        # Only minicom (104) should be frozen; 101 (grpc_server_setup),
        # 102 (servod), and 103 (/sbin/init) MUST be skipped.
        self.assertEqual(claimed, [104])
        self.assertEqual(servo.frozen_pids, [104])
        mock_kill.assert_called_once_with(104, signal.SIGSTOP)

        # Restore state must release PID 104 with SIGCONT and clear frozen_pids
        mock_kill.reset_mock()
        servo.restore_state()
        mock_kill.assert_called_once_with(104, signal.SIGCONT)
        self.assertEqual(servo.frozen_pids, [])

    @patch("flash.servo_utils.os.kill")
    @patch("flash.servo_utils.subprocess.run")
    def test_claim_pty_empty_path_returns_empty(
        self, mock_run: MagicMock, mock_kill: MagicMock
    ) -> None:
        """Return empty list when pty_path is empty without running lsof."""
        mock_run.return_value = subprocess.CompletedProcess(
            ["dut-control"], 0, stdout="", stderr=""
        )
        servo = ServoDriver(host="localhost", port=9999)
        self.assertEqual(servo.claim_pty(""), [])
        mock_kill.assert_not_called()

    @patch("flash.servo_utils.os.kill")
    @patch("flash.servo_utils.subprocess.run")
    def test_claim_pty_dry_run_skips_signals(
        self, mock_run: MagicMock, mock_kill: MagicMock
    ) -> None:
        """In dry_run mode, do not send actual SIGSTOP or SIGCONT signals."""

        def fake_run(cmd, **_kwargs):
            if cmd[0] == "dut-control":
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            if cmd[0] == "lsof":
                return subprocess.CompletedProcess(
                    cmd, 0, stdout="p205\n", stderr=""
                )
            if cmd[0] == "ps":
                return subprocess.CompletedProcess(
                    cmd, 0, stdout="cat /dev/pts/12", stderr=""
                )
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        mock_run.side_effect = fake_run

        servo = ServoDriver(host="localhost", port=9999, dry_run=True)
        claimed = servo.claim_pty("/dev/pts/12")
        self.assertEqual(claimed, [205])
        mock_kill.assert_not_called()

        servo.release_pty()
        mock_kill.assert_not_called()

    @patch("flash.servo_utils.os.kill")
    @patch("flash.servo_utils.subprocess.run")
    def test_module_helpers_claim_and_release_pty(
        self, mock_run: MagicMock, mock_kill: MagicMock
    ) -> None:
        """Verify module-level claim_pty and release_pty helper functions."""

        def fake_run(cmd, **_kwargs):
            if cmd[0] == "dut-control":
                return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")
            if cmd[0] == "lsof":
                return subprocess.CompletedProcess(
                    cmd, 0, stdout="p301\n", stderr=""
                )
            if cmd[0] == "ps":
                return subprocess.CompletedProcess(
                    cmd, 0, stdout="screen /dev/pts/5", stderr=""
                )
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        mock_run.side_effect = fake_run
        servo = ServoDriver(host="localhost", port=9999)

        claimed = claim_pty("/dev/pts/5", servo=servo)
        self.assertEqual(claimed, [301])
        mock_kill.assert_called_once_with(301, signal.SIGSTOP)

        mock_kill.reset_mock()
        release_pty(servo=servo)
        mock_kill.assert_called_once_with(301, signal.SIGCONT)


class TestFlashEcCli(unittest.TestCase):
    """Tests for flash_ec CLI argument parsing and main entrypoint."""

    def test_parse_args_defaults(self) -> None:
        """Verify default timeout (600s), port, region, and OpenOCD flags."""
        with patch.dict(os.environ):
            os.environ.pop("SERVOD_PORT", None)
            args = parse_args(["--board=veluza", "--ocd_chip_cfg=custom.cfg"])
        self.assertEqual(args.board, "veluza")
        self.assertEqual(args.timeout, 600)
        self.assertEqual(args.port, 9999)
        self.assertEqual(args.ocd_chip_cfg, "custom.cfg")
        self.assertTrue(args.verify)
        self.assertFalse(args.ro)
        self.assertFalse(args.rw)

    @patch.dict(os.environ, {"SERVOD_PORT": "9901"})
    def test_parse_args_port_from_servod_port(self) -> None:
        """Default --port to $SERVOD_PORT like bash flash_ec; --port wins."""
        self.assertEqual(parse_args(["--board=veluza"]).port, 9901)
        args = parse_args(["--board=veluza", "--port=9905"])
        self.assertEqual(args.port, 9905)
        os.environ["SERVOD_PORT"] = ""
        self.assertEqual(parse_args(["--board=veluza"]).port, 9999)

    def test_parse_args_region_flags(self) -> None:
        """Verify --ro and --rw select the region to flash."""
        args = parse_args(["--board=veluza", "--ro"])
        self.assertTrue(args.ro)
        self.assertFalse(args.rw)
        args = parse_args(["--board=veluza", "--rw"])
        self.assertFalse(args.ro)
        self.assertTrue(args.rw)
        with patch("sys.stderr"), self.assertRaises(SystemExit):
            parse_args(["--board=veluza", "--ro", "--rw"])

    def test_parse_args_negated_boolean_aliases(self) -> None:
        """Verify shflags-compatible negated boolean aliases on CLI flags."""
        args = parse_args(
            [
                "--board=veluza",
                "--no-verify",
                "--unprotect",
                "--no-unprotect",
                "--dry-run",
                "--nodryrun",
                "--verbose",
                "--noverbose",
                "--raiden",
                "--noraiden",
                "--bitbang",
                "--nobitbang",
                "--ro",
                "--noro",
                "--norw",
                "--try-apshutdown",
                "--notry_apshutdown",
                "--nopreserve",
            ]
        )
        self.assertFalse(args.verify)
        self.assertFalse(args.unprotect)
        self.assertFalse(args.dry_run)
        self.assertFalse(args.verbose)
        self.assertFalse(args.raiden)
        self.assertFalse(args.bitbang)
        self.assertFalse(args.ro)
        self.assertFalse(args.rw)
        self.assertFalse(args.try_apshutdown)
        self.assertTrue(args.no_preserve)

        args_preserve = parse_args(
            ["--board=veluza", "--no-preserve", "--preserve"]
        )
        self.assertFalse(args_preserve.no_preserve)

        for bad_flag in ("--verify_ec_type", "--noverify_ec_type", "--no_pres"):
            with patch("sys.stderr"), self.assertRaises(SystemExit) as ctx:
                parse_args(["--board=veluza", bad_flag])
            self.assertEqual(ctx.exception.code, 2)

    @patch("flash.flash_ec.FlasherFactory.create")
    @patch("flash.flash_ec.ServoDriver")
    def test_main_success(
        self, _mock_servo: MagicMock, mock_create: MagicMock
    ) -> None:
        """Verify main() returns 0 when flasher.run() succeeds."""
        ret = main(["--board=veluza", "--image=/tmp/ec.bin"])
        self.assertEqual(ret, 0)
        mock_create.return_value.run.assert_called_once()

    @patch(
        "flash.flash_ec.ServoDriver",
        side_effect=ServoError("servod unreachable"),
    )
    def test_main_error_returns_1(self, _mock_servo: MagicMock) -> None:
        """Verify main() returns 1 when ServoError is raised."""
        ret = main(["--board=veluza", "--image=/tmp/ec.bin"])
        self.assertEqual(ret, 1)

    @patch("flash.flash_ec.FlasherFactory.create")
    @patch("flash.flash_ec.ServoDriver")
    def test_main_requires_image(
        self, _mock_servo: MagicMock, mock_create: MagicMock
    ) -> None:
        """Verify main() fails without --image unless --read is given."""
        self.assertEqual(main(["--board=veluza"]), 1)
        mock_create.assert_not_called()
        self.assertEqual(main(["--board=veluza", "--read=/tmp/ec.bin"]), 0)


if __name__ == "__main__":
    unittest.main()
