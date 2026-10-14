#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Connection pill mapping in muse_pill.c: the CompanionApp connection
bubble's labels and dot colours, and this gadget's Hatch link as a pill."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

# muse_pill_t order (muse_pill.h), mirroring the app's ConnectionState.
UNPAIRED, CONNECTING, CONNECTED, WAITING, STOPPED = range(5)

# muse_hatch_state_t order (muse_chat.h).
NOT_SET, OFFLINE, UNTESTED, TESTING, REACHABLE, UNREACHABLE = range(6)


class PillTest(unittest.TestCase):
    binary: Path

    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "muse_pill_harness"
        proc = subprocess.run(
            [
                *cc,
                "-include",
                str(ROOT / "tests" / "host_compat.h"),
                "-std=gnu11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-I",
                str(ROOT / "components" / "muse"),
                str(ROOT / "tests" / "muse_pill_harness.c"),
                str(ROOT / "components" / "muse" / "muse_pill.c"),
                "-o",
                str(cls.binary),
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if proc.returncode:
            raise AssertionError(proc.stdout + proc.stderr)

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def run_harness(self, *args: str) -> str:
        proc = subprocess.run(
            [str(self.binary), *args], capture_output=True, text=True
        )
        self.assertEqual(proc.returncode, 0, msg=proc.stderr)
        return proc.stdout.strip()

    def test_labels_verbatim_from_app(self) -> None:
        self.assertEqual(self.run_harness("label", str(CONNECTED)), "Connected")
        self.assertEqual(
            self.run_harness("label", str(CONNECTING)), "Connecting…"
        )
        self.assertEqual(
            self.run_harness("label", str(WAITING)), "Waiting to retry"
        )
        self.assertEqual(
            self.run_harness("label", str(UNPAIRED)), "Not paired"
        )
        self.assertEqual(self.run_harness("label", str(STOPPED)), "Stopped")

    def test_dots_green_amber_orange_grey(self) -> None:
        self.assertEqual(self.run_harness("dot", str(CONNECTED)), "6FF0BF")
        self.assertEqual(self.run_harness("dot", str(CONNECTING)), "FFC107")
        self.assertEqual(self.run_harness("dot", str(WAITING)), "FFB45C")
        self.assertEqual(self.run_harness("dot", str(UNPAIRED)), "8B84A8")
        self.assertEqual(self.run_harness("dot", str(STOPPED)), "8B84A8")

    def test_unknown_pill_is_grey_unknown(self) -> None:
        self.assertEqual(self.run_harness("label", "99"), "Unknown")
        self.assertEqual(self.run_harness("dot", "99"), "8B84A8")
        self.assertEqual(self.run_harness("label", "-1"), "Unknown")
        self.assertEqual(self.run_harness("dot", "-1"), "8B84A8")

    def test_from_link_reachable_is_connected(self) -> None:
        self.assertEqual(
            self.run_harness("from", str(REACHABLE), "0"),
            "Connected 6FF0BF",
        )
        self.assertEqual(
            self.run_harness("from", str(REACHABLE), "1"),
            "Connected 6FF0BF",
        )

    def test_from_link_testing_is_connecting(self) -> None:
        self.assertEqual(
            self.run_harness("from", str(TESTING), "1"),
            "Connecting… FFC107",
        )
        self.assertEqual(
            self.run_harness("from", str(UNTESTED), "0"),
            "Connecting… FFC107",
        )

    def test_from_link_unpaired_needs_no_pairing(self) -> None:
        self.assertEqual(
            self.run_harness("from", str(NOT_SET), "0"), "Not paired 8B84A8"
        )
        # A pairing via the Muse link is still working towards a session.
        self.assertEqual(
            self.run_harness("from", str(NOT_SET), "1"),
            "Connecting… FFC107",
        )

    def test_from_link_offline_is_waiting(self) -> None:
        self.assertEqual(
            self.run_harness("from", str(OFFLINE), "1"),
            "Waiting to retry FFB45C",
        )
        self.assertEqual(
            self.run_harness("from", str(UNREACHABLE), "1"),
            "Waiting to retry FFB45C",
        )

    def test_from_link_unknown_state_waits(self) -> None:
        self.assertEqual(
            self.run_harness("from", "99", "1"), "Waiting to retry FFB45C"
        )
        self.assertEqual(
            self.run_harness("from", "-1", "0"), "Waiting to retry FFB45C"
        )


if __name__ == "__main__":
    unittest.main()
