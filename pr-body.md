**In one sentence:** Point at a spot in any view and press Ctrl+C (⌘C on a Mac) to copy its volume coordinates as `x, y, z`, or press Ctrl+Shift+C to copy the focus point, without clicking and without moving the views.

**One real example:** On a crop of the public PHerc0139 scan, I pointed at a bright inclusion in the XY view and pressed Ctrl+C. The clipboard held `507, 415, 192`, the same numbers as the view's `[X=507, Y=415, Z=192]` readout, and the focus point stayed at `384, 384, 192`. Pasting another copied spot, `200, 486, 192`, into the Focus field and pressing Enter brought the views to it.

**Before:** VC3D already shows the position under the mouse and can copy the focus point, but nothing copies the spot you are pointing at, and no key copies anything:
- Each view's `[X=, Y=, Z=]` readout follows the mouse, but it is a label: it cannot be selected or copied.
- The **Copy** button next to the Focus field copies the text in that field.
- To copy any other spot, you first make it the focus point with Ctrl+click (or R with the mouse over it). That recenters every view on it. Then you click **Copy**.

**After this PR:**
- **Ctrl+C** copies the position under the mouse in whichever view the mouse is over, the same whole-voxel numbers as that view's readout. The focus point and the views do not move.
- **Ctrl+Shift+C** copies the focus point, the same numbers as the Focus field and the Copy button.
- Both write the `x, y, z` form the Focus field accepts, so a copied spot can be pasted there to go back to it.
- Both keys are listed in Help > Keybinds and in the README, and the Copy button's tooltip names its key.
- Nothing that exists changes: Ctrl+click, R and the Copy button work as before, and Ctrl+C in a text field, the console or a list still copies from there.

| To copy | Before | After this PR |
| --- | --- | --- |
| The spot under the mouse | Ctrl+click it (or press R over it), which recenters every view there, then click Copy | Ctrl+C; the views stay where they are |
| The focus point | Click Copy | Ctrl+Shift+C, or click Copy |
| From the keyboard | No key | Either key |

**Proof:** The same spot copied both ways from the same starting view. Left, the existing route: Ctrl+click recenters every view on the spot, then Copy. Right, Ctrl+C copies the same `507, 415, 192` and nothing moves. Look at the views at the end and at the two clipboard boxes:

![the same spot copied with Ctrl+click and Copy (left) and with Ctrl+C (right)](https://raw.githubusercontent.com/ChaseHendrick/villa/pr-assets-copy-coords/vc3d/before-after.gif)
![end of both routes: two steps and the views moved, one key and they did not](https://raw.githubusercontent.com/ChaseHendrick/villa/pr-assets-copy-coords/vc3d/before-after.png)

The whole flow: point, Ctrl+C, Ctrl+Shift+C, paste a copied spot into Focus to go back to it, and the two new lines in Help > Keybinds:

![pointing at a spot, Ctrl+C, Ctrl+Shift+C, pasting a copied position into Focus, and Help > Keybinds](https://raw.githubusercontent.com/ChaseHendrick/villa/pr-assets-copy-coords/vc3d/copy-coordinates.gif)

![Ctrl+C over the XY view: the clipboard matches the view's readout](https://raw.githubusercontent.com/ChaseHendrick/villa/pr-assets-copy-coords/vc3d/copy-cursor.png)
![Ctrl+Shift+C: the clipboard matches the Focus field](https://raw.githubusercontent.com/ChaseHendrick/villa/pr-assets-copy-coords/vc3d/copy-focus.png)
![the two new lines in Help > Keybinds](https://raw.githubusercontent.com/ChaseHendrick/villa/pr-assets-copy-coords/vc3d/keybinds.png)

Recorded from this branch's VC3D under Xvfb on the crop below. The caption bars, the pointer (Xvfb draws none), the clipboard boxes (read with `xclip` after each key) and the orange outlines are added to the frames; everything else is VC3D.

**Why / where this is useful:** Someone on Discord asked for a key to copy the POI or cursor coordinates, and I wanted the same thing. Right now you have to Ctrl+click the spot and then hit Copy, and the Ctrl+click recenters every view, so you lose your place. With this it is one key and the views stay where they are, which makes it quick to drop a spot into a message, a script or an issue.

- [ ] I personally verified that the example and proof above were produced by this PR on the stated data.

## Details

- Tested commit: `289a9beb9` on `main` at `39a8cf3a1`. One commit, 11 files, 373 lines added, 1 removed.
- **Ctrl+C** is handled in `CWindow::keyPressEvent`, which only sees a key that the focused widget did not take. So text fields and the console keep their own Ctrl+C. Lists and trees copy their current cell but can still pass the key on, so Ctrl+C is left to them while one has the keyboard focus, and so is a field drawn inside a view (such as the Rotate Surface angle box) while it has selected text.
- The position comes from a new `ViewerManager::hoveredVolumePosition()`. It reads only the view the mouse is actually over (not one covered by a panel), at the pixel the readout used, and samples it with `CChunkedVolumeViewer::sampleSceneVolume()`, the call behind the readout. Nothing is copied over an empty view, off a segment's mesh, outside the volume or with no volume open, and the status bar says why.
- **Ctrl+Shift+C** is a window shortcut, so it also works while typing in a field and from a floating panel. It copies the focus point itself, not an entry still being typed in the Focus field. It does not fire in separate windows such as the console output.
- In the Spiral tab both keys use that tab's views and focus point (from the code; not tried in the running app, see below).
- Numbers are whole voxels. Ctrl+C rounds like the readout; Ctrl+Shift+C drops the fraction like the Focus field, so after a Ctrl+click on a spot like `507.6, ...` the two can differ by 1. `-0` is written `0`.
- Limits: Ctrl+C does nothing while a text field, spin box or drop-down has the keyboard focus, or while a floating panel is active; click a view first, as with R. After panning with the arrow keys, the readout and Ctrl+C keep the last mouse position until the mouse moves. Ctrl+Shift+C copies the focus point even when it is outside the volume, as the Copy button does.
- Tests: new `test_coordinate_copy` (QtTest, CTest `coordinate_copy`, label `vc-core`), 8 of 8: the text form, that it passes the Focus field's validator, the volume bounds, when Ctrl+C is left to the focused widget, the two key chords, and that no other binding in Help > Keybinds uses them. `test_point_collection_widget`, `agent_bridge_contract`, `object_store_vc3d_controller` and the agent bridge offscreen smoke test (120 methods) pass too.
- In the running app with synthetic key presses, 12 of 12 checks pass: Ctrl+C in the XY view (two spots) and the YZ view matches `canvas.get_cursor_volume_point`; nothing is copied over an empty view or outside the volume; Ctrl+Shift+C matches the Focus field, copies the focus point rather than an unfinished entry being typed in the field, and also works while the surface list has the focus; Ctrl+C in the Focus field copies its selected text; Ctrl+C with the surface list focused leaves the clipboard alone; a pasted position moves the focus there; a plain click does not move the focus.
- On a synthetic segment with a hole, 6 of 6 pass: two spots on the mesh, the hole, off the mesh on both sides, and a view covered by a panel. These checks can fail: without the off-mesh check, both off-mesh spots copied `2, 2, 8`, a point nowhere near the mouse; without the covered-view check, Ctrl+C copied `498, 481, 139` from a view hidden under the panel; and without the list check, Ctrl+C with the surface list focused replaced the clipboard.
- Built and run on Linux (Ubuntu 24.04, Qt 6.4.2, gcc 13). Not tried in the running app: macOS, the Spiral tab, floating panels and the console window. On macOS Qt maps Ctrl to ⌘ by default and VC3D does not change that.
- Overlap with my #2224: they merge cleanly, but if #2224 goes in first, this test and the Ctrl+Shift+C shortcut need moving to its `helpRows()` list and command catalog. I will rebase whichever lands second.
- Merges cleanly with `main` and with the `tifxyz-to-obj-store` and `spiral-object-store` branches.
- Data: a 384 x 768 x 768 voxel crop (z 10336 to 10720, y 2700 to 3468, x 2100 to 2868) of PHerc0139 volume `20250728140407-9.362um-1.2m-113keV-masked.zarr`, Vesuvius Challenge open data, CC BY-NC 4.0. Coordinates in the pictures are in that crop.
