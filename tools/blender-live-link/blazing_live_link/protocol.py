# $LicenseInfo:firstyear=2026&license=viewerlgpl$
# Copyright (C) 2026 Blazing Storm contributors
# $/LicenseInfo$

"""Blazing Storm Blender Live Link v1 wire format. No bpy dependency.

Only transforms on the local computer are sent. Nothing here interacts with SL.
"""
import json
import math

PROTOCOL = "bs-blender-live-link"
VERSION = 1
PORT = 27779
MAX_BYTES = 32768
MAX_JOINTS = 256


def build_packet(kind, session, sequence, frame=0, fps=30.0, joints=None, pelvis=None):
    if kind not in ("pose", "stop"):
        raise ValueError("invalid packet type")
    if len(session) != 32 or any(c not in "0123456789abcdef" for c in session):
        raise ValueError("invalid session")
    packet = {
        "protocol": PROTOCOL, "version": VERSION, "type": kind,
        "session": session, "seq": int(sequence),
    }
    if kind == "pose":
        if joints is None or len(joints) > MAX_JOINTS:
            raise ValueError("invalid joints")
        packet.update(frame=int(frame), fps=float(fps), joints=joints)
        if pelvis is not None:
            packet["pelvis"] = pelvis
    wire = json.dumps(packet, separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode("ascii")
    if len(wire) > MAX_BYTES:
        raise ValueError("pose packet too large")
    return wire


def parse_ack(data, session):
    try:
        value = json.loads(data)
        return (value.get("protocol") == PROTOCOL and value.get("version") == VERSION
                and value.get("type") == "ack" and value.get("session") == session)
    except (ValueError, UnicodeError, TypeError, AttributeError):
        return False
