# Blazing Storm — Blender Live Link (experimental v0.1)

Local-only pose preview bridge. Intended for **Blender 5.2 + Avastar** first;
a separate Blender 3.6 installable add-on is built from the same Python source.

## Installation

1. Check out the `feature/blender-live-link` branch of `Red2Blaze/Blazing-Storm` and rebuild the viewer with its existing Windows build setup.
2. Open the rebuilt viewer, log in, then enable `BSBlenderLiveLinkEnabled` in **Advanced > Show Debug Settings** / Debug Settings search. It's **off by default**.
3. In Blender 5.2, use `Edit > Preferences > Get Extensions/Add-ons > Install from Disk` (the exact label depends on Blender install) and select `blazing_live_link_blender_5_2.zip`; enable the add-on.
4. Open an Avastar character with a deform rig. In the 3D View's `N` sidebar, go to **Blazing > Blazing Storm Live Link**, select the **armature** (the object that contains `mPelvis`, `mHead`, etc.), and press **Start Live Link**.
5. Check for **Viewer connected**. If it stays at **Waiting for viewer**, check whether the viewer was rebuilt with the patch, the Debug Setting is enabled, and UDP port `127.0.0.1:27779` is available.
6. Scrub or play Blender's timeline. The viewer draws pose updates on **your own avatar only**. Press Stop Live Link, uncheck the viewer setting, or close Blender to release the pose (the receiver also times out after ~1.5 s).

The 3.6 add-on zip is `blazing_live_link_blender_3_6.zip`. It uses the same wire protocol and receiver, so a different viewer build is not needed.

Both installers can be generated from one source tree with:

```bash
python tools/blender-live-link/package_addons.py
```

This creates `tools/blender-live-link/dist/blazing_live_link_blender_5_2.zip` and `blazing_live_link_blender_3_6.zip`. The script adjusts the add-on's minimum Blender version for each build; the viewer protocol remains identical. These are classic Python add-on ZIPs, not new-style Blender Extensions manifests.

### Development mode and limitations

- **Experimental / not visually validated.** The Blender add-on samples evaluated Avastar `m*` deform joints, including IK constraint results, and computes rotations relative to their bind/rest transforms. Bone-axis mapping from Blender's rig to the Second Life avatar skeleton **must be checked in a running viewer**. An X/Y/Z movement may need calibration before the feature is usable for production.
- The viewer applies rotations directly to local joints *after the usual `updateCharacter()` call*. It is a local rendering override, not a native animation asset or a new motion controller. Existing animation and AO blending, attachment placement, rigged mesh skinning, and collision may need integration adjustments after a live test.
- `mPelvis` translation is disabled by default because coordinate/bind calibration is needed. Leave it off initially.
- One incoming Blender session drives the viewer at a time; a new valid sender takes over.
- The initial viewer opt-in is a Debug Setting, not a polished floater. There is no in-viewer recording/export, nor bidirectional timeline control. Blender is authoritative.
- No Blender, Avastar, or full viewer runtime is installed in the packaging environment. Only protocol unit tests and Python static compilation have been run. **Viewer build and actual pose behavior remain unverified.**

### Safety

- UDP receiver binds to `127.0.0.1` only (not to external network interfaces), using port 27779. Messages are capped at 32 KB; joint count and finite rotation values are validated.
- The viewer does not transmit this animation to the SL servers. It modifies local avatar joint transforms for rendering only.
- The feature is opt-in, defaults to off, and releases on Stop, timeout, or disabling the setting.
- **Local processes may still send datagrams to this port** when enabled. Do not enable it on an untrusted computer. A future version can implement an explicit pairing token and a proper UI.

### Build this feature branch

```bash
git fetch origin feature/blender-live-link
git switch feature/blender-live-link
```

Run the viewer's normal Windows CMake/autobuild configuration and build. A full viewer build and actual Blender/Avastar integration are not yet validated.

### Protocol (v1)

Viewer receives UDP JSON at `127.0.0.1:27779`:

```json
{"protocol":"bs-blender-live-link","version":1,"type":"pose","session":"00000000000000000000000000000000","seq":1,"frame":1,"fps":30.0,"joints":{"mHead":[1.0,0.0,0.0,0.0]}}
```

Quaternions use `[w,x,y,z]`; viewer converts to `LLQuaternion(x,y,z,w)`. The sender is responsible for translating Avastar's evaluated rest-relative bone rotations into the viewer skeleton's expected local joint space. Optional `pelvis` is `[x,y,z]`. A `stop` packet releases the current session. The viewer acknowledges valid poses to the sending UDP port; the add-on shows **Viewer connected** only when it receives such acknowledgement.

## Validation checklist

- Build patched viewer with MSVC and open a test avatar.
- Enable debug setting, connect Blender 5.2, verify ACK.
- Test neutral T-pose: confirm no visible bone movement.
- Rotate left wrist 45 degrees: confirm left, not right; direction and magnitude.
- Bend right elbow then use Avastar IK target; confirm constraints are evaluated.
- Test hand and face/Bento joints and all axis rotations.
- Scrub frame-by-frame and play for 60 seconds; check jitter, memory use and frame timing.
- Stop link while AO is enabled, then teleport and relog; ensure normal pose is restored and no simulator messages were emitted.
- Repeat basic pose/connection tests in Blender 3.6.

## Repository workflow

This feature is developed on `feature/blender-live-link` using the `Red2Blaze` GitHub account. Org Organizer account separation does not apply to this viewer project.
