# Copyright 2021 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Tests for zmake packers."""

import pathlib
import subprocess
import tempfile
from unittest import mock

# pylint:disable=import-error
import hypothesis
import hypothesis.strategies as st
import pytest
import zmake.output_packers as packers


# Strategies for use with hypothesis
absolute_path = st.from_regex(regex=r"\A/[\w/]*\Z")


class FakePacker(packers.BasePacker):
    """Fake packer to expose protected methods."""

    def __init__(self, max_size):
        super().__init__(project=None)
        self.max_size = max_size

    def check_packed_file_size(self, file, dir_map):
        """Expose the _check_packed_file_size method."""
        return self._check_packed_file_size(file, dir_map)

    def _get_max_image_bytes(self, _dir_map):
        return self.max_size

    def pack_firmware(self, _work_dir, _jobclient, _dir_map, version_string=""):
        """See base class."""
        del version_string
        assert False


@hypothesis.given(st.binary(min_size=101, max_size=200))
@hypothesis.settings(deadline=60000)
def test_file_size_unbounded(data):
    """Test with file size unbounded."""
    packer = FakePacker(None)
    with tempfile.TemporaryDirectory() as temp_dir_name:
        file = pathlib.Path(temp_dir_name) / "zephyr.elf"
        with open(file, "wb") as outfile:
            outfile.write(data)
        assert packer.check_packed_file_size(file=file, dir_map={}) == file


@hypothesis.given(st.binary(min_size=5, max_size=100))
@hypothesis.settings(deadline=60000)
def test_file_size_in_bounds(data):
    """Test with file size limited."""
    packer = FakePacker(100)
    with tempfile.TemporaryDirectory() as temp_dir_name:
        file = pathlib.Path(temp_dir_name) / "ec.bin"
        with open(file, "wb") as outfile:
            outfile.write(data)
        assert packer.check_packed_file_size(file=file, dir_map={}) == file


@hypothesis.given(st.binary(min_size=101, max_size=200))
@hypothesis.settings(deadline=60000)
def test_file_size_out_of_bounds(data):
    """Test with file size limited, and file exceeds limit."""
    packer = FakePacker(100)
    with tempfile.TemporaryDirectory() as temp_dir_name:
        file = pathlib.Path(temp_dir_name) / "ec.bin"
        with open(file, "wb") as outfile:
            outfile.write(data)
        with pytest.raises(RuntimeError):
            packer.check_packed_file_size(file=file, dir_map={})


def test_binman_packer_timeout_kills_process(tmp_path):
    """Test BinmanPacker kills and reaps binman on TimeoutExpired."""
    ro_dir = tmp_path / "ro"
    rw_dir = tmp_path / "rw"
    work_dir = tmp_path / "work"
    (ro_dir / "zephyr").mkdir(parents=True)
    (rw_dir / "zephyr").mkdir(parents=True)
    work_dir.mkdir(parents=True)
    for build_dir in (ro_dir, rw_dir):
        (build_dir / "zephyr" / "zephyr.bin").write_bytes(b"\0")
        (build_dir / "zephyr" / "zephyr.elf").write_bytes(b"\0")

    project = mock.Mock()
    project.config.project_name = "telith"
    packer = packers.BinmanPacker(project)

    jobclient = mock.Mock()
    proc = mock.Mock()
    proc.wait.side_effect = [
        subprocess.TimeoutExpired(cmd="binman", timeout=300),
        0,
    ]
    jobclient.popen.return_value = proc

    with (
        mock.patch("zmake.multiproc.LogWriter.log_output"),
        mock.patch(
            "zmake.util.get_tool_path",
            side_effect=lambda tool: f"/usr/bin/{tool}",
        ),
        pytest.raises(subprocess.TimeoutExpired),
    ):
        list(
            packer.pack_firmware(
                work_dir, jobclient, {"ro": ro_dir, "rw": rw_dir}
            )
        )

    proc.wait.assert_any_call(timeout=300)
    proc.kill.assert_called_once_with()
    assert proc.wait.call_count == 2
