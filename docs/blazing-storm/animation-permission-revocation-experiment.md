# Simulator-side animation permission revocation experiment

> Experimental branch: `experiment/animation-permission-revocation`.
> Derived from `feature/detachable-floaters` at `de35e67e5ba8aa749157233d5add9f474d159930`.
> No claim of a working security fix until a simulator test confirms it.

## Purpose

Investigate how a resident can revoke **their own** script-granted animation permissions when a script is inside an object worn by a different resident, even after the original sit-granting interaction. Existing viewers can stop an active animation without permanently revoking that script's authorization.

The viewer already implements `LLAgent::sendRevokePermissions(target, permissions)` in `indra/newview/llagent.cpp`, emitting the existing `RevokePermissions` UDP message with the authenticated agent/session IDs, target object or region ID, and permission mask.

According to current viewer source comments, the simulator supports only:

- `PERMISSION_TRIGGER_ANIMATION` = `0x10`
- `PERMISSION_OVERRIDE_ANIMATIONS` = `0x8000`

The test mask is `0x8010`. Do not add other permission bits.

## New experimental viewer controls

Open **Avatar > Avatar Health > Animation Permission Tests (Experimental)**.

- **Request Revoke: Current Region**: sends `RevokePermissions` with the current region ID as `ObjectID` and mask `0x8010`, *without* stopping animations first. It is separate from the pre-existing Stop Animations & Revoke action and guarded by a confirmation dialog.
- **Request Revoke: Object UUID...**: asks for a UUID, validates it and sends the same message with that object UUID as `ObjectID`. This allows examination of the simulator's handling of targeted objects, including attachments worn by other residents, if the UUID is known. The target must be in the region; the viewer cannot establish server-side residency from a UUID alone.

Both actions check that the resident has not changed regions between opening the dialog and submitting it. They show the request target, and log under `BlazingAnimRevoke`. A "sent" notice means only that the viewer **attempted** the request; **no success/failure acknowledgment** is provided by the protocol. The viewer does **not** purport to revoke permissions that only the simulator can own.

The original Stop Animations menu items remain unchanged.

## Controlled, consent-based test matrix

Use an isolated test region and two accounts you control or have explicit permission to test with. Have the scripted test object owner display the result of `llGetPermissions()` and `llGetPermissionsKey()` before and after each test. Also observe whether `llStartAnimation` succeeds when intentionally re-triggered with the subject's consent. Use benign animations only.

| ID | Scenario | Baseline / action | Evidence to record |
| --- | --- | --- | --- |
| A | Rezzed object in the same region has animation permission from a consenting avatar | Baseline `Stop Avatar Animations & Revoke Permissions`, then fresh grant and new Region request | Before/after permission mask, `run_time_permissions` behavior, whether intentional replay succeeds |
| B | Same rezzed object, known root object UUID | Targeted Object UUID request | Object UUID, before/after permission mask, replay result |
| C | Other account wears an attachment whose script previously received the consenting subject's animation permission | Region request **without** active stop | Whether the script actually loses `0x10` or `0x8000`; compare with A |
| D | Same consenting test attachment with a known in-region object UUID | Object UUID request | Whether targeted handling differs from region-wide handling |
| E | Consenting avatar teleports away, returns to the region, and tries again | Baseline and both requests after return | Whether permissions persist over region exit/return; ensure the target still exists |
| F | Consenting avatar relogs and returns while the other account still wears the attachment | Baseline and both requests after relog | Whether persistent permissions can replay after relog |
| G | Ordinary animations, AO, and scripted seating | Use both test actions in benign circumstances | Regression effects, especially auto-granted seated permissions |

For each run capture viewer build commit, simulator channel/version, region, UTC time, test subject and target UUIDs **privately**, scenario ID, requested scope, logs, observed script permission bits, and observed replay outcome. Do not publish avatar/object identifiers in public issue reports without consent.

### How to interpret outcomes

- **Script cannot replay and loses permissions**: evidence of server-side revocation in that specific scenario, not proof that all attachment cases are covered.
- **Script can replay**: revocation failed or was not applied; the action may have stopped only the current animation or affected a different object.
- **Only animation stops, permission unchanged**: NOT a successful fix.
- **No response from simulator**: expected; the current protocol does not confirm a revoke. A log entry is not success evidence.
- **Different results for region vs object targeting**: document precisely; do not assume a message can cross regions.

## Likely protocol limitation

The official Second Life script permission documentation says the Stop Animating Me option releases trigger/override permissions when the script is in the same region and **is not attached to the permission-granting avatar**. The published Firestorm/official viewer bug report confirms that attacker-worn attachments can restart animations even after Stop Animations. So a successful fully general fix may require **Linden Lab simulator changes** (e.g., revocation of third-party attachment grants or a new simulator capability), rather than any alternative viewer packet.

Do not try to forge another user's agent/session IDs or rely on undocumented server-side handling. Test only revocation requests for the logged-in avatar.

## Build and validation

1. Check out `experiment/animation-permission-revocation` and run your normal Windows ReleaseFS_open build, or use the branch-specific **Build Animation Permission Revoke Experiment** GitHub Actions workflow (`.github/workflows/animation-revocation-experiment.yml`). Its output is a **Windows x64 experimental artifact**, with a SHA256 file, retained for 14 days. This workflow does not publish or replace the production `latest-test` release.
2. Verify `llviewermenu.cpp` compiles and both menu entries open the intended dialogs.
3. Verify Cancel, malformed UUID, and region-change paths do not dispatch a request.
4. Confirm a valid Region request uses the current region UUID, and an Object request uses precisely the entered UUID.
5. Observe `BlazingAnimRevoke` logs and compare script-side before/after state.
6. Run the matrix above and record results before describing the feature as a protection.

### References

- https://create.secondlife.com/script/lsl-reference/functions/llrequestpermissions/
- https://feedback.secondlife.com/server-bugs/p/avatar-stop-animations-does-not-revoke-permissions
- https://feedback.secondlife.com/feature-requests/p/revoke-animation-permission-on-teleport
