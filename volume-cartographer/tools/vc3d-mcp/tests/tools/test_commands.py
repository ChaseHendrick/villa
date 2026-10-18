#!/usr/bin/env python3
"""Self-test for vc3d_list_commands.

A small fake bridge answers command.list, so the test checks that the tool
sends the method with no params and returns the catalog unchanged.

Run with:
    python3 -m unittest tests.tools.test_commands -v
"""

from __future__ import annotations

import os
import shutil
import tempfile
import unittest
from typing import Any

from tests.support import FakeBridgeServer
from vc3d_mcp import core
from vc3d_mcp.tools.commands import vc3d_list_commands

_COMMANDS = [
    {
        "id": "command_palette",
        "title": "Command Palette...",
        "category": "Help",
        "shortcut": "Ctrl+K",
        "checkable": False,
        "checked": None,
        "enabled": True,
    },
    {
        "id": "draw_bbox",
        "title": "Draw BBox",
        "category": "Actions",
        "shortcut": None,
        "checkable": True,
        "checked": False,
        "enabled": True,
    },
]


class CommandBridge(FakeBridgeServer):
    """Answers command.list like the real catalog."""

    async def handle_message(self, message: dict[str, Any], writer) -> None:
        if message.get("method") == "command.list":
            await self.reply(writer, message.get("id"), result={"commands": _COMMANDS})
            return
        await super().handle_message(message, writer)


class CommandToolsTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.tmp_dir = tempfile.mkdtemp(prefix="vc3d-commands-test-")
        self.socket_path = os.path.join(self.tmp_dir, "fake-bridge.sock")
        self.fake = CommandBridge(self.socket_path)
        await self.fake.start()
        core.configure_client(self.socket_path, request_timeout=5)

    async def asyncTearDown(self) -> None:
        await core._get_client().close()
        await self.fake.stop()
        shutil.rmtree(self.tmp_dir, ignore_errors=True)

    async def test_list_sends_no_params_and_returns_the_catalog(self) -> None:
        result = await vc3d_list_commands()
        self.assertEqual(self.fake.received_requests[-1]["method"], "command.list")
        self.assertEqual(self.fake.params_for("command.list"), {})
        self.assertEqual(result, {"commands": _COMMANDS})


if __name__ == "__main__":
    unittest.main()
