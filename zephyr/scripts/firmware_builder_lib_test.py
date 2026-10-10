# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Unit tests for firmware_builder_lib."""

import argparse
import types
import unittest

from scripts import firmware_builder_lib


def _create_mock_project(name: str) -> types.SimpleNamespace:
    """Creates a mock project object mimicking zmake.project.Project."""
    return types.SimpleNamespace(
        config=types.SimpleNamespace(project_name=name)
    )


class TestShardProjects(unittest.TestCase):
    """Tests for shard_projects function."""

    def test_sorts_and_shards_project_objects(self):
        """Test sorting and sharding project objects with config.project_name."""
        projects = [
            _create_mock_project("skyrim"),
            _create_mock_project("brya"),
            _create_mock_project("corsola"),
            _create_mock_project("dedede"),
        ]
        opts = argparse.Namespace(shard_count=2, shard_index=1)
        result1 = firmware_builder_lib.shard_projects(list(projects), opts)
        self.assertEqual(
            [p.config.project_name for p in result1], ["brya", "corsola"]
        )

        opts.shard_index = 2
        result2 = firmware_builder_lib.shard_projects(list(projects), opts)
        self.assertEqual(
            [p.config.project_name for p in result2], ["dedede", "skyrim"]
        )

    def test_sorts_and_shards_string_board_names(self):
        """Test sorting and sharding iterable of string board names."""
        boards = ["sanok", "bloonchipper", "helipilot", "chudow"]
        opts = argparse.Namespace(shard_count=4, shard_index=1)
        self.assertEqual(
            firmware_builder_lib.shard_projects(list(boards), opts),
            ["bloonchipper"],
        )

        opts.shard_index = 2
        self.assertEqual(
            firmware_builder_lib.shard_projects(list(boards), opts),
            ["chudow"],
        )

        opts.shard_index = 3
        self.assertEqual(
            firmware_builder_lib.shard_projects(list(boards), opts),
            ["helipilot"],
        )

        opts.shard_index = 4
        self.assertEqual(
            firmware_builder_lib.shard_projects(list(boards), opts),
            ["sanok"],
        )

    def test_unsharded_preserves_all_boards(self):
        """Test that missing opts or unset shard flags preserves all boards."""
        boards = ["sanok", "bloonchipper", "helipilot", "chudow"]
        # None opts
        self.assertEqual(
            firmware_builder_lib.shard_projects(list(boards), None),
            ["bloonchipper", "chudow", "helipilot", "sanok"],
        )

        # Unset shard flags
        opts = argparse.Namespace(shard_count=None, shard_index=None)
        self.assertEqual(
            firmware_builder_lib.shard_projects(list(boards), opts),
            ["bloonchipper", "chudow", "helipilot", "sanok"],
        )

    def test_invalid_shard_index_raises(self):
        """Test that invalid shard index bounds raise ValueError."""
        boards = ["a", "b"]
        opts = argparse.Namespace(shard_count=2, shard_index=3)
        with self.assertRaises(ValueError):
            firmware_builder_lib.shard_projects(list(boards), opts)

        opts = argparse.Namespace(shard_count=2, shard_index=0)
        with self.assertRaises(ValueError):
            firmware_builder_lib.shard_projects(list(boards), opts)


if __name__ == "__main__":
    unittest.main()
