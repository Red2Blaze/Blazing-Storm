# Detachable Floaters

This branch develops native top-level windows for viewer floaters.

## Goals

- Let compatible LLFloater instances move from the main viewer window into an independent OS window.
- Allow multiple detached floaters and multi-monitor placement.
- Return a detached floater to the viewer without recreating its logical state.
- Preserve detached state, size and position where practical.
- Keep the core implementation close to upstream LLFloater/LLWindow APIs so it can later be ported to the Linden Lab viewer.
- Windows, macOS and Linux are first-class targets; platform-specific code belongs below LLWindow.

## Architecture

The feature is intentionally split into two layers.

### LLFloater-facing layer

A detachable floater remains the same LLFloater object. The host changes; application state must not be copied into a second floater. The eventual public API is expected to expose detach/attach/capability/state operations while keeping native-window details out of individual floaters.

### Native-window layer

Native top-level creation, graphics-context handling, input dispatch, DPI, focus and destruction belong in the LLWindow/LLWindowManager layer. Do not add HWND/Cocoa/X11/Wayland types to LLFloater.

A detached host owns the native window but not the floater's application lifetime. Closing the native host should normally reattach or cleanly close according to viewer policy rather than deleting an LLFloater behind LLFloaterReg.

## Implementation order

1. Prove a second LLWindow can be created and serviced without disturbing the primary viewer window.
2. Render one opt-in test floater in that host.
3. Route mouse, keyboard, Unicode input, focus, resize and DPI changes.
4. Add attach/detach commands and safe teardown.
5. Generalize capability checks for normal floaters.
6. Persist monitor/rect/detached state.
7. Test multi-floater and multi-monitor operation.
8. Handle complex cases: LLMultiFloater, drag-and-drop, modal dialogs, tooltips, media and inventory.

## Compatibility rules

Floaters that assume gViewerWindow coordinates, modal ownership, world rendering, or a specific GL context must remain attached until explicitly made compatible. Detachment must fail safely rather than partially moving a floater.

## Portability

Blazing Storm branding and possession/look-at code must not be dependencies of the core host. Firestorm-only glue should be isolated so the host can later be adapted to the upstream Second Life viewer.

## Test targets

- Windows: primary initial development target.
- Linux: X11/Wayland behavior and GL context validation.
- macOS: Cocoa window, Retina scale and context validation.

The first usable milestone is one explicitly opted-in floater that can detach, accept input, resize, and reattach without losing its state.
