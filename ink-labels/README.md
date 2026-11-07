# VC3D ink label captures

Screen captures of VC3D (branch vc3d-ink-label-painting) under Xvfb, opened on
the open-data catalog: PHerc0139, volume 20250728140407 (9.362 um), segment
20260317000000-w035_2026031718. The labels shown are the organizers' published
w035 ink labels and supervision mask (ink-labels/9.362um-volume-20250728140407/20260918,
level 0), loaded as the segment's label TIFFs, plus a few strokes and boxes drawn
for the demo. No ink prediction is selected (Detection: None), so no model output
appears in any pixel.

- `vc3d_ink_labels_w035.png`: label mode on, labels over CT, three region boxes, the dock.
- `vc3d_ink_label_regions_w035.png`: regions with notes and statuses; #2 holds its box out for validation.
- `vc3d_ink_labels_masked_edge_w035.gif` (and `.mp4`): a background and an ink stroke painted across the
  segment's masked border (hatched), then Ctrl+S. The saved TIFFs hold 0 labeled pixels on masked surface;
  the dock reports 23,072 px saved as unlabeled.

Data: Vesuvius Challenge, CC BY-NC 4.0 (https://scrollprize.org/data).
