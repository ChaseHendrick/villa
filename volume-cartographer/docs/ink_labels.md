# Ink labels

VC3D can draw ink detection training labels directly on a segment's flattened
view and save them as the files the ink detection trainer reads. Labels are
drawn at the exact pixel grid `vc_render_tifxyz` writes for the segment, so
they line up with its renders without any manual alignment.

The label tools are the **Ink labels** section at the bottom of the
**Ink Detection** dock (**View → Ink Detection**). **View → Ink label mode**
turns label mode on and off too.

## Label mode

Select a segment, then press **I** (or **Label mode** in the dock). The
segment view's left mouse button now paints labels; the right button still
pans and the wheel still zooms. Label mode is refused while segmentation
editing is on, because editing moves the grid the labels are attached to.
Turning label mode on turns approval painting off, and turning editing or
approval painting on turns label mode off at the next click. While label mode
is on, the status bar shows the class, tool and radius, for example
"Ink labels: ink, brush r 12".

Every label pixel has three independent bits, as the trainer sees them:

* **Ink (W)** marks the pixel as ink and as supervised.
* **Background (F)** marks it as supervised and not ink. With
  **Background keeps ink** checked, painting background over ink keeps the ink.
* **Validation (V)** marks the pixel as part of the validation area.
* **Eraser (Z)** returns ink and background to unknown (not supervised, so
  the trainer ignores the pixel) and clears validation. To turn ink into
  background, paint background instead.

## Tools

* **Brush.** Click to stamp a disc, drag to paint, **Shift+Click** for a
  straight stroke from the last point. **Ctrl+Mouse Wheel** changes the radius
  (0.5 to 512 label pixels, which are voxels at full resolution).
* **Polygon.** Click vertices and press **Enter** or double-click to fill, or
  drag a lasso and release. **Backspace** removes the last vertex, **Esc**
  cancels. Self-intersecting outlines fill even-odd.

**L** switches tools. Each stroke, polygon or Shift+Click segment is one undo
step: **Ctrl+Z** undoes and **Ctrl+Shift+Z** (or **Ctrl+Y**) redoes, also when
a dock control has focus. Label undo is separate from segmentation undo. The
history keeps whole operations up to 256 MiB. Switching to another segment
keeps unsaved labels (as an autosave) but ends their undo history; switching
back restores the labels with a fresh history.

## Display

**H** shows or hides the labels; **Space** still toggles the ink prediction
below them. Ink is drawn red, validation yellow and supervised background
blue; the dock sets which of them are shown and their opacity. When zoomed out,
blocks of label pixels are pooled: a block is red if any pixel is ink, yellow
if any is validation, solid blue if all are supervised and faint blue if some
are. Labels are not drawn on the volumetric composite camera.

## Files

Labels live outside the segment folder, in

```text
<project folder>/ink_labels/<segment folder name>/
  <seg>_inklabels.tif              ink           (written by Save)
  <seg>_supervision_mask.tif       supervised    (written by Save)
  <seg>_validation_mask.tif        validation    (only when used)
  <seg>_inklabels.json             sidecar: canvas, hashes, edit counts
  *.tif.prev                       the previous saved generation
  *.autosave                       autosave checkpoints
  <seg>_inklabels.lock             held while a VC3D has the labels open
```

A project opened from the open-data catalog keeps its project file in the
remote cache, which can be wiped and is shared by every sample, so its labels
go to `<application data folder>/open_data_labels/<sample>/ink_labels/` instead
(on Linux `~/.local/share/Vesuvius Challenge/VC3D/...`). The dock shows the folder in use. The
folder and its lock file are created at the first edit, so opening a segment
writes nothing.

**Save (Ctrl+S)** writes the three TIFFs as uint8 0/255, one sample, tiled
1024, LZW, at the canvas size `vc_render_tifxyz --scale 1` produces for the
segment, then the sidecar. The trainer and `create_label_zarrs` read the TIFFs
and ignore the other files. The sidecar records the canvas (grid size and the
scale's exact float bits), the segment uuid, a hash of the segment's geometry
and a SHA-256 of each file.

* **Autosave.** Two minutes after the last checkpoint, once painting has paused
  for two seconds, unsaved labels are written to the `.autosave` files. They
  never replace the trainer files. Switching segment or closing the project
  checkpoints too. After a crash, opening the labels offers to restore the
  checkpoint. A save deletes the checkpoint, so an undo followed by a save
  can never bring the undone strokes back. A checkpoint that cannot be
  restored (written for another canvas, or with an unreadable sidecar) is
  renamed to `*.unusable`, never deleted.
* **Outside edits.** If a trainer file changed since VC3D loaded or saved it
  (an edit in another program, or an interrupted save), Save asks before
  replacing it; the replaced file stays as `.prev`.
* **Another VC3D.** A second VC3D opening the same labels gets them read-only,
  with the holder's process and host in the dock. If both opened a store that
  did not exist yet, the first to paint takes it and the other turns read-only
  at its first edit.
* **A changed segment.** If the segment's grid or scale changed since the
  labels were saved (a re-render or a regrown segment), the labels lock and are
  neither shown nor saved; the dock names both canvases. To carry labels
  across a re-render use `vesuvius.tifxyz_label_transfer`. A geometry edit
  that keeps the grid only warns: labels stay on the same grid positions.
* **Closing VC3D** with unsaved labels asks to save, discard or cancel.

## Pixel mapping

Label pixel `(c, r)` covers `[c, c+1) x [r, r+1)` of the canvas of size
`W = max(1, round(cols / scale_x))` by `H = max(1, round(rows / scale_y))`.
Its grid position is

```text
grid_x = cols/2 + scale_x * (x - W/2)
grid_y = rows/2 + scale_y * (y - H/2)
```

which at pixel centers is exactly the position `vc_render_tifxyz` samples for
output pixel `(c, r)`. The formula lives in `vc/core/util/RenderCanvas.hpp`,
which the renderer uses too.

## Keys

| Key | Action |
| --- | --- |
| I | Toggle ink label mode |
| W, F, V | Paint ink, background, validation |
| Z | Toggle the eraser |
| L | Switch tools |
| H | Show or hide the labels |
| Ctrl+Z, Ctrl+Shift+Z | Undo, redo (in label mode) |
| Ctrl+S | Save (in label mode) |
| Ctrl+Mouse Wheel | Brush radius |
| Shift+Click | Straight stroke from the last point |
| Enter, Backspace, Esc | Close, shorten or cancel a polygon |

The keys work wherever focus is in the main window, including the docks and the
dock bar's pop-up, while the **main** (segment) workspace is
the current tab; other workspaces keep their own bindings of these letters.
Enter, Backspace and Esc are taken only while a polygon is open.
