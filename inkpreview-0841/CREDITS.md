# Credits: live ink preview media on PHerc0841 ag896 (13 Nov 2026)

Every picture here shows model output, not a reading. The colored overlay is the output of the `scrollprize/ink_9um` model (seed 42, step 75,000) drawn by the VC3D live ink preview; it is not a transcription and no letters are claimed.

## Data

- Scroll and segment: PHerc0841, segment `auto_grown_20260220144552896` (folder `PHerc0841/segments/20260220214732-auto_grown_20260220144552896/`, mesh `mesh/intermediate/tifxyz_original/`), on volume `PHerc0841/volumes/20250821151531-9.366um-1.2m-113keV-masked.zarr`, level 0.
- Source: Vesuvius Challenge open data, https://vesuvius-challenge-open-data.s3.us-east-1.amazonaws.com, license CC BY-NC 4.0. Credit: PHerc0841, Vesuvius Challenge open data, CC BY-NC 4.0.
- PHerc0841 is not prize-eligible. No ink labels were read or shown.
- Model: `scrollprize/ink_9um` (`hybrid_3d2d-seed42/step-075000.pth`).

## How they were made

VC3D (villa branch `vc3d-ink-preview` at `17ef0d3`) under Xvfb at 1920 x 1080, against the villa preview server (branch `ink-preview-server` at `b0df13d9d`) on 127.0.0.1, CPU. Colormap Magma, opacity 70 %. All captures were taken on 13 Nov 2026 between 15:16 and 15:23 UTC, after both parity gates were recorded. Each image and GIF frame carries the line "ink_9um preview: model output, not a reading" and the credit. GIF frames hold 10 s each and the last holds 14 s. The two charts are drawn from the gate records (gate1.json, gate2.json of the ag896 parity check).

## Files

| File | Size (px) | Bytes | What it shows |
| --- | --- | --- | --- |
| `vc3d-preview-ag896-forward.png` | 1920x1144 | 1,649,676 | VC3D window, live ink preview on ag896, forward layers, Magma at 70 %, Ink Detection dock open |
| `vc3d-preview-ag896-reversed.png` | 1920x1144 | 1,639,334 | Same view with Reverse layers checked (the control view) |
| `vc3d-preview-ag896-hero.png` | 1920x1144 | 2,962,131 | VC3D with the segment view widened, preview across the view, dock open |
| `vc3d-preview-pan.gif` | 960x1114 | 2,154,824 | 3 frames (10 s, 10 s, 14 s): before a pan, after the pan while the new strip is computed, the new strip in place |
| `vc3d-preview-reverse-toggle.gif` | 960x1114 | 2,325,889 | 4 frames (10 s, 10 s, 10 s, 14 s): forward, Reverse layers checked and sampling, reversed, forward again from the cache |
| `vc3d-preview-parity-chart.png` | 1650x810 | 124,659 | Gate 1 chart: VC3D upload against vc_render_tifxyz, 12 views, with the 1 px control (from gate1.json) |
| `inkpreview-server-parity-chart.png` | 1650x810 | 136,722 | Gate 2 chart: preview server against infer --roi, 12 views, with both controls (from gate2.json) |
