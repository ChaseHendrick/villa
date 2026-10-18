"""Regions of interest: 3D boxes and ray arcs kept with the project."""

from __future__ import annotations

from typing import Any, Literal, Optional, TypedDict

from typing_extensions import Required

from mcp.server.fastmcp import Context

from ..core import _call, _strip_none, _wait_for_job, mcp


class Point3D(TypedDict):
    x: float
    y: float
    z: float


class RegionShape(TypedDict, total=False):
    """A region shape, in the current volume's voxel coordinates.

    type "box": min and max are inclusive voxel bounds (x, y, z).

    type "ray_arc": an annular sector around the project's umbilicus over a
    z range. thetaStartDeg is in [0, 360), measured from the seam toward +y;
    thetaSweepDeg is in [0.1, 360]. radiusMin and radiusMax are XY distances
    from the umbilicus center of each slice, at least 1 voxel apart. zMin and
    zMax are inclusive slices. seam ("+x", "-x", "+y" or "-y", default "+x"
    for a new arc) is the direction theta = 0 points from the umbilicus. The
    umbilicus itself is never sent: VC3D embeds the project's resolved
    umbilicus when the arc is created. To aim an arc at a point, read the
    umbilicus center from a dry run (computed.umbilicusCenter) and take the
    angle and distance of your point from it.
    """

    type: Required[Literal["box", "ray_arc"]]
    min: Point3D
    max: Point3D
    thetaStartDeg: float
    thetaSweepDeg: float
    radiusMin: float
    radiusMax: float
    zMin: float
    zMax: float
    seam: Literal["+x", "-x", "+y", "-y"]


@mcp.tool()
async def vc3d_list_regions(id: Optional[str] = None, full: bool = False) -> dict[str, Any]:
    """List the project's regions (3D boxes and ray arcs marked in VC3D), or
    one region by id.

    Returns {"directory", "regions": [doc], "loadErrors": [...]}. Each doc has
    id, name, color, tags, status ("open"/"resolved"), notes, author,
    created, modified, revision, frame, shape and read-only fields:

    - shape is in the current volume's voxel coordinates when shapeFrame is
      "volume". A region drawn on another scan, or whose grid or voxel size
      does not fit the open volume, has compatible: false, an
      incompatibleReason, and its shape in its own frame (shapeFrame
      "region").
    - computed.center is the point to pass to vc3d_center_viewer before
      vc3d_screenshot to look at the region; computed.bounds is its bounding
      box; computed.bytesPerLevel lists {level, voxels, bytes} so you can see
      what a statistics run costs at each pyramid level before starting one;
      computed.autoLevel is the level vc3d_region_stats picks by default;
      computed.voxelEstimateLevel0 is the full-resolution voxel count;
      computed.stats lists saved statistics (volumeId, level, stale, path).
    - A ray arc also has computed.umbilicusCenter, the umbilicus center at
      its middle slice: radii are measured from it, and angles from it
      toward the shape's seam.
    - readOnly regions (written by a newer VC3D, or with unknown fields) are
      listed and can be deleted, but VC3D never rewrites or measures them.

    A ray arc's embedded umbilicus control points are included only when you
    pass id or full=true (they can be long).
    """
    return await _call("regions.list", _strip_none({"id": id, "full": full or None}))


@mcp.tool()
async def vc3d_save_region(
    id: Optional[str] = None,
    expected_revision: Optional[int] = None,
    name: Optional[str] = None,
    color: Optional[str] = None,
    tags: Optional[list[str]] = None,
    status: Optional[Literal["open", "resolved"]] = None,
    notes: Optional[str] = None,
    shape: Optional[RegionShape] = None,
    dry_run: bool = False,
) -> dict[str, Any]:
    """Create a region (omit id) or update one (give id), then return its doc.

    Create: shape is required, e.g.
      {"type": "box", "min": {"x": 4100, "y": 3900, "z": 6000},
                      "max": {"x": 4356, "y": 4156, "z": 6128}}
    or a ray arc (needs a project umbilicus):
      {"type": "ray_arc", "thetaStartDeg": 30, "thetaSweepDeg": 25,
       "radiusMin": 900, "radiusMax": 1400, "zMin": 6000, "zMax": 6400}
    Name and color default like the GUI ("Region N", a palette color).

    Update: fields you send replace, fields you omit stay. A shape with the
    same type patches only the fields present (sending only zMax changes only
    zMax); a shape with another type replaces it and needs all of that type's
    fields. Pass expected_revision (the doc's revision) to refuse a stale
    write: a mismatch fails with data.param "expectedRevision" and
    data.currentRevision.

    Coordinates are the current volume's voxel coordinates; VC3D stores
    regions in the scan's annotation frame (rounded to whole voxels there),
    so a region drawn on a downsampled store covers the same anatomy on the
    full-resolution one. Points outside the volume fail with -32003.

    revision, author, created, modified, frame and the computed fields are
    set by VC3D; sending them fails with data.param naming the field. A ray
    arc without a usable project umbilicus fails with data.kind
    "umbilicus" (attach one with File > Attach Umbilicus..., or put an
    umbilicus.json next to the project file and reopen the project).

    dry_run=true makes every check a save makes and returns the doc that
    would be saved (with a ray arc's umbilicus control points and
    computed.umbilicusCenter), writing nothing. The project must be saved
    (regions live next to the project file).
    """
    return await _call(
        "regions.save",
        _strip_none(
            {
                "id": id,
                "expectedRevision": expected_revision,
                "name": name,
                "color": color,
                "tags": tags,
                "status": status,
                "notes": notes,
                "shape": shape,
                "dryRun": dry_run or None,
            }
        ),
    )


@mcp.tool()
async def vc3d_delete_region(id: str, expected_revision: Optional[int] = None) -> dict[str, Any]:
    """Delete a region: its file (and its saved statistics) move to the
    regions folder's .trash. Returns {"deleted": true, "id", "trashPath"}.
    expected_revision refuses a stale delete like vc3d_save_region does."""
    return await _call("regions.delete", _strip_none({"id": id, "expectedRevision": expected_revision}))


@mcp.tool()
async def vc3d_region_stats(
    ids: list[str],
    volume_id: Optional[str] = None,
    level: Optional[int] = None,
    threshold: Optional[float] = None,
    percentiles: Optional[list[float]] = None,
    wait: bool = True,
    ctx: Optional[Context] = None,
) -> dict[str, Any]:
    """Measure the voxels inside 1 to 64 regions on one volume, as a job.

    volume_id: a loaded project volume the regions fit (default: the current
    volume). level: a stored pyramid level, or omit it for Auto (the finest
    level with at most 32 Mi voxels in the region); check
    computed.bytesPerLevel from vc3d_list_regions first. Level L voxel i is the sample at full-resolution
    point i * 2^L. threshold adds count_above / fraction_above (voxels with
    value > threshold). percentiles (0 to 100, at most 4 decimals) default to
    0.5, 1, 5, 25, 50, 75, 95, 99, 99.5 and follow numpy's inverted CDF.

    Returns {"jobId", "kind": "regions.stats", "source": "region"}; with
    wait=true (the default) it blocks until the job finishes (30-minute cap)
    and returns the terminal job.status, whose result is
    {"stats": [stats doc], "outputPaths": [...]}. Each stats doc has count,
    min, max, sum and sum_squares (decimal strings), mean, std (population),
    percentiles, a 256-bin histogram (uint16 values are binned v >> 8),
    volume_mm3, chunks_read and fill_chunks, and is also saved under the
    regions folder. Use wait=false and vc3d_wait_job / vc3d_cancel_job for
    long runs. Only one region job runs at a time (-32004 otherwise), and it
    may be one the person started from the Regions dock. A region with more
    than 8 GiB of voxels at the level is refused (data.param "level").
    """
    params = _strip_none(
        {
            "ids": ids,
            "volumeId": volume_id,
            "level": level,
            "threshold": threshold,
            "percentiles": percentiles,
        }
    )
    result = await _call("regions.stats", params)
    return await _wait_for_job(result["jobId"], wait, result, ctx)


@mcp.tool()
async def vc3d_export_region(
    id: str,
    volume_id: Optional[str] = None,
    level: Optional[int] = None,
    output_dir: Optional[str] = None,
    wait: bool = True,
    ctx: Optional[Context] = None,
) -> dict[str, Any]:
    """Save one region's voxels at one pyramid level as a local zarr crop,
    as a job (the Regions dock's Export crop).

    volume_id and level work as in vc3d_region_stats (omit level for Auto).
    output_dir must be an absolute path to an empty or missing folder; omit
    it for a new folder under the regions folder's exports/. Crops above
    8 GiB are refused (pick a coarser level; computed.bytesPerLevel from
    vc3d_list_regions shows the size first).

    The folder gets volume.zarr (one level, the source's dtype),
    manifest.json (the region, the source volume and level, and
    crop.crop_index_zyx_to_annotation_xyz, the matrix that places crop
    voxels in the scan) and, for a ray arc, mask.zarr (255 inside the arc, 0
    outside). Returns {"jobId", "kind": "regions.export", "source":
    "region", "level"}; with wait=true (the default) it blocks until the job
    finishes and returns the terminal job.status, whose result is
    {"manifest", "outputPaths": [folder]}. Only one region job runs at a
    time (-32004 otherwise); vc3d_cancel_job removes a partial crop.
    """
    params = _strip_none({"id": id, "volumeId": volume_id, "level": level, "outputDir": output_dir})
    result = await _call("regions.export", params)
    return await _wait_for_job(result["jobId"], wait, result, ctx)
