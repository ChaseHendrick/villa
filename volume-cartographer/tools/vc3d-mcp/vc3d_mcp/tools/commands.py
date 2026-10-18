"""The VC3D command catalog: menu actions, shortcuts, and status bar panels."""

from __future__ import annotations

from typing import Any

from ..core import mcp, _call


@mcp.tool()
async def vc3d_list_commands() -> dict[str, Any]:
    """List VC3D's command catalog: every menu action, application-wide
    keyboard shortcut, and status bar panel, the same list the Ctrl+K command
    palette shows a person.

    Use this to tell a person which menu entry or key does something, to look
    up the keys VC3D binds, or to read whether a toggle such as Draw BBox
    ("draw_bbox") or Sync cursor ("sync_cursor") is on. It is read-only: to
    act, call the typed tools, for example vc3d_open_volume rather than the
    "open_volpkg" menu action.

    Returns {"commands": [...]} in catalog order. Each entry has id (a keybind
    id such as "zoom_in", a menu id such as "draw_bbox", or "panel.<name>"
    for a status bar panel such as "panel.volumes"), title, category (the
    menu or keybind section), shortcut (portable text such as "Ctrl+K", or
    null), checkable, checked (null unless checkable), and enabled.
    """
    return await _call("command.list")
