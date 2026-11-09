# VC3D annotation tutorial and media

Start with [the four-step tutorial](tutorial.md). This private preview contains 12 actual stills and five GIFs from the combined VC3D feature build. Chase reviews and posts the final contribution.

## Validation and provenance

The examples use public PHerc0139 w035 CT and disposable practice annotations, with no prediction selected. They demonstrate the local combined implementation, not a released build. Colored marks are not scientific readings or verified ink.

All stills are byte-exact raw captures. GIFs show selected actual states held for 2.3 seconds, with captions outside the app image, proportional reduction when needed and a shared GIF palette. They do not show continuous mouse motion. No frames, brush motion or dialogs were invented, cropped, redacted or retouched. The visible benign demo path includes the username chase and no private checkout path. manifest.json binds every asset to its original capture hash.

The captured input was automated through ordinary Qt controls and viewport events. An instrumented companion supplied a synthetic User-role history event to verify refusal of agent Undo; no physical person drawing is claimed. Save was invoked through the application bridge. The tutorial describes the Save control, but the captured run does not establish a recorded Save-button click.

Polygon and Fill selection was exercised, not their complete drawing workflows. Assist and Versions/import sections were opened; their algorithms, imports and version creation were not exercised. Keyboard shortcuts and Pin open were not tested. Ordinary panel reopening was exercised. The opacity sequence initially observed 60%, then set 37% and 100%; it does not return to the initial value.

Visibility and opacity affect display, not saved annotation values. Save, same-process reopen and production cold restart matched the independent expected raster across all 30,496,800 annotation pixels. The three saved TIFF hashes matched. The screenshots illustrate framing and state; they are not claimed to be pixel-identical CT images. Background CT texture can refresh between captures.

| Combined-build gate | Actual result |
| --- | --- |
| Qt tests | 173 passed, zero failed or skipped |
| Plain bridge contract | 181 checks passed |
| Plugin and surface-panel checks | Passed |
| MCP tests | 199 passed, zero failures, errors or runtime skips |
| Full real-app bridge smoke | 83 checks passed, zero exclusions; 9.664 seconds total |
| Native annotation controls | All 17 exercised |
| Generated coordinate oracle | Exact 5-pixel click and 64-pixel drag, all 2,400 pixels checked |
| Public annotation persistence | All 30,496,800 pixels matched across save/reopen/cold restart |
| Independent visual review | All nine criteria passed across 107 captures and 40 review sheets |

MCP coverage used its frozen portable selection: the Windows-only named-pipe integration and opt-in MCP real-binary probe were excluded prospectively. Five portable Windows mocks remained included. The complete real-app bridge smoke ran separately with its original generated volume fixtures, all 21 check functions and exact 140-method description snapshot. Runtime inventory was 140 RPC methods and 141 MCP tools. The public native demo is separately scoped from those generated fixtures.

Source: codex/vc3d-combined-annotation at `43a64fd21fefa0a431f67dde5fcb0c45fd3881f7`, based on upstream `2144fdd3f32133136159738a9a4d1c843374648a`. Production executable SHA256: `c67cd62d7b9f60a5a0280e42dee2ea7ed7327487e221f156f0e7fa36eba15743`. The User-role companion was instrumented; cold restart used production. A reused startup version string is stale; these source and binary pins identify the validated build. All recorded gates ran locally for $0; no GPU was used.

The initial private Undo QA thumbnail prompted a clipping concern. Full raw and directly decoded GIF frames preserve Class, Tool and History labels. All three GIF frames contain the complete 822×1036 source at (101,86), with zero pixel differences after applying the same palette. The actual GIF is retained unchanged. A narrow section-chevron edge and the lower scroll-viewport edge remain exactly as captured.

## Data credit

Vesuvius Challenge, PHerc0139, segment `20260317000000-w035_2026031718`, volume `20250728140407`, 9.362 um voxels, CC BY-NC 4.0. Annotations are disposable practice marks.

## Local release status

The combined source, validation and media are ready privately. The new combined commit and branch have not been uploaded to Chase's public fork in this work. No live public compare link for this tested head is supplied. Public transfer and Chase's Villa post remain separate actions requiring the authorized publication step.
