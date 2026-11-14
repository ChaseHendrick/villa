"""Draws the PHerc0139 organizers' umbilicus as fetched by vesuvius.data.umbilicus (annotation geometry, no CT)."""
import json
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

line_path, out = Path(sys.argv[1]), Path(sys.argv[2])
points = json.loads(line_path.read_text())["control_points"]
z = [p["z"] for p in points]
x = [p["x"] for p in points]
y = [p["y"] for p in points]

ink, muted, grid, blue, surface = "#0b0b0b", "#52514e", "#e4e3df", "#2a78d6", "#fcfcfb"
plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 13, "text.color": ink,
                     "axes.labelcolor": muted, "xtick.color": muted, "ytick.color": muted})
fig, axes = plt.subplots(1, 2, figsize=(16, 9), dpi=100, sharey=True, facecolor=surface)
for ax, values, name in ((axes[0], x, "x"), (axes[1], y, "y")):
    ax.set_facecolor(surface)
    ax.plot(values, z, color=blue, lw=2, solid_capstyle="round")
    ax.plot(values, z, "o", ms=3.5, color=blue, mec=surface, mew=0.8)
    ax.set_xlabel(f"{name} (level-0 voxels)")
    ax.grid(True, color=grid, lw=1)
    for side in ("top", "right"):
        ax.spines[side].set_visible(False)
    for side in ("left", "bottom"):
        ax.spines[side].set_color(grid)
    ax.set_title(f"{name} against slice", loc="left", fontsize=14, color=ink)
axes[0].set_ylabel("z (level-0 slice)")
axes[0].invert_yaxis()
fig.suptitle("PHerc0139 umbilicus, fetched with fetch_umbilicus(\"PHerc0139\"): "
             f"{len(points)} control points, z {int(min(z))} to {int(max(z))}",
             x=0.06, ha="left", fontsize=17, color=ink)
fig.text(0.06, 0.015, "Organizers' line from the open-data bucket (annotator David Josey), "
         "volume 20260102150214, 2.399 um voxels. Annotation geometry only, no CT. "
         "Vesuvius Challenge open data, CC BY-NC 4.0", fontsize=11, color=muted, ha="left")
fig.subplots_adjust(left=0.08, right=0.98, top=0.88, bottom=0.12, wspace=0.08)
fig.savefig(out, facecolor=surface)
