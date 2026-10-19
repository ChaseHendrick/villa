#!/usr/bin/env python3
"""Self-test for the theme.* MCP tools.

Uses the shared echo bridge to check that each tool sends the right bridge
method and params, including the defaults the bridge documents.

Run with:
    python3 -m unittest tests.tools.test_theme -v
"""

from __future__ import annotations

import os
import shutil
import tempfile
import unittest

from tests.support import EchoBridgeServer
from vc3d_mcp import core
from vc3d_mcp.tools.theme import (
    vc3d_export_theme,
    vc3d_import_theme,
    vc3d_list_themes,
    vc3d_set_theme,
)


class ThemeToolTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.tmp_dir = tempfile.mkdtemp(prefix="vc3d-theme-test-")
        self.socket_path = os.path.join(self.tmp_dir, "fake-bridge.sock")
        self.fake = EchoBridgeServer(self.socket_path)
        await self.fake.start()
        core.configure_client(self.socket_path, request_timeout=5)

    async def asyncTearDown(self) -> None:
        await core._get_client().close()
        await self.fake.stop()
        shutil.rmtree(self.tmp_dir, ignore_errors=True)

    def _assert_wire(self, method: str, params: dict) -> None:
        req = self.fake.received_requests[-1]
        self.assertEqual(req["method"], method)
        self.assertEqual(req.get("params", {}), params)

    async def test_list_rescans_only_when_asked(self) -> None:
        await vc3d_list_themes()
        self._assert_wire("theme.list", {"rescan": False})
        await vc3d_list_themes(rescan=True)
        self._assert_wire("theme.list", {"rescan": True})

    async def test_set_persists_by_default(self) -> None:
        await vc3d_set_theme("modern-dark")
        self._assert_wire("theme.set", {"id": "modern-dark", "persist": True})
        await vc3d_set_theme("classic", persist=False)
        self._assert_wire("theme.set", {"id": "classic", "persist": False})

    async def test_import_does_not_overwrite_by_default(self) -> None:
        await vc3d_import_theme("/tmp/ocean.vc3dtheme")
        self._assert_wire("theme.import", {"path": "/tmp/ocean.vc3dtheme", "overwrite": False})
        await vc3d_import_theme("/tmp/ocean", overwrite=True)
        self._assert_wire("theme.import", {"path": "/tmp/ocean", "overwrite": True})

    async def test_export_sends_id_only_when_given(self) -> None:
        await vc3d_export_theme("/tmp/ocean.vc3dtheme")
        self._assert_wire("theme.export", {"path": "/tmp/ocean.vc3dtheme", "overwrite": False})
        await vc3d_export_theme("/tmp/light.vc3dtheme", id="modern-light", overwrite=True)
        self._assert_wire(
            "theme.export", {"path": "/tmp/light.vc3dtheme", "overwrite": True, "id": "modern-light"}
        )


if __name__ == "__main__":
    unittest.main()
