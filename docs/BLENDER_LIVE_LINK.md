# Blazing Storm — Blender Live Link (experimental v0.2)

Local-only pose preview bridge. Intended for **Blender 5.2 + Avastar** first;
a separate Blender 3.6 installable add-on is built from the same Python source.

## Installation

1. Check out the `feature/blender-live-link` branch of `Red2Blaze/Blazing-Storm` and rebuild the viewer with its existing Windows build setup.
2. Open the rebuilt viewer, log in, then use **World > Blazing Storm > Enable Blender Live Link**. The underlying `BSBlenderLiveLinkEnabled` Debug Setting remains available and defaults to **off**.
3. In Blender 5.2, use `Edit > Preferences > Get Extensions/Add-ons > Install from Disk` (the exact label depends on Blender install) and select `blazing_live_link_blender_5_2.zip`; enable the add-on.
4. Open an Avastar character with a deform rig. In the 3D View's `N` sidebar, go to **Blazing > Blazing Storm Live Link**, choose the **armature** (containing `mPelvis`, `mHead`, etc.). Choose **Keyframed + selected** and **Avastar (-Y to SL +X)** to start, then press **Start Live Link**.
5. Check for **Viewer connected**. If it stays at **Waiting for viewer**, check whether the viewer was rebuilt with the patch, the Debug Setting is enabled, and UDP port `127.0.0.1:27779` is available.
6. Scrub or play Blender's timeline, or edit a selected bone in Pose Mode. The viewer draws pose updates on **your own avatar only**. Press Stop Live Link, uncheck the viewer setting, or close Blender to release the pose (the receiver also times out after ~1.5 s).

The 3.6 add-on zip is `blazing_live_link_blender_3_6.zip`. It uses the same wire protocol and receiver, so a different viewer build is not needed.

Both installers can be generated from one source tree with:

```bash
python tools/blender-live-link/package_addons.py
```

This creates `tools/blender-live-link/dist/blazing_live_link_blender_5_2.zip` and `blazing_live_link_blender_3_6.zip`. The script adjusts the add-on's minimum Blender version for each build; the viewer protocol remains identical. These are classic Python add-on ZIPs, not new-style Blender Extensions manifests.

### Selective animation preview (v0.2)

- **Keyframed + selected** (default): transfer only bones with keyframes in the current action, bones selected in Blender Pose Mode, and moved deform bones affected by selected/keyframed non-deform controls. Other SL avatar joints can keep their existing AO/idle animation.
- **Keyframed only**: helpful to test the actual deform-bone tracks expected in an uploaded animation, but IK controls that are not baked into deform-bone animation will **not** export the same way.
- **All moved bones**: transfer every evaluated deform bone away from rest, including static unkeyed pose components.
- **Entire rig**: v0.1-style full skeleton override.
- **Blender to SL axes** offers Avastar forward conversion (+90 degrees about Z) and alternate yaw presets. If an arm moves along the wrong axis, switch this before rewriting animation keys. Bone rest orientation is also accounted for in v0.2.
- **Transfer keyed/moved positions** also previews non-pelvis local bone translations. Pelvis translation remains separately opt-in because it can move the avatar unexpectedly.

### Capture a Firestorm Poser pose to Blender keyframes (v0.2)

1. Enable Live Link in your viewer from **World > Blazing Storm**. Open **Firestorm Poser...** there and use Poser to move the avatar.
2. In Blender, select the corresponding Avastar armature and go to the frame on which you want the pose keyframed.
3. **Stop Live Link** in Blender; capture deliberately will not reflect Blender's own preview back at it.
4. Select the deform bones to keyframe (or uncheck **Capture selected bones only** to import all *user-modified, actively posed Firestorm Poser joints*).
5. Click **Capture Viewer Pose to Keyframes** in the Blazing panel. The viewer replies locally and Blender inserts quaternion rotation keys on that frame.
6. Optional **Capture Poser translations (experimental)** imports matching position offsets. Leave this off for the first test: avatar appearance adjustments can affect position baselines.
7. Advance Blender to another frame, change the pose in Firestorm Poser, and capture again to build an animation.

Capture is *not* a continuous bidirectional link, and it is not a viewer-side animation recorder. It samples the Firestorm Poser joints that have actually been modified in the current Poser session; other AO and walking motions are not intentionally imported. The viewer never uploads or creates SL animation inventory items. Review and bake your keys with Blender/Avastar's usual exporter before upload.

### Development mode and limitations

- **Experimental / not fully visually validated.** The Blender add-on samples evaluated Avastar `m*` deform joints, including IK constraint results, and maps rotations from the rest/parent frame into Second Life axes using a configurable preset. Specific rigs or joints may still require axis calibration; check a wrist and neck on all three axes before treating the preview as upload-accurate.
- The viewer applies transforms **inside** `updateCharacter()` after motion blending but before updating the head offset and joint world matrices. This should make regular rigged attachments and linked eyes follow more reliably. Independently animated attachments (including Animesh or scripted eye animations) are **not** guaranteed to be controlled by Live Link.
- `mPelvis` translation is disabled by default because coordinate/bind calibration is needed. Leave it off initially; other keyed/selected local joint translations are supported.
- One incoming Blender session drives the viewer at a time; a new valid sender takes over.
- The initial viewer opt-in is a Debug Setting, not a polished floater. There is no in-viewer recording/export, nor bidirectional timeline control. Blender is authoritative.
- The v0.1 preview was user-tested and found to work partially; v0.2 coordinate conversion, selected-bone behavior, attachment timing and Poser keyframe capture still require real Blender + Avastar + viewer testing.

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

Quaternions use `[w,x,y,z]`; viewer converts to `LLQuaternion(x,y,z,w)`. The sender is responsible for translating Avastar's evaluated rest-relative bone rotations into the viewer skeleton's expected local joint space. Optional `pelvis` is `[x,y,z]`. A `stop` packet releases the current session. The viewer acknowledges valid poses to the sending UDP port; the add-on shows **Viewer connected** only when it receives such acknowledgement. v0.2 adds an optional `positions` mapping of per-joint local offsets `[x,y,z]` to `pose` packets, a `snapshot_request` packet, and a viewer `pose_snapshot` reply containing actively edited Firestorm Poser joints. Version stays 1 for compatibility with older pose-only viewers, although an older viewer does not support snapshot replies.

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
