#!/usr/bin/env python3
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Unit tests for test/run_device_tests.py."""

from __future__ import annotations

import collections
from collections.abc import Sequence
import dataclasses
import datetime
from pathlib import Path
import signal
import types
import unittest
from unittest import mock

import run_device_tests


@dataclasses.dataclass
class _FakeClock:
    """Deterministic monotonic clock for unit tests."""

    now: float = 0.0
    sleeps: list[float] = dataclasses.field(default_factory=list)

    def monotonic(self) -> float:
        """Return the current fake monotonic timestamp."""
        return self.now

    def sleep(self, secs: float) -> None:
        """Record a sleep duration and advance the fake clock."""
        self.sleeps.append(secs)
        self.now += secs


@dataclasses.dataclass
class _FakeConsole:
    """In-memory console stream backed by _FakeClock."""

    clock: _FakeClock
    writes: list[bytes] = dataclasses.field(default_factory=list)
    _queue: collections.deque[tuple[float, bytes | None]] = dataclasses.field(
        default_factory=collections.deque, init=False
    )
    _buf: bytearray = dataclasses.field(default_factory=bytearray, init=False)

    def feed(
        self,
        *args: bytes | None,
        delay: datetime.timedelta = datetime.timedelta(),
    ) -> None:
        """Queue console lines, each arriving after delay."""
        delay_secs = delay.total_seconds()
        self._queue.extend((delay_secs, line) for line in args)

    def select(
        self,
        rlist: Sequence[object],
        _wlist: Sequence[object],
        _xlist: Sequence[object],
        timeout: float,
    ) -> tuple[list[object], list[object], list[object]]:
        """Simulate select.select() backed by the fake clock."""
        if self._buf:
            return list(rlist), [], []

        if self._queue:
            delay, item = self._queue.popleft()
            if delay >= timeout:
                self._queue.appendleft((delay - timeout, item))
            elif item is not None:
                self.clock.now += delay
                self._buf.extend(item)
                return list(rlist), [], []

        self.clock.now += timeout
        return [], [], []

    def read(self, size: int = 1) -> bytes:
        """Read up to size bytes from the buffered console output."""
        chunk = bytes(self._buf[:size])
        del self._buf[:size]
        return chunk

    def write(self, data: bytes) -> int:
        """Record console input bytes and return the byte count."""
        self.writes.append(data)
        return len(data)

    def flush(self) -> None:
        """No-op console flush."""


@dataclasses.dataclass
class _FakeProcess:
    """In-memory subprocess.Popen fake for unit tests."""

    pid: int = 12345
    stdin: None = None
    waited: bool = False

    def poll(self) -> int | None:
        """Return None to indicate the process is still running."""
        return None

    def wait(self, timeout: float | None = None) -> int:
        """Mark the process as reaped and return a zero exit status."""
        del timeout
        self.waited = True
        return 0


# pylint: disable=missing-function-docstring,protected-access
class RunDeviceTestsTest(unittest.TestCase):
    """Tests for run_device_tests."""

    def setUp(self) -> None:
        super().setUp()
        self.clock = _FakeClock()
        self.console = _FakeConsole(self.clock)
        self.enterContext(
            mock.patch(
                "run_device_tests.time.monotonic",
                side_effect=self.clock.monotonic,
            )
        )
        self.enterContext(
            mock.patch(
                "run_device_tests.time.sleep",
                side_effect=self.clock.sleep,
            )
        )
        self.enterContext(
            mock.patch(
                "run_device_tests.select.select",
                side_effect=self.console.select,
            )
        )
        self.mock_killpg = self.enterContext(
            mock.patch("run_device_tests.os.killpg", autospec=True)
        )

    def test_single_check_failed_regex_matches_failures(self) -> None:
        regex = run_device_tests.SINGLE_CHECK_FAILED_REGEX
        self.assertIsNotNone(
            regex.match("test/foo.c:42: ASSERTION failed: x == y")
        )
        self.assertIsNotNone(
            regex.match('assertion "x == y" failed: file "test/foo.c", line 42')
        )
        self.assertIsNotNone(
            regex.match("    FAIL - test_foo in 0.001 seconds")
        )

    def test_single_check_failed_regex_ignores_driver_errors(self) -> None:
        regex = run_device_tests.SINGLE_CHECK_FAILED_REGEX
        self.assertIsNone(
            regex.match(
                "[0.012345 egis630_pal: SPI PAL transaction failed: -116]"
            )
        )
        self.assertIsNone(regex.match("Pass: all checks succeeded"))

    def test_power_cycle_sleeps_for_reboot_timeout(self) -> None:
        platform = mock.create_autospec(
            run_device_tests.Platform, instance=True
        )
        board = run_device_tests.HELIPILOT_CONFIG

        run_device_tests.power_cycle(platform, board)

        self.assertEqual(
            platform.power.call_args_list,
            [
                mock.call(board, power_on=False),
                mock.call(board, power_on=True),
            ],
        )
        self.assertEqual(self.clock.sleeps, [3.0, 3.0])

    def test_run_test_ro_sleeps_for_reboot_timeout(self) -> None:
        self.console.feed(b"Pass!\r\n")
        test = run_device_tests.TestConfig(
            test_name="abort",
            imagetype_to_use=run_device_tests.ImageType.RO,
        )

        passed = run_device_tests.run_test(
            test,
            run_device_tests.HELIPILOT_CONFIG,
            self.console,
            zephyr=False,
        )

        self.assertTrue(passed)
        self.assertEqual(self.clock.sleeps, [3.0, 3.0])

    def _run_test(
        self,
        test: run_device_tests.TestConfig,
        *,
        zephyr: bool = False,
        output: bytes = b"Pass!\r\n",
    ) -> bool:
        self.console.feed(output)
        return run_device_tests.run_test(
            test,
            run_device_tests.HELIPILOT_CONFIG,
            self.console,
            zephyr=zephyr,
        )

    def test_run_test_ro_ec_reboots_to_ro_and_runs_test(self) -> None:
        test = run_device_tests.TestConfig(
            test_name="abort",
            imagetype_to_use=run_device_tests.ImageType.RO,
        )

        self.assertTrue(self._run_test(test))
        self.assertEqual(
            self.console.writes,
            [b"\nreboot ro\n", b"\nruntest \n"],
        )

    def test_run_test_rw_ec_with_args_joins_test_args(self) -> None:
        test = run_device_tests.TestConfig(
            test_name="fp_transport",
            test_args=["spi", "dma"],
        )

        self.assertTrue(self._run_test(test))
        self.assertEqual(self.console.writes, [b"\nruntest spi dma\n"])

    def test_run_test_rw_zephyr_without_args_runs_all_suites(self) -> None:
        test = run_device_tests.TestConfig(test_name="flash_protection_rw")

        self.assertTrue(self._run_test(test, zephyr=True))
        self.assertEqual(self.console.writes, [b"\nztest run-all\n"])

    def test_run_test_rw_zephyr_with_args_runs_matching_testcase(self) -> None:
        test = run_device_tests.TestConfig(
            test_name="fp_transport",
            test_args=["spi", "dma"],
        )

        self.assertTrue(
            self._run_test(
                test,
                zephyr=True,
                output=b"TESTSUITE fp_transport_spi_dma succeeded\r\n",
            )
        )
        self.assertEqual(
            self.console.writes,
            [b"\nztest run-testcase fp_transport_spi_dma\n"],
        )

    def test_run_test_ec_production_app_writes_no_command(self) -> None:
        test = run_device_tests.TestConfig(
            test_name="production_app_test",
            finish_regexes=[run_device_tests.RW_IMAGE_BOOTED_REGEX],
            apptype_to_use=run_device_tests.ApplicationType.PRODUCTION,
        )

        self.assertTrue(
            self._run_test(test, output=b"[Image: RW, helipilot]\r\n")
        )
        self.assertEqual(self.console.writes, [])

    def test_run_test_zephyr_production_app_writes_no_command(self) -> None:
        test = run_device_tests.TestConfig(
            test_name="production_app_test",
            finish_regexes=[run_device_tests.RW_IMAGE_BOOTED_REGEX],
            apptype_to_use=run_device_tests.ApplicationType.PRODUCTION,
        )

        self.assertTrue(
            self._run_test(
                test, zephyr=True, output=b"[Image: RW, helipilot]\r\n"
            )
        )
        self.assertEqual(self.console.writes, [])

    def test_run_test_zephyr_upstream_writes_no_command(self) -> None:
        test = run_device_tests.TestConfig(
            zephyr_name="cpp.main.newlib",
            test_name="zephyr_cpp_newlib",
            finish_regexes=[run_device_tests.ALL_TESTS_PASSED_REGEX_ZEPHYR],
        )

        self.assertTrue(
            self._run_test(
                test, zephyr=True, output=b"PROJECT EXECUTION SUCCESSFUL\r\n"
            )
        )
        self.assertEqual(self.console.writes, [])

    def _make_console_path(self, ready_at: datetime.timedelta) -> Path:
        console_path = Path("/tmp/renode-uart")
        ready_at_secs = ready_at.total_seconds()

        def is_char_device(path: Path) -> bool:
            return path == console_path and self.clock.now >= ready_at_secs

        self.enterContext(
            mock.patch.object(
                Path,
                "is_char_device",
                autospec=True,
                side_effect=is_char_device,
            )
        )
        return console_path

    def test_renode_wait_for_console_succeeds_before_timeout(self) -> None:
        process = _FakeProcess()
        platform = run_device_tests.Renode()
        platform.process = process

        ready = platform._wait_for_console(
            self._make_console_path(datetime.timedelta(milliseconds=100)),
            Path("/tmp/renode.log"),
            timeout=datetime.timedelta(milliseconds=200),
        )

        self.assertTrue(ready)
        self.assertIs(platform.process, process)
        self.assertEqual(self.clock.sleeps, [0.1, 1])
        self.mock_killpg.assert_not_called()

    def test_renode_wait_for_console_times_out_at_deadline(self) -> None:
        process = _FakeProcess()
        platform = run_device_tests.Renode()
        platform.process = process

        with self.assertLogs(level="ERROR"):
            ready = platform._wait_for_console(
                self._make_console_path(datetime.timedelta(milliseconds=100)),
                Path("/tmp/renode.log"),
                timeout=datetime.timedelta(milliseconds=100),
            )

        self.assertFalse(ready)
        self.assertEqual(self.clock.sleeps, [0.1])
        self.assertIsNone(platform.process)
        self.assertTrue(process.waited)
        self.mock_killpg.assert_called_once_with(process.pid, signal.SIGKILL)

    def test_readlines_until_timeout_reads_until_deadline(self) -> None:
        self.console.feed(b"line 1\r\n", delay=datetime.timedelta(seconds=0.5))
        self.console.feed(b"line 2\r\n", delay=datetime.timedelta(seconds=1.0))
        self.console.feed(b"line 3\r\n", delay=datetime.timedelta(seconds=0.5))

        lines = run_device_tests.readlines_until_timeout(
            self.console, datetime.timedelta(seconds=2)
        )

        self.assertEqual(lines, [b"line 1\r\n", b"line 2\r\n"])
        self.assertEqual(self.clock.now, 2.0)
        self.assertEqual(
            run_device_tests.readline(self.console, 1.0), b"line 3\r\n"
        )
        self.assertEqual(self.clock.now, 2.0)

    def test_get_private_tests_converts_legacy_timeout_secs(self) -> None:
        fake_private_tests = types.SimpleNamespace(
            tests=[
                {"test_name": "rick_benchmark", "timeout_secs": 30},
                {"test_name": "rng"},
            ]
        )
        self.enterContext(
            mock.patch("run_device_tests.os.path.isdir", return_value=True)
        )
        self.enterContext(mock.patch("run_device_tests.sys.path", []))
        self.enterContext(
            mock.patch.dict(
                "sys.modules", {"private_tests": fake_private_tests}
            )
        )

        tests = run_device_tests.AllTests.get_private_tests()

        self.assertEqual(
            [(t.test_name, t.timeout) for t in tests],
            [
                ("rick_benchmark", datetime.timedelta(seconds=30)),
                ("rng", datetime.timedelta(seconds=60)),
            ],
        )

    def test_run_test_preserves_subsecond_remaining_timeout(self) -> None:
        self.clock.now = 100.0
        board = run_device_tests.HELIPILOT_CONFIG
        remaining_timeout = datetime.timedelta(milliseconds=500)
        self.console.feed(
            b"Pass!\r\n", delay=datetime.timedelta(milliseconds=400)
        )
        test = run_device_tests.TestConfig(
            test_name="aes",
            timeout=board.reboot_timeout + remaining_timeout,
        )

        passed = run_device_tests.run_test(
            test, board, self.console, zephyr=False
        )

        self.assertTrue(passed)

    def test_run_test_times_out_at_subsecond_deadline(self) -> None:
        self.clock.now = 100.0
        board = run_device_tests.HELIPILOT_CONFIG
        remaining_timeout = datetime.timedelta(milliseconds=500)
        self.console.feed(b"Pass!\r\n", delay=remaining_timeout)
        test = run_device_tests.TestConfig(
            test_name="aes",
            timeout=board.reboot_timeout + remaining_timeout,
        )

        passed = run_device_tests.run_test(
            test, board, self.console, zephyr=False
        )

        self.assertFalse(passed)
        # 100.0s initial clock + 3.0s HELIPILOT_CONFIG.reboot_timeout + 0.5s
        # remaining_timeout.
        self.assertEqual(self.clock.now, 103.5)

    def test_run_test_times_out_at_deadline(self) -> None:
        test = run_device_tests.TestConfig(
            test_name="aes", timeout=datetime.timedelta(seconds=10)
        )

        passed = run_device_tests.run_test(
            test,
            run_device_tests.HELIPILOT_CONFIG,
            self.console,
            zephyr=False,
        )

        self.assertFalse(passed)
        self.assertEqual(self.clock.now, 10.0)


if __name__ == "__main__":
    unittest.main()
