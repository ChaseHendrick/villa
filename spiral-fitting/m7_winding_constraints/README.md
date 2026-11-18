# Winding constraints from m7 slices

Make relative winding constraints for `fit_spiral.py` from a binary surface
prediction such as m7, with nobody labeling windings. In each chosen z slice
the sheets are thinned to pieces, every piece gets a winding number in one
solve, and only the pieces the solve is surest of are written, as a point
collection per slice with `wind_a` for `vc_object_ingest --point-collections`.
A plus-one control file, every step between winding levels made one larger,
is written beside it: a fit taught by the control should get worse. If it
does not, the gain is not coming from the windings.

Everything here is model output: a label is where the prediction's sheets
put a winding, not evidence of ink.

Needs numpy, scipy and zarr. Every step is a module run from `spiral-fitting/`
with `python -m m7_winding_constraints.<module>`; `--help` lists its options.

## Steps

| Step | Module | Reads | Writes |
| --- | --- | --- | --- |
| 0. Winding direction | `outward_sense` | Surface prediction zarr, umbilicus | `sense.json`: `call` CW, ACW or abstain, with votes per plane |
| 1. Slice labels | `label_slices` | Surface prediction zarr, umbilicus, `sense.json`, z list | `zNNNNN.npz` per slice, `runs.jsonl`, `params.json` |
| 2. Constraints | `ladders` | The slice labels | `relative_windings.json`, `relative_windings_control.json`, `ladders.json` |
| 3. Fit | `vc_object_ingest`, `fit_spiral.py` | The constraints file | The fit, as usual |

**0. Winding direction.** For a volume without the catalog's
`z_direction_is_top_to_bottom` and `left_handed_coordinates`, the fit needs
`spiral_outward_sense`. `outward_sense` thins single z planes of the surface
prediction into sheet chains, keeps chains that wind at least one full turn
around the umbilicus, and votes by whether the radius grows or shrinks over a
turn. Each plane is also tracked mirrored and its votes added flipped, so a
mirrored volume gives exactly the opposite call. Too few votes, or a split
vote, abstains. `--mirror` runs the control: the call must flip. The call goes
into the input manifest's `scroll.spiral_outward_sense` and, through
`--sense`, sets the direction the slice labels count in. This is the same code
as `band_unroll/outward_sense.py`.

**1. Slice labels.** Per slice: threshold the prediction, drop chunk-fill
tiles, thin to one-pixel lines, prune short spurs, cut the lines at junctions,
at every crossing of the start ray and every 40 pixels into pieces. Two kinds
of edge join pieces: continuity (consecutive pieces of one line carry the
same label, or one more or less across the start ray) and radial (march out
along the normal from sampled pixels to the next sheet, which should be one
winding higher; a step of two costs little, so a sheet the prediction missed
is allowed for, a step of zero costs a lot). The costs are convex and
piecewise linear with integer breakpoints, so the best integer labels come
from one linear program (scipy's HiGHS), exactly. Each connected component of
the piece graph has its own offset; the `comp` array says which. Without
`--sense`, or when it abstains, the direction is tracked on each slice by
following sheets once round the umbilicus.

**2. Constraints.** A piece is bad when it touches an edge its labels break,
and sure when it is at least 3 pieces from every bad piece. Per slice the sure
pieces of one component are written (the component with the most of them),
8 points per piece, numbered by angle from the start ray so the fit's walk
along a collection never crosses it; a slice whose neighboring points are pi
or more apart is refused. `wind_a` is the piece's label minus the slice's
smallest written label, and every collection has `winding_is_absolute` false.
Points are in the fit volume's voxels: `--matrix` (a 3 x 4 affine, such as the
catalog transform from the prediction's scan to the fit's) and `--fit-scale`
(full-resolution voxels per fit-volume voxel) map the prediction's level-0
voxels there. `--control` also writes the plus-one control.

## Quick start

Paths and values in capitals are placeholders. The umbilicus is in the
prediction's level-0 voxels, as `{"control_points": [{"x", "y", "z"}, ...]}`.

```sh
cd spiral-fitting
python -m m7_winding_constraints.outward_sense --prediction SURFACE_PREDICTION.zarr --level 1 \
    --umbilicus umbilicus.json --pitch SPACING --z-range Z0,Z1 --out sense.json
python -m m7_winding_constraints.label_slices --prediction SURFACE_PREDICTION.zarr \
    --umbilicus umbilicus.json --sense sense.json --z Z_1 Z_2 ... Z_10 --jobs 4 --out slices/
python -m m7_winding_constraints.ladders --slices slices/ --fit-scale 4 --control --out constraints/
vc_object_ingest --db dataset.vcobj --author "$USER" --derive-uuids "$NS" \
    --volume-uuid VOLUME --scale-factor 1 \
    --point-collections constraints/relative_windings.json
python fit_spiral.py --inputs spiral-inputs.json --object-store dataset.vcobj
```

`NS` and `VOLUME` are the dataset's UUID namespace and reference volume, as in
"One-time dataset ingest" in `spiral-fitting/README.md`; `--scale-factor 1`
because the points are in fit-volume voxels. About 10 slices spread over the
z range is a good start. For the control, ingest
`relative_windings_control.json` into a second store and fit against that.
Score both fits on slices the constraints never touch.

Lengths in `label_slices.PARAMS` (minimum radius, piece length, the march to
the next sheet, the direction tracker's radii) are pixels of the plane read,
set for level 0 of a scan near 9 um; pass `--params '{"r_min": ...}'` for
another resolution. A synthetic 6,621 px square slice with 106 windings took
52 s and 2.1 GB on one CPU core (half of it the solve); real slices have
messier sheets and take longer.

## Limits

- The labels follow the surface prediction: where it merges two sheets or
  bridges a gap, the solve can only spread the error, and the sure rule keeps
  the pieces next to it out.
- Labels compare only inside one component of one slice; nothing ties the
  winding offsets of two slices together. That is what the fit does.
- The rule (D_SURE 3, K 8 and the costs) was fixed before the tests it was
  made for, which ran on villa `e2254ebe` with `patch_radius_loss_margin` 0.3
  and `loss_weight_unattached_pcl_radius` 0.5. On villa main the fit reads
  these collections through the object store and thins an unattached
  collection to `pcl_unattached_pcl_min_point_spacing` along its point order.
- The thinning is a plain numpy Zhang-Suen; it can differ from other
  skeletonizations by a pixel at junctions, which moves piece cuts slightly.

## Tests

```sh
cd spiral-fitting && python -m pytest -q tests/test_m7_winding_constraints.py
```

They build small synthetic spirals (no download) and check that the outward
sense is right in both directions and flips with a mirrored plane, abstains
on closed rings, the slice labels match the planted windings in both
directions (also with a sheet missing over a quarter turn and holes), the
wrong direction leaves broken edges that the sure rule finds, wrongly labeled
pieces and a second component stay out of the file, the file follows the
point-collection rules the ingest checks, the plus-one control changes every
step and nothing else, and the three commands run end to end on a local zarr.
