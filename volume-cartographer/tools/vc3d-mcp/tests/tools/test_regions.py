"""Standalone tests for the regions MCP tool module.

Exercises the @mcp.tool() wrappers in vc3d_mcp.tools.regions against a small
in-process fake Agent Bridge, asserting:
  * snake_case arguments become the RPC's camelCase keys and None (and the
    false defaults of full / dry_run) are stripped;
  * shapes pass through unchanged, a box and a ray arc alike;
  * regions.stats and regions.export honor wait vs no-wait like the other
    job tools;
  * a ray arc's seam passes through with the shape;
  * bridge errors (a stale revision with currentRevision, a missing
    umbilicus) reach the caller unchanged;
  * the agent-facing docstrings carry the facts an agent needs.

Run:
    python3 -m unittest tests.tools.test_regions -v
"""

from __future__ import annotations

import os
import shutil
import tempfile
import unittest

from tests.support import FakeBridgeServer
from vc3d_mcp import core
from vc3d_mcp.tools import regions as regions_module
from vc3d_mcp.tools.regions import (
    vc3d_delete_region,
    vc3d_export_region,
    vc3d_list_regions,
    vc3d_region_stats,
    vc3d_save_region,
)


class RegionsBridge(FakeBridgeServer):
    def __init__(self, socket_path: str):
        super().__init__(socket_path)
        self._polls = 0

    async def handle_message(self, msg: dict, writer) -> None:
        method = msg.get("method")
        req_id = msg.get("id")
        params = msg.get("params") or {}
        if method == "regions.list":
            await self.reply(writer, req_id, result={"directory": "/p/regions/x", "regions": [], "loadErrors": []})
        elif method == "regions.save":
            if params.get("expectedRevision") == 1:
                await self.reply(writer, req_id, error={
                    "code": -32602, "message": "Stale revision",
                    "data": {"param": "expectedRevision", "currentRevision": 3}})
            elif params.get("shape", {}).get("type") == "ray_arc" and "id" not in params:
                await self.reply(writer, req_id, error={
                    "code": -32007, "message": "No umbilicus",
                    "data": {"kind": "umbilicus"}})
            else:
                await self.reply(writer, req_id, result={"id": params.get("id", "r-new"), "revision": 1, **params})
        elif method == "regions.delete":
            await self.reply(writer, req_id, result={"deleted": True, "id": params["id"], "trashPath": "/t"})
        elif method == "regions.stats":
            await self.reply(writer, req_id, result={"jobId": "job-7", "kind": "regions.stats", "source": "region"})
        elif method == "regions.export":
            await self.reply(writer, req_id, result={"jobId": "job-8", "kind": "regions.export", "source": "region", "level": 1})
        elif method == "job.status":
            self._polls += 1
            state = "running" if self._polls < 2 else "succeeded"
            await self.reply(writer, req_id, result={
                "jobId": params.get("jobId"), "kind": "regions.stats", "source": "region",
                "state": state, "message": "done" if state == "succeeded" else "working",
                "result": {"stats": [{"count": 10}], "outputPaths": ["/s.json"]} if state == "succeeded" else None,
                "consoleTail": []})
        else:
            await self.reply(writer, req_id, error={"code": -32601, "message": "Method not found"})


class RegionsToolTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.tmp_dir = tempfile.mkdtemp(prefix="vc3d-regions-test-")
        self.socket_path = os.path.join(self.tmp_dir, "fake-bridge.sock")
        self.bridge = RegionsBridge(self.socket_path)
        await self.bridge.start()
        core.configure_client(self.socket_path, request_timeout=5)
        self._orig_poll = core.POLL_INTERVAL_S
        core.POLL_INTERVAL_S = 0.01

    async def asyncTearDown(self) -> None:
        core.POLL_INTERVAL_S = self._orig_poll
        await core._get_client().close()
        await self.bridge.stop()
        shutil.rmtree(self.tmp_dir, ignore_errors=True)

    async def test_list_strips_defaults(self) -> None:
        await vc3d_list_regions()
        self.assertEqual(self.bridge.params_for("regions.list"), {})
        await vc3d_list_regions(id="r1", full=True)
        self.assertEqual(self.bridge.params_for("regions.list"), {"id": "r1", "full": True})

    async def test_save_box_maps_names(self) -> None:
        box = {"type": "box", "min": {"x": 1, "y": 2, "z": 3}, "max": {"x": 10, "y": 20, "z": 30}}
        result = await vc3d_save_region(name="jump", tags=["sheet_jump"], shape=box, dry_run=True)
        self.assertEqual(
            self.bridge.params_for("regions.save"),
            {"name": "jump", "tags": ["sheet_jump"], "shape": box, "dryRun": True},
        )
        self.assertEqual(result["id"], "r-new")

    async def test_save_partial_arc_update(self) -> None:
        await vc3d_save_region(id="r1", expected_revision=3, shape={"type": "ray_arc", "zMax": 6400})
        self.assertEqual(
            self.bridge.params_for("regions.save"),
            {"id": "r1", "expectedRevision": 3, "shape": {"type": "ray_arc", "zMax": 6400}},
        )

    async def test_stale_revision_error_passes_through(self) -> None:
        with self.assertRaises(Exception) as caught:
            await vc3d_save_region(id="r1", expected_revision=1, name="x")
        text = str(caught.exception)
        self.assertIn("expectedRevision", text)
        self.assertIn("3", text)

    async def test_missing_umbilicus_error_passes_through(self) -> None:
        arc = {"type": "ray_arc", "thetaStartDeg": 30, "thetaSweepDeg": 25,
               "radiusMin": 900, "radiusMax": 1400, "zMin": 6000, "zMax": 6400}
        with self.assertRaises(Exception) as caught:
            await vc3d_save_region(shape=arc)
        self.assertIn("umbilicus", str(caught.exception))

    async def test_delete(self) -> None:
        result = await vc3d_delete_region("r1", expected_revision=2)
        self.assertEqual(self.bridge.params_for("regions.delete"), {"id": "r1", "expectedRevision": 2})
        self.assertTrue(result["deleted"])

    async def test_stats_no_wait_returns_job(self) -> None:
        result = await vc3d_region_stats(["r1", "r2"], level=2, threshold=128.0, wait=False)
        self.assertEqual(
            self.bridge.params_for("regions.stats"),
            {"ids": ["r1", "r2"], "level": 2, "threshold": 128.0},
        )
        self.assertEqual(result["jobId"], "job-7")
        self.assertNotIn("state", result)

    async def test_stats_wait_returns_terminal_result(self) -> None:
        result = await vc3d_region_stats(["r1"])
        self.assertEqual(self.bridge.params_for("regions.stats"), {"ids": ["r1"]})
        self.assertEqual(result["state"], "succeeded")
        self.assertEqual(result["result"]["stats"][0]["count"], 10)

    async def test_export_no_wait_maps_names(self) -> None:
        result = await vc3d_export_region("r1", level=1, output_dir="/tmp/crop", wait=False)
        self.assertEqual(
            self.bridge.params_for("regions.export"),
            {"id": "r1", "level": 1, "outputDir": "/tmp/crop"},
        )
        self.assertEqual(result["jobId"], "job-8")

    async def test_export_wait_returns_terminal_status(self) -> None:
        result = await vc3d_export_region("r1")
        self.assertEqual(self.bridge.params_for("regions.export"), {"id": "r1"})
        self.assertEqual(result["state"], "succeeded")

    async def test_arc_seam_passes_through(self) -> None:
        shape = {"type": "ray_arc", "zMax": 12, "seam": "-y"}
        await vc3d_save_region(id="arc-1", shape=shape)
        self.assertEqual(self.bridge.params_for("regions.save"), {"id": "arc-1", "shape": shape})

    def test_docstrings_carry_agent_facts(self) -> None:
        listing = " ".join((vc3d_list_regions.__doc__ or "").split())
        self.assertIn("computed.center", listing)
        self.assertIn("vc3d_center_viewer", listing)
        self.assertIn("bytesPerLevel", listing)
        save = " ".join((vc3d_save_region.__doc__ or "").split())
        self.assertIn("current volume's voxel coordinates", save)
        self.assertIn("expected_revision", save)
        self.assertIn("same type patches only the fields present", save)
        self.assertIn("umbilicus", save)
        stats = " ".join((vc3d_region_stats.__doc__ or "").split())
        self.assertIn("vc3d_wait_job", stats)
        self.assertIn("inverted CDF", stats)
        export = " ".join((vc3d_export_region.__doc__ or "").split())
        self.assertIn("absolute path", export)
        self.assertIn("crop_index_zyx_to_annotation_xyz", export)
        self.assertIn("mask.zarr", export)
        self.assertIn("umbilicusCenter", listing)
        shape_doc = " ".join((regions_module.RegionShape.__doc__ or "").split())
        self.assertIn("never sent", shape_doc)
        self.assertIn('default "+x"', shape_doc)


if __name__ == "__main__":
    unittest.main()
