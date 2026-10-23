**In one sentence:** In VC3D you can now grow a segment past its edge by hand: pick a side and a vertex count (or press 1 to 4 in Manual Add), and the new strip follows the sheet in the CT; Shift+click where it still misses, then apply.

**One real example:** Starting with an 80x60 vertex crop of the published PHerc0139 w035 mesh on the public 9.362 um volume, I pressed Extend Right with 20 vertices and Apply, with no clicks. The segment was saved with its grid grown to 80x80. The 1600 new vertices sit a mean 3.7 voxels from the real w035 surface; with "Extensions follow the sheet" turned off (the straight guess), the same extension is 45.6 voxels off.

**Before:** Manual Add only filled holes inside the grid. There was no manual way to add surface past an edge; the only options were the tracers.

**After this PR:**
- The Manual Add panel has an "Extend edge" row (vertex count plus Up, Down, Left, Right), and keys 1 / 2 / 3 / 4 do the same in Manual Add. Pressing the same key again applies the pending strip and starts the next one from the new edge, so you can keep walking out.
- Shift+drag along an edge extends only that stretch of it.
- The strip first shows as a straight continuation, then, in the background, each new row or column is re-walked along the sheet using the structure tensor normals from vc_reader's `sheet_normal`. It took about a minute for 80 lines of 20 vertices on the remote volume with nothing cached; the UI stays usable meanwhile. A checkbox turns this off.
- Shift+clicks in slice views pull the strip onto the sheet without moving the existing surface; on a sheet-following strip a click only corrects its own neighborhood. Shift+right-click removes a click. Apply or Cancel finish it; the grid is padded when there is no room past the edge and the padding is removed on cancel.
- Ctrl+Z after Apply undoes the extension, including when rows or columns were added at the top or left.
- The agent bridge and vc3d-mcp can do all of this headlessly.

**Proof:** Extend Right by 20 vertices on the public PHerc0139 volume and the published w035 mesh (VC3D screen captures). The cyan line in the XY slice is where the new strip cuts that slice:

![Manual Add edge extension](https://raw.githubusercontent.com/ChaseHendrick/villa/pr-assets-manual-extend/manual-extend/manual-extend.gif)

Where the new strip cuts the XY slice, from the same two VC3D captures (the thin cyan intersection line redrawn thick so it is visible): the straight guess in red, the same extension after it follows the sheet in yellow, no clicks:

![Straight guess vs follows the sheet](https://raw.githubusercontent.com/ChaseHendrick/villa/pr-assets-manual-extend/manual-extend/strip-in-xy-slice.png)

How close the new strip lands to the sheet: I cut 16 edge windows out of a published mesh (4 sides x 4 positions), extended each one, and measured each new vertex's distance to the real surface. A simulated user Shift+clicks where the strip is worst. Median over the 16 windows of the mean distance, in voxels. I tuned on w035 only, then froze the rule and ran w045 (PHerc0139, never used while developing) once.

| Segment | Extension | Guess | 0 clicks | 1 | 2 | 4 | 8 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| w035 (tuning) | 10 vertices | straight | 16.6 | 10.1 | 7.1 | 6.1 | 4.1 |
| w035 (tuning) | 10 vertices | follows the sheet | 3.2 | 2.9 | 2.9 | 2.7 | 2.2 |
| w035 (tuning) | 20 vertices | straight | 35.7 | 19.6 | 16.8 | 14.4 | 8.0 |
| w035 (tuning) | 20 vertices | follows the sheet | 4.5 | 4.3 | 4.3 | 3.9 | 3.4 |
| w045 (held out) | 10 vertices | straight | 12.0 | 9.6 | 7.5 | 6.3 | 3.4 |
| w045 (held out) | 10 vertices | follows the sheet | 2.7 | 3.0 | 3.1 | 2.4 | 2.2 |
| w045 (held out) | 20 vertices | straight | 31.5 | 22.7 | 19.9 | 13.4 | 7.1 |
| w045 (held out) | 20 vertices | follows the sheet | 4.1 | 6.1 | 5.5 | 4.7 | 3.9 |

Following the sheet was closer than straight on all 16 windows of both segments at 0 clicks. The w045 click columns were run before the local click falloff existed (clicks then spread over the whole strip, which is why they made it slightly worse); the falloff was chosen afterwards on w035 only (reach 0, 2, 4, 8; 4 was best), so the w045 click numbers above are for the older click behavior. The test compares against the true mesh only; it does not run a tracer.

**Why / where this is useful:** For annotators fixing or finishing segments by hand, especially where the tracer runs off the sheet or stops. A community member asked on Discord for a sane way to extend meshes manually in VC3D; this is that. Following the sheet with vc_reader's sheet normals was Sean's suggestion on Discord.

- [ ] I personally verified that the example and proof above were produced by this PR on the stated data.

## Motivation

<!-- Chase: please rewrite this in your own words; CONTRIBUTING asks for human-written commentary on LLM-assisted PRs. -->
I was working on PHerc0139 segments and saw a question on Discord about extending meshes by hand in VC3D, which I also could not do: Manual Add only fills holes, and growing past an edge meant running a tracer. While testing this I found that Manual Add fills under a mask.tif were hidden again on reload, and that Shift+click in a slice could move a vertex far from the plane, so those are fixed here too.

## Details

Seven commits:

1. **Extend a segment's edge with Manual Add.** `ManualAddTool::beginExtension` and grid padding (`segmentation::resizeSurfaceGrid` pads the points and every channel, loading channels still only on disk, and shifts correction anchors when rows or columns are added at the top or left). Two save fixes found on the way:
   - Manual Add fills under a `mask.tif` were saved but hidden on the next load, because the mask still marked those cells invalid. Filled cells are now marked valid in the mask. The test `maskedFillSurvivesReload` failed before this fix.
   - Autosave's vertex-delta path assumed the cached save copy matched the surface size; after a size change it now writes the whole surface.
2. **Shift+click in a slice picks the vertex nearest in 3D.** It used to pick the vertex nearest in the plane's 2D projection, however far off the plane it was. When several rows project onto the same spot, as along an extended edge cut by an XY slice, the click moved a vertex from the wrong row and pulled the strip off the sheet.
3. **Panel buttons** for the extension.
4. **Agent bridge and vc3d-mcp:** `segmentation.manual_add.extend` and `segmentation.manual_add.add_constraint` (constrain at a world point), with SPEC.md, the rpc_description.json snapshot and the method counts updated.
5. **Keep extending, keys, partial edges, undo.** Keys 1 to 4 (the Grow keys; arrow keys already pan the slice views) extend in Manual Add, and the same key again applies and continues once autosave has finished. Shift+drag picks the nearest edge and the stretch it covers (`ManualAddTool::edgeFromDrag`). `SegmentationUndoHistory::padGrid` shifts undo deltas and pads snapshots when the grid grows, so undo keeps working instead of being cleared. The help dialog lists the new keys.
6. **Agent bridge:** `applyPending` on `segmentation.manual_add.extend` for the same walk-on behavior.
7. **Extensions follow the sheet.** `SheetNormalField` reads a 109^3 patch per query on a 3-voxel lattice (cached per volume) and calls `vcr::sheet_normal::cpu`; `ManualAddTool::walkSheetLine` projects each step into the plane across the normal at its midpoint and keeps the step length. It runs on a worker pool off the UI thread, and the result is dropped if the extension changed meanwhile. The bridge reports it as `followingSheet` and `manualAddFollowingSheet` in `state.get`. Note for review: `normal.hpp` is header-only and included from `vc_reader/src/cpp` through an include directory in VC3D's CMakeLists rather than copied; happy to move it if you prefer another layout.

Tried and left out: snapping the strip to the CT profile (won only 10 of 16 tuning windows, by 3 to 4%) and a tint for how far a vertex is from the sheet (no better than chance at finding the bad vertices).

Not changed: the tracer path of Manual Add is untouched.

Tested on `2896c5aea` (Linux, QuickBuild): `test_manual_add_extend` 18 of 18 pass; `agent_bridge/test/smoke_offscreen.py` passes with 122 methods; `tools/vc3d-mcp` pytest 167 passed, 2 skipped. The keys, drag and undo were checked in the running app under Xvfb; the screen captures came from this build.

Data credit: Vesuvius Challenge open data, CC BY-NC 4.0 (PHerc0139 volume `20250728140407-9.362um-1.2m-113keV-masked.zarr`, segments `20260317000000-w035_2026031718` and `20260126000000-w045_2026012619`).
