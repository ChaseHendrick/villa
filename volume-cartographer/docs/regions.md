# VC3D regions

A region marks a part of a scan you want to come back to: a sheet jump in a
prediction, a delamination, odd texture. VC3D draws it in the slice views,
keeps it with the project, measures the voxels inside it and saves them as a
zarr crop. This page gives the file formats and every rule VC3D uses, so a
region can be checked or used without VC3D.

## Using regions

Open the Regions dock (View menu). The project must be saved, because regions
are stored next to the project file.

- **3D Box**: drag with the left button in the XY, XZ or YZ view. The drag sets
  the two axes of that view; the third axis gets the dock's depth (default 64
  voxels), centered on the current slice. A click without a drag selects the
  region under the cursor. After a box is drawn the tool switches to Edit, and
  the focus point moves to the box's center (keeping the drawn slice), so the
  other two slice views show its depth; like any focus change, this also
  recenters the segmentation view.
- **Edit**: drag the active box's corner or edge handles, or its center handle
  (the ring) to move the whole box. Only handles take a press: a click
  anywhere else, inside the box too, keeps its usual meaning (selecting a
  point, starting a fiber). On a rotated vertical plane only the top and
  bottom (z) handles exist; tilted planes have none. Where a handle and the XY
  view's plane-rotation handle overlap, the region's handle wins; turn the
  tool off to rotate there.
- **Esc** cancels a drag, or leaves the tool. Shift, Ctrl and right-button
  input keep their usual meaning while a tool is on. Region edits have no
  undo; Ctrl+Z keeps its segmentation meaning.
- **Delete** moves the region file to the regions folder's `.trash/`.
- **Use as focus box** sets the session focus box to exactly the region, so
  the XY, XZ and YZ views dim the voxels outside it (the Surface view does not
  dim). Untick BBox in the View dock to turn it off.
- **Measure** computes statistics of the voxels inside the region on a chosen
  volume and level; **Export crop** saves them as a local zarr volume.

## Coordinates

Three coordinate systems are involved.

- **Region frame** (stored): integer voxel coordinates, x y z order, of the
  annotation frame the region was drawn in: the scan's level-0 source grid,
  the frame fibers and the umbilicus use. A region records that grid as
  `frame.coordinate_base_shape_zyx` and, for an open-data volume, the
  `vc_open_data_*` identity keys.
- **Viewer frame**: level-0 coordinates of the open store; the frame of the
  slice planes, the focus box and the agent bridge. A region point `r` is the
  viewer point `v = r * k`.
- **Store level L**: index `i` of level L of a store is the viewer point
  `v = i * 2^L`. Level L voxel `i` is the sample at level-0 point `i * 2^L`,
  not the center of its `2^L` footprint (VC3D's level transforms are pure
  scales with zero offset).

`k` for a region on a volume V:

```
B   = region.frame.coordinate_base_shape_zyx
E_V = the annotation-frame extent of V (z y x), rounded
f_V = V's annotation-frame factor (1, 2^level of an open-data identity, or 2^baseScaleLevel)
N_V = E_V / f_V, V's own voxel counts
d   = f_V * dyadicCoordinateScaleBetweenShapes(B, N_V)
k   = d / f_V
```

`dyadicCoordinateScaleBetweenShapes` accepts extents at the same level that
differ by one voxel, and a coarser level whose counts are the finer ones
divided by a power of two, rounded up or down. Comparing against `N_V`
rather than `E_V` matters for a rebased store: a 20974-voxel source stored at
level 2 has 5244 voxels, so `E_V` is 20976.

A region fits V when the coordinate spaces do not conflict, `d` exists, the
voxel sizes agree within a relative 1e-6 when both are known, and `f_V` is a
power of two. Otherwise the dock lists the region greyed with the reason.

## Boxes

A box stores inclusive voxel bounds `min` and `max` in the region frame,
`0 <= min <= max <= extent - 1` per axis. A region point `r` is inside when
`min <= r <= max` on every axis.

- Level-0 voxel count in the region frame: `prod(max - min + 1)`.
- Viewer box (the focus box): `[min * k, max * k]`.
- Outline: the plane's cross-section of the face box `[min - 0.5, max + 0.5] * k`.
  It encloses exactly the counted voxels; the focus-box dimming tests voxel
  centers, so it sits half a voxel inside the outline.
- Index range at level L, per axis, clipped to `[0, shape_L - 1]`:

```
i_min = ceil (min * k / 2^L)
i_max = floor(max * k / 2^L)
```

  A level-L voxel is counted exactly when its viewer point `i * 2^L` maps to a
  region point inside the box. The range is empty when `i_min > i_max` on any
  axis.

## Region file

One file per region: `<project dir>/regions/<sanitized project file name>/<id>.region.json`.
An unsaved project has no regions folder, and nothing is written.

```json
{
  "schema": "vc3d.region",
  "version": 1,
  "id": "6f1c2c1e-4a7b-4f43-9a7e-2b2d9a0e9c11",
  "name": "sheet jump near x 4200",
  "color": "#ffb000",
  "tags": ["sheet_jump"],
  "status": "open",
  "notes": "prediction jumps one sheet outward",
  "author": "chase",
  "created": "2026-10-18T14:03:11Z",
  "modified": "2026-10-18T14:05:40Z",
  "revision": 3,
  "frame": {
    "coordinate_base_shape_zyx": [14376, 7888, 8096],
    "voxel_size_um": 7.91,
    "volume_id": "20241024131838-7.910um-53keV-masked",
    "vc_open_data_coordinate_space": "PHerc0172/20241024131838@L0"
  },
  "shape": {"type": "box", "min": {"x": 4100, "y": 3900, "z": 6000}, "max": {"x": 4356, "y": 4156, "z": 6128}}
}
```

- `status` is `open` or `resolved`; `color` is `#rrggbb`; `id` is 1 to 64 of
  `[A-Za-z0-9_-]` and equals the file name stem.
- A file with `version` above 1, or with a key this version does not know,
  loads read-only with the reason in its tooltip, and VC3D never rewrites it.
- Writes go through a temporary file renamed into place, under the folder lock
  `.regions.lock`; every change re-reads the file's `revision` first and
  refuses when another writer changed it.
- Delete moves the file, and its statistics folder, into `.trash/`. VC3D does
  not restore it from there; move the file back by hand.
- The folder name comes from the project file name, so after Save As or a
  rename the regions stay in the old project's folder (fibers behave the same
  way); copy `regions/<old project file>/` to keep them.

## Statistics

`stats/<id>/<volume id>-L<level>.json` under the regions folder, one per volume
and level. The dock marks a result stale when `region_revision` differs from
the region's revision. Supported dtypes are uint8 and uint16, the only ones a
VC3D volume can have. A region with more than 8 GiB of voxels at the chosen
level is refused; choose a coarser level. Chunks are requested a few at a time
ahead of the readers, so Cancel stops the downloads too.

- Voxel set: the level-L index range above. Chunks are read directly, each
  once, clipped to the level's shape, so zero padding never enters. A chunk
  missing from the store counts as the fill value, as zarr reads it, and is
  counted in `fill_chunks`.
- From the value histogram `h[v]` (256 or 65536 bins): `count = sum h`; `min`
  and `max` are the smallest and largest `v` with `h[v] > 0`; `sum` and
  `sum_squares` are exact integers (decimal strings in the file).
- `mean = sum / count`; `std = sqrt((count * sum_squares - sum^2) / count^2)`
  (population, numpy `ddof=0`), the numerator exact in 128-bit integers.
- Percentile `p` (0 to 100, at most 4 decimals): `p = 0` gives `min`;
  otherwise the smallest `v` whose cumulative count reaches
  `ceil(round(p * 10^4) * count / 10^6)`. This is numpy
  `percentile(..., method="inverted_cdf")`.
- `histogram.counts`: 256 bins, `bin = v` for uint8 and `v >> 8` for uint16.
- `count_above` counts voxels with value greater than `threshold`;
  `fraction_above = count_above / count`.
- `volume_mm3 = count * (voxel_size_um * 2^L)^3 * 1e-9`.
- Auto level: the finest stored level whose range has at most 32 Mi voxels.

Every number comes from an integer histogram merged by addition, so the thread
count and chunk order cannot change any result.

## Crop export

The output folder must be absent or empty. The crop is built in
`<out>.partial-<8 hex>` and renamed to `<out>` at the end; `manifest.json` is
written last, and the partial folder is removed on cancel or failure.

- `volume.zarr`: one level, the source dtype, chunks of at most 64 per axis,
  uncompressed, voxel size `voxel_size_um * 2^L`.
- `manifest.json` (`"schema": "vc3d.region_export"`): the region document, the
  source volume, the level, the crop's `index_origin_zyx` and `shape_zyx` at
  level L, its viewer and annotation origins, and
  `crop_index_zyx_to_annotation_xyz`, the 4x4 matrix from a crop index
  (z, y, x) to annotation coordinates (x, y, z):
  `a = (i0 + i) * 2^L / k`.
- Exports above 8 GiB (uncompressed) are refused; choose a coarser level.
  `vc_zarr_to_tiff` converts the crop to TIFF.

The crop opens in VC3D as its own local volume with coordinates starting at 0;
use the matrix to place it in the scan.

## Checking a box without VC3D

This reads a region file and a stats file and recomputes the statistics from
the zarr with numpy. It needs `numpy` and `zarr` (and `s3fs` for an `s3://`
volume).

```python
import json, math, sys
import numpy as np
import zarr

region = json.load(open(sys.argv[1]))       # <id>.region.json
stats = json.load(open(sys.argv[2]))        # stats/<id>/<volume>-L<level>.json
level = stats["level"]
k = stats["region_to_viewer"]               # power of two
arr = zarr.open(stats["volume"]["location"], mode="r")[str(level)]

lo, hi = region["shape"]["min"], region["shape"]["max"]
start, stop = [], []
for axis, n in zip("zyx", arr.shape):
    s = k / 2**level
    i0 = max(0, math.ceil(lo[axis] * s))
    i1 = min(n - 1, math.floor(hi[axis] * s))
    start.append(i0); stop.append(i1 + 1)
a = np.asarray(arr[start[0]:stop[0], start[1]:stop[1], start[2]:stop[2]]).ravel()

h = np.bincount(a)                          # exact per-value counts
assert a.size == stats["count"]
assert a.min() == stats["min"] and a.max() == stats["max"]
assert sum(int(c) * v for v, c in enumerate(h)) == int(stats["sum"])
assert sum(int(c) * v * v for v, c in enumerate(h)) == int(stats["sum_squares"])
shift = 8 if a.dtype == np.uint16 else 0
assert np.bincount(a >> shift, minlength=256).tolist() == stats["histogram"]["counts"]
for entry in stats["percentiles"]:
    assert np.percentile(a, entry["p"], method="inverted_cdf") == entry["value"]
if stats["threshold"] is not None:
    assert int((a > stats["threshold"]).sum()) == stats["count_above"]
print("all values match")
```
