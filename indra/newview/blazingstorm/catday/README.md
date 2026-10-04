# International Cat Day avatar override

Branch: `feature/international-cat-day`

## Goal

On August 8, Blazing Storm renders normal avatars as cats on this viewer only.

This feature is deliberately visual-only. It must not modify the server-side
avatar, animation state, movement, identity, attachments, chat, IM, radar, or
other network-visible state.

## Rendering design

The supplied cat mesh will be rigged to the normal Second Life avatar
skeleton. The normal avatar continues to evaluate its skeleton and animations.
The Cat Day renderer will use those existing joint matrices to skin the cat
mesh instead of trying to create a separate cat animation system.

During Cat Day rendering:

1. Keep the original `LLVOAvatar` alive and updating normally.
2. Keep its skeleton and animation evaluation unchanged.
3. Suppress the normal avatar body render.
4. Suppress visible worn attachments by default so human clothing does not
   float around the replacement cat. The attachments remain logically present.
5. Render the bundled cat mesh using the avatar's current skeleton matrices.
6. Keep name tags and identity/UI behavior associated with the original avatar.

UI preview avatars and control avatars/animesh are excluded unless explicitly
added later.

## UUID-based appearance

`BSCatDay::variantForAvatar()` hashes the avatar UUID with a stable FNV-1a
hash. The same avatar therefore receives the same cat variant every time.

The variant is intentionally derived only from UUID, never from display name or
appearance, so renaming or changing outfits does not change the assigned cat.

`materialSeedForAvatar()` supplies additional stable bits for coat tint,
pattern, eye color, or other material choices once the final model/material
layout is known.

## Settings

- `BSCatDayEnabled` — master feature switch.
- `BSCatDayForceEnabled` — manual testing override for dates other than August 8.
- `BSCatDayVariantCount` — number of deterministic cat variants available.
- `BSCatDayHideAttachments` — hide worn attachment geometry while the cat override is active.

The actual draw replacement is intentionally not enabled until the rigged cat
asset is added, so this branch remains safe to build and run in the meantime.
