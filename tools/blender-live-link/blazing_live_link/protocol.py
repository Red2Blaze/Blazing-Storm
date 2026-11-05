# $LicenseInfo:firstyear=2026&license=viewerlgpl$
# Copyright (C) 2026 Blazing Storm contributors
# $/LicenseInfo$

"""Blazing Storm Blender Live Link v1 wire protocol.

Backwards-compatible v1 extensions: optional per-bone positions and explicit,
one-shot viewer pose snapshots. No bpy dependency. All traffic stays on loopback.
"""
import json
import math

PROTOCOL = "bs-blender-live-link"
VERSION = 1
PORT = 27779
MAX_BYTES = 32768
MAX_JOINTS = 256


def _valid_session(session):
    return (isinstance(session, str) and len(session) == 32
            and all(c in "0123456789abcdef" for c in session))


def _valid_bones(joints, size):
    return (isinstance(joints, dict) and len(joints) <= MAX_JOINTS
            and all(isinstance(name, str) and 2 <= len(name) <= 80
                    and name.startswith("m") and isinstance(values, (list, tuple))
                    and len(values) == size
                    and all(isinstance(v, (int, float)) and math.isfinite(v) for v in values)
                    for name, values in joints.items()))


def build_packet(kind, session, sequence, frame=0, fps=30.0, joints=None, pelvis=None,
                 positions=None):
    if kind not in ("pose", "stop", "snapshot_request"):
        raise ValueError("invalid packet type")
    if not _valid_session(session):
        raise ValueError("invalid session")
    packet = {
        "protocol": PROTOCOL, "version": VERSION, "type": kind,
        "session": session, "seq": int(sequence),
    }
    if kind == "pose":
        if not _valid_bones(joints, 4):
            raise ValueError("invalid joints")
        packet.update(frame=int(frame), fps=float(fps), joints=joints)
        if pelvis is not None:
            if not isinstance(pelvis, (tuple, list)) or len(pelvis) != 3 or not all(
                    isinstance(v, (int, float)) and math.isfinite(v) for v in pelvis):
                raise ValueError("invalid pelvis")
            packet["pelvis"] = pelvis
        if positions is not None:
            if not _valid_bones(positions, 3):
                raise ValueError("invalid positions")
            packet["positions"] = positions
    wire = json.dumps(packet, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode("ascii")
    if len(wire) > MAX_BYTES:
        raise ValueError("packet too large")
    return wire


def parse_ack(data, session):
    try:
        value = json.loads(data)
        return (value.get("protocol") == PROTOCOL and value.get("version") == VERSION
                and value.get("type") == "ack" and value.get("session") == session)
    except (ValueError, UnicodeError, TypeError, AttributeError):
        return False


def parse_snapshot(data, session):
    """Return a validated one-shot viewer pose, or None for unrelated packets."""
    if not isinstance(data, (bytes, bytearray)) or len(data) > MAX_BYTES:
        return None
    try:
        packet = json.loads(data)
        if not isinstance(packet, dict):
            return None
        if (packet.get("protocol") != PROTOCOL or packet.get("version") != VERSION
                or packet.get("type") != "pose_snapshot" or packet.get("session") != session):
            return None
        joints = packet.get("joints")
        positions = packet.get("positions", {})
        if not _valid_bones(joints, 4) or not _valid_bones(positions, 3):
            return None
        return {"joints": joints, "positions": positions}
    except (ValueError, UnicodeError, TypeError, AttributeError):
        return None
