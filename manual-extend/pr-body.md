**In one sentence:** In VC3D you can now grow a segment past its edge by hand: pick a side and a vertex count, Shift+click where the sheet really is in a slice view, and apply.

**One real example:** Starting with an 80x60 vertex crop of the published PHerc0139 w035 mesh on the public 9.362 um volume, I pressed Extend Right with 20 vertices, Shift+clicked the sheet twice in the XY slice, and pressed Apply; the segment was saved with its grid grown to 80x80.

**Before:** Manual Add only filled holes inside the grid. There was no manual way to add surface past an edge; the only options were the tracers.

**After this PR:** The Manual Add panel has an "Extend edge" row (vertex count plus Up, Down, Left, Right). The preview continues each row or column straight along its own edge direction. Shift+clicks in slice views bend the new strip onto the sheet without moving the existing surface, Shift+right-click removes a click, and Apply or Cancel finish it. The grid is padded when there is no room past the edge, and the padding is removed again on cancel.

**Proof:** Extend Right by 20 vertices, two Shift+clicks on the sheet in the XY slice, Apply (VC3D screen captures, public PHerc0139 volume, published w035 mesh):

![Manual Add edge extension](https://raw.githubusercontent.com/ChaseHendrick/villa/pr-assets-manual-extend/manual-extend/manual-extend.gif)

Before and after (left: the segment ends at the focus point; right: after Apply, with the applied surface's intersection in the XY slice):

![Before and after](https://raw.githubusercontent.com/ChaseHendrick/villa/pr-assets-manual-extend/manual-extend/before-after.png)

How close the new strip lands to the sheet: I cut 16 edge windows out of the published w035 mesh (4 sides x 4 positions), extended each one, and measured each new vertex's distance to the real w035 surface. A simulated user clicks where the preview is worst. Median over the 16 windows of the mean distance, in voxels:

| Extension | 0 clicks | 1 | 2 | 4 | 8 |
| --- | --- | --- | --- | --- | --- |
| 10 vertices | 16.6 | 10.1 | 7.1 | 6.1 | 4.1 |
| 20 vertices | 35.7 | 19.6 | 16.8 | 14.4 | 8.0 |

So the straight preview is a starting guess and the clicks do the work. This compares against the true mesh only; there is no tracer baseline here because the test does not run the tracer.

**Why / where this is useful:** For annotators fixing or finishing segments by hand, especially where the tracer runs off the sheet or stops. A community member asked on Discord for a sane way to extend meshes manually in VC3D; this is that.

- [ ] I personally verified that the example and proof above were produced by this PR on the stated data.

## Motivation

I was working on PHerc0139 segments and saw a question on Discord about extending meshes by hand in VC3D, which I also could not do: Manual Add only fills holes, and growing past an edge meant running a tracer. While testing this I found that Manual Add fills under a mask.tif were hidden again on reload, and that Shift+click in a slice could move a vertex far from the plane, so those are fixed here too.

## Details

Four commits:

1. **Extend a segment's edge with Manual Add.** `ManualAddTool::beginExtension` and grid padding (`segmentation::resizeSurfaceGrid` pads the points and every channel, loading channels still only on disk, and shifts correction anchors when rows or columns are added at the top or left). Two save fixes found on the way:
   - Manual Add fills under a `mask.tif` were saved but hidden on the next load, because the mask still marked those cells invalid. Filled cells are now marked valid in the mask. The test `maskedFillSurvivesReload` failed before this fix.
   - Autosave's vertex-delta path assumed the cached save copy matched the surface size; after a size change it now writes the whole surface.
2. **Shift+click in a slice picks the vertex nearest in 3D.** It used to pick the vertex nearest in the plane's 2D projection, however far off the plane it was. When several rows project onto the same spot, as along an extended edge cut by an XY slice, the click moved a vertex from the wrong row and pulled the strip off the sheet. I found this while recording the GIF above.
3. **Panel buttons** for the extension.
4. **Agent bridge and vc3d-mcp:** `segmentation.manual_add.extend` and `segmentation.manual_add.add_constraint` (constrain at a world point), with SPEC.md, the rpc_description.json snapshot and the method counts updated.

Not changed: the tracer path of Manual Add is untouched, and an extension always uses the thin plate spline. Undo history is cleared only when rows or columns are added at the top or left, since older undo entries no longer address the right vertices.

Tested on `cc86569ce` (Linux, QuickBuild): `test_manual_add_extend` 13 of 13 pass; `agent_bridge/test/smoke_offscreen.py` passes with 122 methods; `tools/vc3d-mcp` pytest 167 passed, 2 skipped. The screen captures came from this build driven through the agent bridge under Xvfb.

Data credit: Vesuvius Challenge open data, CC BY-NC 4.0 (PHerc0139 volume `20250728140407-9.362um-1.2m-113keV-masked.zarr` and segment `20260317000000-w035_2026031718`).
