#!/usr/bin/env python3
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Unit tests for test/run_device_tests.py."""

from __future__ import annotations

import argparse
import collections
from collections.abc import Sequence
import dataclasses
import datetime
import errno
import io
import os
from pathlib import Path
import select
import signal
import socket
import subprocess
import tty
import types
from typing import BinaryIO
import unittest
from unittest import mock

import run_device_tests


_REAL_SELECT = select.select


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

    def prebuffer(self, *args: bytes) -> None:
        """Buffer bytes already unread in the console before reading."""
        for chunk in args:
            self._buf.extend(chunk)

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
    stdout: io.BytesIO = dataclasses.field(default_factory=io.BytesIO)
    waited: bool = False

    def poll(self) -> int | None:
        """Return None to indicate the process is still running."""
        return None

    def wait(self, timeout: float | None = None) -> int:
        """Mark the process as reaped and return a zero exit status."""
        del timeout
        self.waited = True
        return 0

    def __enter__(self) -> _FakeProcess:
        return self

    def __exit__(self, *args: object) -> None:
        del args


@dataclasses.dataclass
class _FakeReadStream:
    """In-memory stream fake for fault-injecting _drain_console() reads."""

    responses: collections.deque[bytes | None | Exception] = dataclasses.field(
        default_factory=collections.deque
    )

    def select(
        self,
        rlist: Sequence[object],
        _wlist: Sequence[object],
        _xlist: Sequence[object],
        _timeout: float,
    ) -> tuple[list[object], list[object], list[object]]:
        """Report readable while scripted responses remain."""
        if self.responses:
            return list(rlist), [], []
        return [], [], []

    def read(self, size: int = -1) -> bytes | None:
        """Return or raise the next scripted read outcome."""
        del size
        outcome = self.responses.popleft()
        if isinstance(outcome, Exception):
            raise outcome
        return outcome


# pylint: disable=unused-argument
@dataclasses.dataclass
class _FakePlatform(run_device_tests.Platform):
    """In-memory Platform fake for flash_and_run_test() unit tests."""

    console_pty: str | None = None
    get_console_calls: list[run_device_tests.BoardConfig] = dataclasses.field(
        default_factory=list
    )
    attached_consoles: list[BinaryIO | None] = dataclasses.field(
        default_factory=list
    )
    events: list[str] = dataclasses.field(default_factory=list)

    def get_console(
        self, board_config: run_device_tests.BoardConfig
    ) -> str | None:
        """Record the board_config query and return the configured PTY path."""
        self.get_console_calls.append(board_config)
        return self.console_pty

    def attach_console(self, console: BinaryIO | None) -> None:
        """Record the attached console stream."""
        self.attached_consoles.append(console)
        self.events.append(
            "detach_console" if console is None else "attach_console"
        )

    def discard_console_input(self) -> None:
        """Record a console-input discard request."""
        self.events.append("discard_console_input")

    def hw_write_protect(self, enable: bool) -> None:
        """Record a hardware write-protect toggle."""
        self.events.append(f"hw_write_protect({enable})")

    def power(
        self, board_config: run_device_tests.BoardConfig, power_on: bool
    ) -> None:
        """Record a board power toggle."""
        self.events.append(f"power({power_on})")

    def flash(
        self,
        board_config: run_device_tests.BoardConfig,
        image_path: str,
        flasher: str,
        remote_ip: str,
        remote_port: int,
        test_config: run_device_tests.TestConfig,
        zephyr: bool,
    ) -> bool:
        """Record flashing and succeed immediately."""
        self.events.append("flash")
        return True

    def cleanup(self) -> None:
        """Record platform cleanup."""
        self.events.append("cleanup")

    def skip_test(
        self,
        test_config: run_device_tests.TestConfig,
        board_config: run_device_tests.BoardConfig,
        zephyr: bool,
    ) -> bool:
        """Never skip tests."""
        return False


@dataclasses.dataclass
class _FakeSocket:
    """In-memory TCP socket fake for remote console unit tests."""

    console: io.BytesIO = dataclasses.field(default_factory=io.BytesIO)
    connected_addr: tuple[str, int] | None = None

    def connect(self, address: tuple[str, int]) -> None:
        """Record the remote endpoint address."""
        self.connected_addr = address

    def setblocking(self, flag: bool) -> None:
        """No-op socket blocking mode toggle."""

    def makefile(self, mode: str = "r", buffering: int = -1) -> io.BytesIO:
        """Return the in-memory console stream."""
        return self.console

    def __enter__(self) -> _FakeSocket:
        return self

    def __exit__(self, *args: object) -> None:
        pass


# pylint: enable=unused-argument


# pylint: disable=missing-function-docstring,protected-access
# pylint: disable=too-many-public-methods
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

    def test_run_test_production_app_skips_pre_test_setup(self) -> None:
        self.console.feed(
            b"[Image: RO, sanok]\r\n",
            b"[Image: RW, sanok]\r\n",
        )
        test = run_device_tests.TestConfig(
            test_name="production_app_test",
            imagetype_to_use=run_device_tests.ImageType.RO,
            finish_regexes=[run_device_tests.RW_IMAGE_BOOTED_REGEX],
            apptype_to_use=run_device_tests.ApplicationType.PRODUCTION,
        )

        passed = run_device_tests.run_test(
            test,
            run_device_tests.SANOK_CONFIG,
            self.console,
            zephyr=True,
        )

        self.assertTrue(passed)
        self.assertEqual(self.clock.sleeps, [])
        self.assertEqual(self.console.writes, [])

    def test_run_test_zephyr_upstream_skips_pre_test_setup(self) -> None:
        self.console.feed(
            b"*** Booting Zephyr OS build 1234 ***\r\n",
            b"PROJECT EXECUTION SUCCESSFUL\r\n",
        )
        test = run_device_tests.TestConfig(
            zephyr_name="cpp.main.newlib",
            test_name="zephyr_cpp_newlib",
            imagetype_to_use=run_device_tests.ImageType.RO,
            finish_regexes=[run_device_tests.ALL_TESTS_PASSED_REGEX_ZEPHYR],
        )

        passed = run_device_tests.run_test(
            test,
            run_device_tests.HELIPILOT_CONFIG,
            self.console,
            zephyr=True,
        )

        self.assertTrue(passed)
        self.assertEqual(self.clock.sleeps, [])
        self.assertEqual(self.console.writes, [])

    def _stub_build_and_run_test(self) -> None:
        self.enterContext(
            mock.patch.object(run_device_tests, "build", autospec=True)
        )
        self.enterContext(
            mock.patch.object(
                run_device_tests,
                "get_image_path",
                autospec=True,
                return_value="/tmp/abort.bin",
            )
        )
        self.enterContext(
            mock.patch.object(
                os.path, "exists", autospec=True, return_value=True
            )
        )
        self.enterContext(
            mock.patch.object(
                run_device_tests, "run_test", autospec=True, return_value=True
            )
        )

    def _make_args(
        self,
        *,
        remote: str | None = None,
        jlink_port: int | None = None,
        console_port: int | None = None,
    ) -> argparse.Namespace:
        return argparse.Namespace(
            build_only=False,
            compiler=run_device_tests.GCC,
            console_port=console_port,
            flasher=run_device_tests.JTRACE,
            jlink_port=jlink_port,
            remote=remote,
            renode=False,
            zephyr=False,
        )

    def _open_pty_path(self) -> str:
        master_fd, slave_fd = os.openpty()
        self.addCleanup(os.close, master_fd)
        self.addCleanup(os.close, slave_fd)
        return os.ttyname(slave_fd)

    def test_flash_and_run_test_remote_console_skips_get_console(self) -> None:
        self._stub_build_and_run_test()
        fake_socket = _FakeSocket()
        self.enterContext(
            mock.patch.object(
                socket, "socket", autospec=True, return_value=fake_socket
            )
        )
        platform = _FakePlatform()
        args = self._make_args(remote="127.0.0.1", console_port=10000)

        passed = run_device_tests.flash_and_run_test(
            run_device_tests.TestConfig(test_name="abort"),
            platform,
            run_device_tests.HELIPILOT_CONFIG,
            args,
        )

        self.assertTrue(passed)
        self.assertEqual(platform.get_console_calls, [])
        self.assertEqual(fake_socket.connected_addr, ("127.0.0.1", 10000))

    def test_flash_and_run_test_remote_jlink_only_uses_local_console(
        self,
    ) -> None:
        self._stub_build_and_run_test()
        board = run_device_tests.HELIPILOT_CONFIG
        platform = _FakePlatform(console_pty=self._open_pty_path())
        args = self._make_args(remote="127.0.0.1", jlink_port=19020)

        passed = run_device_tests.flash_and_run_test(
            run_device_tests.TestConfig(test_name="abort"),
            platform,
            board,
            args,
        )

        self.assertTrue(passed)
        self.assertEqual(platform.get_console_calls, [board])

    def test_flash_and_run_test_without_remote_uses_local_console(
        self,
    ) -> None:
        self._stub_build_and_run_test()
        board = run_device_tests.HELIPILOT_CONFIG
        platform = _FakePlatform(console_pty=self._open_pty_path())
        args = self._make_args(console_port=10000)

        passed = run_device_tests.flash_and_run_test(
            run_device_tests.TestConfig(test_name="abort"),
            platform,
            board,
            args,
        )

        self.assertTrue(passed)
        self.assertEqual(platform.get_console_calls, [board])

    def _drain_stream(
        self, *responses: bytes | None | Exception
    ) -> _FakeReadStream:
        stream = _FakeReadStream(collections.deque(responses))
        self.enterContext(
            mock.patch(
                "run_device_tests.select.select", side_effect=stream.select
            )
        )
        run_device_tests._drain_console(stream)
        return stream

    def _open_raw_pty(self) -> tuple[int, int, str]:
        master_fd, slave_fd = os.openpty()
        self.addCleanup(os.close, master_fd)
        self.addCleanup(os.close, slave_fd)
        tty.setraw(slave_fd)
        os.set_blocking(slave_fd, False)
        self.enterContext(
            mock.patch(
                "run_device_tests.select.select", side_effect=_REAL_SELECT
            )
        )
        return master_fd, slave_fd, os.ttyname(slave_fd)

    def _stub_dut_control(self, *outputs: bytes) -> None:
        procs = [_FakeProcess(stdout=io.BytesIO(out)) for out in outputs]
        self.enterContext(
            mock.patch.object(
                subprocess, "Popen", autospec=True, side_effect=procs
            )
        )

    def _stub_subprocess_run(self) -> mock.MagicMock:
        return self.enterContext(
            mock.patch.object(
                subprocess,
                "run",
                autospec=True,
                return_value=subprocess.CompletedProcess([], 0),
            )
        )

    def _flash_hardware(
        self,
        platform: run_device_tests.Hardware,
        board: run_device_tests.BoardConfig = (
            run_device_tests.HELIPILOT_CONFIG
        ),
        *,
        remote_ip: str = "",
        remote_port: int = 0,
    ) -> bool:
        return platform.flash(
            board_config=board,
            image_path="/tmp/abort.bin",
            flasher=run_device_tests.JTRACE,
            remote_ip=remote_ip,
            remote_port=remote_port,
            test_config=run_device_tests.TestConfig(test_name="abort"),
            zephyr=False,
        )

    def test_drain_console_drains_multiple_chunks_until_unreadable(
        self,
    ) -> None:
        chunk_size = run_device_tests.CONSOLE_DRAIN_CHUNK_SIZE_BYTES
        self.console.prebuffer(b"a" * chunk_size, b"tail")

        with self.assertLogs(level="DEBUG") as logs:
            run_device_tests._drain_console(self.console)

        self.assertEqual(self.console.read(1), b"")
        self.assertEqual(self.clock.now, 0.0)
        self.assertTrue(
            any(
                f"Discarded {chunk_size + 4} stale console bytes" in msg
                for msg in logs.output
            )
        )

    def test_drain_console_stops_on_eof(self) -> None:
        stream = self._drain_stream(b"", b"unreachable")

        self.assertEqual(list(stream.responses), [b"unreachable"])

    def test_drain_console_stops_on_nonblocking_empty_read(self) -> None:
        stream = self._drain_stream(None, b"unreachable")

        self.assertEqual(list(stream.responses), [b"unreachable"])

    def test_drain_console_stops_on_oserror(self) -> None:
        stream = self._drain_stream(
            OSError(errno.EIO, "hangup"), b"unreachable"
        )

        self.assertEqual(list(stream.responses), [b"unreachable"])

    def test_hardware_flash_without_get_console_succeeds(self) -> None:
        mock_run = self._stub_subprocess_run()

        with run_device_tests.Hardware() as platform:
            flashed = self._flash_hardware(
                platform, remote_ip="127.0.0.1", remote_port=19020
            )

        self.assertTrue(flashed)
        mock_run.assert_called_once()

    def test_hardware_flash_discards_stale_console_pty_bytes(self) -> None:
        master_fd, slave_fd, slave_path = self._open_raw_pty()
        os.write(master_fd, b"[Image: RW, stale]\r\n> ")

        board = run_device_tests.HELIPILOT_CONFIG
        self._stub_dut_control(
            f"{board.servo_uart_name}:{slave_path}\n".encode()
        )

        def fake_flash(
            cmd: Sequence[str], **_kwargs: object
        ) -> subprocess.CompletedProcess[str]:
            os.write(master_fd, b"[Image: RW, fresh]\r\n")
            return subprocess.CompletedProcess(cmd, 0)

        self.enterContext(
            mock.patch.object(
                subprocess, "run", autospec=True, side_effect=fake_flash
            )
        )

        with run_device_tests.Hardware() as platform:
            pty_path = platform.get_console(board)
            flashed = self._flash_hardware(platform, board)

        self.assertEqual(pty_path, slave_path)
        self.assertTrue(flashed)
        self.assertEqual(
            os.read(slave_fd, run_device_tests.CONSOLE_DRAIN_CHUNK_SIZE_BYTES),
            b"[Image: RW, fresh]\r\n",
        )

    def test_hardware_flash_ignores_unopenable_console_pty(self) -> None:
        board = run_device_tests.HELIPILOT_CONFIG
        self._stub_dut_control(
            f"{board.servo_uart_name}:/dev/nonexistent-servo-pty\n".encode()
        )
        self._stub_subprocess_run()

        with self.assertLogs(level="DEBUG") as logs:
            with run_device_tests.Hardware() as platform:
                platform.get_console(board)
                flashed = self._flash_hardware(platform, board)

        self.assertTrue(flashed)
        self.assertTrue(
            any("Failed to drain console PTY" in msg for msg in logs.output)
        )

    def test_hardware_get_console_failure_clears_cached_console_pty(
        self,
    ) -> None:
        master_fd, slave_fd, slave_path = self._open_raw_pty()
        board = run_device_tests.HELIPILOT_CONFIG
        self._stub_dut_control(
            f"{board.servo_uart_name}:{slave_path}\n".encode(), b""
        )
        self._stub_subprocess_run()

        with run_device_tests.Hardware() as platform:
            self.assertEqual(platform.get_console(board), slave_path)
            self.assertIsNone(platform.get_console(board))
            os.write(master_fd, b"unflushed after failed lookup\r\n")
            self._flash_hardware(platform, board)

        self.assertEqual(
            os.read(slave_fd, run_device_tests.CONSOLE_DRAIN_CHUNK_SIZE_BYTES),
            b"unflushed after failed lookup\r\n",
        )

    def test_hardware_cleanup_clears_cached_console_pty(self) -> None:
        master_fd, slave_fd, slave_path = self._open_raw_pty()
        board = run_device_tests.HELIPILOT_CONFIG
        self._stub_dut_control(
            f"{board.servo_uart_name}:{slave_path}\n".encode()
        )
        self._stub_subprocess_run()

        with run_device_tests.Hardware() as platform:
            platform.get_console(board)

        os.write(master_fd, b"after cleanup\r\n")
        self._flash_hardware(platform, board)

        self.assertEqual(
            os.read(slave_fd, run_device_tests.CONSOLE_DRAIN_CHUNK_SIZE_BYTES),
            b"after cleanup\r\n",
        )

    def test_power_cycle_hardware_discards_buffered_console_output(
        self,
    ) -> None:
        def fake_power(
            cmd: Sequence[str], **_kwargs: object
        ) -> subprocess.CompletedProcess[str]:
            if cmd[-1].endswith(":off"):
                self.console.prebuffer(b"[trailing before power off]\r\n")
            elif cmd[-1].endswith(":pp3300"):
                self.console.prebuffer(b"[Image: RW, fresh]\r\n")
            return subprocess.CompletedProcess(cmd, 0)

        def fake_sleep(secs: float) -> None:
            self.clock.sleep(secs)
            if len(self.clock.sleeps) == 1:
                self.console.prebuffer(b"[trailing during power-off sleep]\r\n")

        self.enterContext(
            mock.patch.object(
                subprocess, "run", autospec=True, side_effect=fake_power
            )
        )
        self.enterContext(
            mock.patch("run_device_tests.time.sleep", side_effect=fake_sleep)
        )
        with run_device_tests.Hardware() as platform:
            platform.attach_console(self.console)
            self.console.prebuffer(
                b"[Image: RW, helipilot]\r\n[0.001 Inits done]\r\n"
            )

            run_device_tests.power_cycle(
                platform, run_device_tests.HELIPILOT_CONFIG
            )

        self.assertEqual(
            self.console.read(run_device_tests.CONSOLE_DRAIN_CHUNK_SIZE_BYTES),
            b"[Image: RW, fresh]\r\n",
        )
        self.assertEqual(self.clock.sleeps, [3.0, 3.0])

    def test_power_cycle_renode_leaves_buffered_console_output_intact(
        self,
    ) -> None:
        with run_device_tests.Renode() as platform:
            platform.attach_console(self.console)
            self.console.prebuffer(b"[Image: RW, dartmonkey]\r\n")

            run_device_tests.power_cycle(
                platform, run_device_tests.DARTMONKEY_CONFIG
            )

        self.assertEqual(
            self.console.read(run_device_tests.CONSOLE_DRAIN_CHUNK_SIZE_BYTES),
            b"[Image: RW, dartmonkey]\r\n",
        )

    def test_hardware_cleanup_detaches_attached_console(self) -> None:
        with run_device_tests.Hardware() as platform:
            platform.attach_console(self.console)

        self.console.prebuffer(b"after cleanup\r\n")
        platform.discard_console_input()

        self.assertEqual(
            self.console.read(run_device_tests.CONSOLE_DRAIN_CHUNK_SIZE_BYTES),
            b"after cleanup\r\n",
        )

    def test_hardware_get_console_detaches_attached_console(self) -> None:
        board = run_device_tests.HELIPILOT_CONFIG
        self._stub_dut_control(b"")
        with run_device_tests.Hardware() as platform:
            platform.attach_console(self.console)
            self.assertIsNone(platform.get_console(board))
            self.console.prebuffer(b"after get_console\r\n")
            platform.discard_console_input()

        self.assertEqual(
            self.console.read(run_device_tests.CONSOLE_DRAIN_CHUNK_SIZE_BYTES),
            b"after get_console\r\n",
        )

    def test_flash_and_run_test_attaches_console_before_power_cycle(
        self,
    ) -> None:
        self._stub_build_and_run_test()
        fake_socket = _FakeSocket()
        self.enterContext(
            mock.patch.object(
                socket, "socket", autospec=True, return_value=fake_socket
            )
        )
        platform = _FakePlatform()
        args = self._make_args(remote="127.0.0.1", console_port=10000)
        test = run_device_tests.TestConfig(test_name="abort", toggle_power=True)

        passed = run_device_tests.flash_and_run_test(
            test,
            platform,
            run_device_tests.HELIPILOT_CONFIG,
            args,
        )

        self.assertTrue(passed)
        self.assertEqual(
            platform.attached_consoles, [fake_socket.console, None]
        )
        self.assertEqual(
            platform.events,
            [
                "flash",
                "attach_console",
                "hw_write_protect(False)",
                "power(False)",
                "discard_console_input",
                "power(True)",
                "detach_console",
                "cleanup",
            ],
        )


if __name__ == "__main__":
    unittest.main()
