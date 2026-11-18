"""Winding constraints for fit_spiral.py from m7 z slices, with no hand labels.

Modules (each runs as ``python -m m7_winding_constraints.<module>`` from
spiral-fitting/):

- ``outward_sense``: infer ``spiral_outward_sense`` (CW or ACW) from the
  surface prediction, or abstain
- ``label_slices``: give every sheet piece of a z slice a winding number in
  one solve
- ``ladders``: keep the surest labels and write them as relative winding
  point collections, with a plus-one control

See README.md in this folder.
"""
