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

"""CompanionApp ports in muse_chat_text.c: "say it again" source picking,
reply image URL and identity avatar scans, and speak-generation counters."""

from __future__ import annotations

import os
import shlex
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class CompanionPortsTest(unittest.TestCase):
    binary: Path

    @classmethod
    def setUpClass(cls) -> None:
        cc = shlex.split(os.environ.get("CC", "cc"))
        if not cc or shutil.which(cc[0]) is None:
            raise unittest.SkipTest("C compiler not available")
        cls.tmp = tempfile.TemporaryDirectory()
        cls.binary = Path(cls.tmp.name) / "muse_serial_chat_harness"
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
                str(ROOT / "tests" / "muse_serial_chat_harness.c"),
                str(ROOT / "components" / "muse" / "muse_chat_text.c"),
                str(ROOT / "components" / "muse" / "muse_text.c"),
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

    def run_harness(self, *args: str, data: bytes = b"") -> str:
        proc = subprocess.run(
            [str(self.binary), *args], input=data, capture_output=True
        )
        self.assertEqual(proc.returncode, 0, msg=proc.stderr.decode())
        return proc.stdout.decode()

    def test_repeat_prefers_the_last_reply(self) -> None:
        self.assertEqual(
            self.run_harness("repeat", "status line", data=b"the reply"), "the reply"
        )
        self.assertEqual(
            self.run_harness("repeat", "-", data=b"  padded  "), "padded"
        )

    def test_repeat_falls_back_to_status(self) -> None:
        self.assertEqual(
            self.run_harness("repeat", "status line", data=b"   "), "status line"
        )
        self.assertEqual(self.run_harness("repeat", "-", data=b""), "(null)")
        self.assertEqual(
            self.run_harness("repeat", "  ", data=b"\n"), "(null)"
        )

    def test_repeat_rejects_listening(self) -> None:
        self.assertEqual(
            self.run_harness("repeat", "-", data="Listening…".encode()), "(null)"
        )

    def test_reply_image_url_finds_first_image(self) -> None:
        text = (
            "see https://example.com/page and "
            "https://cdn.example/muse.png?w=2 then https://cdn.example/b.gif"
        )
        self.assertEqual(
            self.run_harness("imgurl", data=text.encode()),
            "https://cdn.example/muse.png?w=2",
        )

    def test_reply_image_url_ignores_http_pages_and_bare_hosts(self) -> None:
        self.assertEqual(
            self.run_harness("imgurl", data=b"http://example.com/muse.png"), "(null)"
        )
        self.assertEqual(
            self.run_harness("imgurl", data=b"see https://example.com/about"), "(null)"
        )
        self.assertEqual(self.run_harness("imgurl", data=b"https://"), "(null)")

    def test_reply_image_url_trims_tail_and_matches_case(self) -> None:
        self.assertEqual(
            self.run_harness("imgurl", data=b"(https://cdn.example/a.JPG)."),
            "https://cdn.example/a.JPG",
        )
        self.assertEqual(
            self.run_harness("imgurl", data=b"[pic](https://cdn.example/a.webp)!"),
            "https://cdn.example/a.webp",
        )
        self.assertEqual(
            self.run_harness("imgurl", data=b"https://cdn.example/a_(1).png)"),
            "https://cdn.example/a_(1).png",
        )
        # A balanced trailing paren stays part of the URL, so the path no
        # longer ends in an image extension.
        self.assertEqual(
            self.run_harness("imgurl", data=b"https://cdn.example/a_((1).png)"),
            "(null)",
        )

    def test_identity_avatar_url_direct_and_nested(self) -> None:
        body = (
            b'{"name":"Muse","avatar_url":"https://cdn.example/muse.png",'
            b'"other":{"picture":"http://cdn.example/other.jpg"}}'
        )
        self.assertEqual(
            self.run_harness("idavatar", data=body), "https://cdn.example/muse.png"
        )
        nested = b'{"agent": {"portrait" : "data:image/png;base64,AAAA"}}'
        self.assertEqual(
            self.run_harness("idavatar", data=nested), "data:image/png;base64,AAAA"
        )

    def test_identity_avatar_url_rejects_non_images(self) -> None:
        self.assertEqual(
            self.run_harness("idavatar", data=b'{"avatar_url":"not a url"}'),
            "(null)",
        )
        self.assertEqual(
            self.run_harness("idavatar", data=b'{"name":"Muse"}'), "(null)"
        )
        # Case-insensitive keys, like the phone's lowercased lookup.
        self.assertEqual(
            self.run_harness("idavatar", data=b'{"Avatar":"https://cdn.example/m.png"}'),
            "https://cdn.example/m.png",
        )

    def test_speak_generations(self) -> None:
        out = self.run_harness("speakgen")
        self.assertEqual(
            out,
            "g1=1 g2=2 latest1=0 latest2=1\n"
            "wrap=1 latest3=1 null=0 zero=0\n"
            "nullclaim=0\n",
        )


if __name__ == "__main__":
    unittest.main()
