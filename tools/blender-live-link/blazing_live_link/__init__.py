# $LicenseInfo:firstyear=2026&license=viewerlgpl$
# Copyright (C) 2026 Blazing Storm contributors
# $/LicenseInfo$

"""Blazing Storm Live Link: selective Avastar pose preview and local Poser capture."""
bl_info = {
    "name": "Blazing Storm - Blender Live Link",
    "author": "Blazing Storm contributors",
    "version": (0, 2, 0),
    "blender": (5, 2, 0),
    "location": "View3D > Sidebar > Blazing",
    "description": "Preview selected/keyed Avastar deform bones and capture the viewer pose",
    "category": "Animation",
}

import math
import re
import socket
import time
import uuid

import bpy
from bpy.props import BoolProperty, EnumProperty, IntProperty, PointerProperty
from bpy.types import Operator, Panel, PropertyGroup
from mathutils import Quaternion, Vector

from .protocol import PORT, build_packet, parse_ack, parse_snapshot

_client = None
_capture = None
_capture_status = ""


def is_armature(_self, obj):
    return obj is not None and obj.type == "ARMATURE"


class BSLLSettings(PropertyGroup):
    armature: PointerProperty(name="Avatar Rig", type=bpy.types.Object, poll=is_armature)
    rate: IntProperty(name="Update rate", default=24, min=5, max=60,
                      description="Maximum pose samples per second")
    mode: EnumProperty(
        name="Bones to preview",
        items=(
            ("KEYED_SELECTED", "Keyframed + selected",
             "Preview keyframed deform bones and currently selected pose bones (including IK-driven deform changes)"),
            ("KEYED", "Keyframed only",
             "Preview keyed deform bones; IK controls must be baked for reliable uploads"),
            ("CHANGED", "All moved bones",
             "Preview all deform bones whose evaluated pose differs from rest, including unkeyed poses"),
            ("ALL", "Entire rig",
             "Override all deform bones, including those without keyframes"),
        ),
        default="KEYED_SELECTED")
    axes: EnumProperty(
        name="Blender to SL axes",
        items=(
            ("AVASTAR", "Avastar (-Y to SL +X)",
             "Rotate Blender's -Y forward into SL's +X forward (+90 degrees Z)"),
            ("NONE", "No conversion", "Use armature-space X/Y/Z without an extra yaw rotation"),
            ("NEG90", "Rotate -90 Z", "Alternate rig orientation"),
            ("FLIP", "Rotate 180 Z", "Reverse armature orientation"),
        ),
        default="AVASTAR")
    positions: BoolProperty(
        name="Transfer keyed/moved positions", default=True,
        description="Also send bone translations relative to their rest pose")
    pelvis: BoolProperty(
        name="Allow pelvis translation", default=False,
        description="Pelvis translation is experimental and disabled by default")
    capture_selected_only: BoolProperty(
        name="Capture selected bones only", default=True,
        description="Import keyframes from the viewer only on selected deform bones")
    capture_positions: BoolProperty(
        name="Capture Poser translations (experimental)", default=False,
        description="Import position offsets as well as rotations; avatar appearance adjustments can influence these")


def choose_rig(scene):
    settings = scene.bs_blender_live_link
    if settings.armature and settings.armature.type == "ARMATURE":
        return settings.armature
    obj = bpy.context.object
    return obj if obj and obj.type == "ARMATURE" else None


def _axis_rotation(settings):
    angle = {"AVASTAR": math.pi / 2, "NONE": 0.0,
             "NEG90": -math.pi / 2, "FLIP": math.pi}[settings.axes]
    return Quaternion((0.0, 0.0, 1.0), angle)


def _local_rest(pose_bone):
    rest = pose_bone.bone.matrix_local
    return (pose_bone.parent.bone.matrix_local.inverted_safe() @ rest
            if pose_bone.parent else rest.copy())


def _fcurves_for_action(animation_data):
    """Support legacy actions and the Blender 4.4+/5.x slotted action API."""
    if not animation_data or not animation_data.action:
        return ()
    action = animation_data.action
    try:
        curves = action.fcurves
        if curves is not None:
            return curves
    except AttributeError:
        pass
    slot = getattr(animation_data, "action_slot", None)
    if slot is None:
        return ()
    found = []
    for layer in getattr(action, "layers", ()):
        for strip in getattr(layer, "strips", ()):
            try:
                bag = strip.channelbag(slot)
                if bag is not None:
                    found.extend(bag.fcurves)
            except (AttributeError, RuntimeError, TypeError):
                continue
    return found


def _keyed_bones(armature):
    names = set()
    for curve in _fcurves_for_action(armature.animation_data):
        if not curve.keyframe_points:
            continue
        match = re.match(r'pose\.bones\["([^"]+)"\]\.', curve.data_path)
        if match:
            names.add(match.group(1))
    return names


def evaluate_pose(armature, settings):
    """Return SL parent-space deltas for the chosen evaluated deform bones.

    Bone rotation_quaternion is in a bone's own rest basis, while LLJoint
    expects a local rotation in the avatar parent frame. The local rest matrix
    removes Blender bone roll; the axis rotation maps Blender armature axes to SL.
    """
    depsgraph = bpy.context.evaluated_depsgraph_get()
    evaluated = armature.evaluated_get(depsgraph)
    axis = _axis_rotation(settings)
    inverse_axis = axis.inverted()
    keyed = _keyed_bones(armature)
    selected = {bone.name for bone in armature.pose.bones if bone.bone.select}
    deform_names = {bone.name for bone in armature.pose.bones
                    if bone.name.startswith("m") and bone.bone.use_deform}
    selected_controls = bool((keyed | selected) - deform_names)
    rotations, positions = {}, {}

    for bone in evaluated.pose.bones:
        if bone.name not in deform_names:
            continue
        rest_local = _local_rest(bone)
        pose_local = (bone.parent.matrix.inverted_safe() @ bone.matrix
                      if bone.parent else bone.matrix)
        # This is a delta expressed in the PARENT's armature coordinate frame,
        # not rest-local bone axes (rest^-1 @ pose was the original bug).
        delta = pose_local @ rest_local.inverted_safe()
        rotation = delta.to_quaternion().normalized()
        offset = pose_local.translation - rest_local.translation
        moved = abs(rotation.angle) > 0.0001 or offset.length > 0.0001

        if settings.mode == "ALL":
            included = True
        elif settings.mode == "CHANGED":
            included = moved
        else:
            included = bone.name in keyed
            if settings.mode == "KEYED_SELECTED":
                included = included or bone.name in selected
                # Control/IK joints may not be deform bones. Preview their
                # evaluated deform results; bake them before uploading to SL.
                included = included or (selected_controls and moved)
        if not included:
            continue

        sl_rotation = axis @ rotation @ inverse_axis
        rotations[bone.name] = [round(float(v), 6) for v in sl_rotation]
        if settings.positions and (bone.name != "mPelvis" or settings.pelvis):
            if offset.length > 0.00001:
                sl_offset = axis @ offset
                positions[bone.name] = [round(float(v), 6) for v in sl_offset]
    return rotations, positions


def _redraw():
    screen = bpy.context.screen
    if screen is not None:
        for area in screen.areas:
            if area.type == "VIEW_3D":
                area.tag_redraw()


class Client:
    def __init__(self):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.setblocking(False)
        self.session = uuid.uuid4().hex
        self.sequence = 0
        self.last_ack = 0.0
        self.last_sent = 0.0
        self.last_content = None
        self.error = ""
        self.running = True

    def send(self, wire):
        try:
            self.socket.sendto(wire, ("127.0.0.1", PORT))
            self.error = ""
            self.last_sent = time.monotonic()
        except OSError as ex:
            self.error = str(ex)

    def poll_ack(self):
        for _ in range(8):
            try:
                data, address = self.socket.recvfrom(1024)
            except (BlockingIOError, InterruptedError):
                break
            except OSError as ex:
                self.error = str(ex)
                break
            if address[0] == "127.0.0.1" and parse_ack(data, self.session):
                self.last_ack = time.monotonic()

    def close(self):
        self.sequence += 1
        try:
            self.send(build_packet("stop", self.session, self.sequence))
        finally:
            self.running = False
            self.socket.close()


def timer_tick():
    client = _client
    if client is None or not client.running:
        return None
    client.poll_ack()
    try:
        scene = bpy.context.scene
        if scene is not None and hasattr(scene, "bs_blender_live_link"):
            settings = scene.bs_blender_live_link
            rig = choose_rig(scene)
            if rig:
                joints, positions = evaluate_pose(rig, settings)
                content = (scene.frame_current,
                           tuple(sorted((name, tuple(q)) for name, q in joints.items())),
                           tuple(sorted((name, tuple(p)) for name, p in positions.items())))
                now = time.monotonic()
                if content != client.last_content or now - client.last_sent > 0.75:
                    client.sequence += 1
                    wire = build_packet("pose", client.session, client.sequence,
                                        scene.frame_current,
                                        scene.render.fps / (scene.render.fps_base or 1),
                                        joints, positions=positions)
                    client.send(wire)
                    client.last_content = content
            else:
                client.error = "Select an Avastar armature"
            _redraw()
            return 1.0 / settings.rate
    except (ValueError, RuntimeError, OSError, AttributeError, TypeError, KeyError) as ex:
        client.error = str(ex)
    _redraw()
    return 0.1


def _apply_snapshot(scene, rig, snapshot):
    settings = scene.bs_blender_live_link
    axis = _axis_rotation(settings)
    inverse_axis = axis.inverted()
    selected = {pb.name for pb in rig.pose.bones if pb.bone.select}
    count = 0
    for name, qvalues in snapshot["joints"].items():
        pb = rig.pose.bones.get(name)
        if pb is None or not pb.bone.use_deform:
            continue
        if settings.capture_selected_only and name not in selected:
            continue
        rest_rotation = _local_rest(pb).to_quaternion().normalized()
        sl_rotation = Quaternion(tuple(qvalues)).normalized()
        parent_rotation = inverse_axis @ sl_rotation @ axis
        pb.rotation_mode = "QUATERNION"
        pb.rotation_quaternion = (rest_rotation.inverted()
                                  @ parent_rotation @ rest_rotation).normalized()
        pb.keyframe_insert(data_path="rotation_quaternion",
                           frame=scene.frame_current, group=name)
        if name in snapshot["positions"] and settings.capture_positions:
            sl_offset = Vector(snapshot["positions"][name])
            pb.location = rest_rotation.inverted() @ (inverse_axis @ sl_offset)
            pb.keyframe_insert(data_path="location",
                               frame=scene.frame_current, group=name)
        count += 1
    rig.update_tag()
    bpy.context.view_layer.update()
    return count


class Capture:
    def __init__(self):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.setblocking(False)
        self.session = uuid.uuid4().hex
        self.started = time.monotonic()
        self.socket.sendto(build_packet("snapshot_request", self.session, 1),
                           ("127.0.0.1", PORT))

    def close(self):
        self.socket.close()


def capture_tick():
    global _capture, _capture_status
    capture = _capture
    if capture is None:
        return None
    try:
        for _ in range(8):
            try:
                wire, address = capture.socket.recvfrom(32769)
            except (BlockingIOError, InterruptedError):
                break
            if address[0] != "127.0.0.1":
                continue
            snapshot = parse_snapshot(wire, capture.session)
            if snapshot is None:
                continue
            scene = bpy.context.scene
            rig = choose_rig(scene)
            count = _apply_snapshot(scene, rig, snapshot) if rig else 0
            _capture_status = f"Captured {count} deform bones at frame {scene.frame_current}"
            _capture.close()
            _capture = None
            _redraw()
            return None
        if time.monotonic() - capture.started > 3.0:
            _capture_status = "No viewer response (stop preview and enable Live Link)"
            capture.close()
            _capture = None
            _redraw()
            return None
    except (ValueError, RuntimeError, OSError, AttributeError, TypeError, KeyError) as ex:
        _capture_status = str(ex)
        capture.close()
        _capture = None
        _redraw()
        return None
    return 0.05


class BSLL_OT_start(Operator):
    bl_idname = "bsll.start"
    bl_label = "Start Live Link"
    bl_description = "Send the selected or keyed deform-bone poses to the local viewer"

    def execute(self, context):
        global _client
        if _client is not None:
            return {"FINISHED"}
        if choose_rig(context.scene) is None:
            self.report({"ERROR"}, "Select the Avastar armature first")
            return {"CANCELLED"}
        _client = Client()
        if not bpy.app.timers.is_registered(timer_tick):
            bpy.app.timers.register(timer_tick, first_interval=0.01)
        self.report({"INFO"}, "Previewing on local viewer at 127.0.0.1")
        return {"FINISHED"}


class BSLL_OT_stop(Operator):
    bl_idname = "bsll.stop"
    bl_label = "Stop Live Link"

    def execute(self, _context):
        global _client
        if _client is not None:
            _client.close()
            _client = None
        return {"FINISHED"}


class BSLL_OT_capture(Operator):
    bl_idname = "bsll.capture_viewer_pose"
    bl_label = "Capture Viewer Pose to Keyframes"
    bl_description = "Import the avatar's current local viewer/Poser pose into the current Blender frame"

    def execute(self, context):
        global _capture, _capture_status
        if _client is not None:
            self.report({"ERROR"}, "Stop Live Link first to avoid capturing its own preview")
            return {"CANCELLED"}
        if choose_rig(context.scene) is None:
            self.report({"ERROR"}, "Select the Avastar armature first")
            return {"CANCELLED"}
        if _capture is not None:
            return {"FINISHED"}
        try:
            _capture = Capture()
            _capture_status = "Requesting viewer pose..."
        except OSError as ex:
            _capture_status = str(ex)
            self.report({"ERROR"}, _capture_status)
            return {"CANCELLED"}
        bpy.app.timers.register(capture_tick, first_interval=0.05)
        return {"FINISHED"}


class BSLL_PT_panel(Panel):
    bl_label = "Blazing Storm Live Link"
    bl_idname = "BSLL_PT_panel"
    bl_space_type = "VIEW_3D"
    bl_region_type = "UI"
    bl_category = "Blazing"

    def draw(self, context):
        layout = self.layout
        settings = context.scene.bs_blender_live_link
        layout.prop(settings, "armature")
        layout.prop(settings, "mode")
        layout.prop(settings, "axes")
        layout.prop(settings, "rate")
        layout.prop(settings, "positions")
        row = layout.row()
        row.enabled = settings.positions
        row.prop(settings, "pelvis")
        if _client:
            layout.operator("bsll.stop", icon="CANCEL")
            if _client.last_ack and time.monotonic() - _client.last_ack < 3:
                layout.label(text="Viewer connected", icon="CHECKMARK")
            else:
                layout.label(text="Waiting for viewer", icon="INFO")
            if _client.error:
                layout.label(text=_client.error[:95], icon="ERROR")
        else:
            layout.operator("bsll.start", icon="PLAY")
        layout.separator()
        layout.label(text="Firestorm Poser -> Blender")
        layout.prop(settings, "capture_selected_only")
        layout.prop(settings, "capture_positions")
        row = layout.row()
        row.enabled = _client is None and _capture is None
        row.operator("bsll.capture_viewer_pose", icon="REC")
        if _capture_status:
            layout.label(text=_capture_status[:95], icon="INFO")
        layout.label(text="Local-only preview; no SL upload")


_classes = (BSLLSettings, BSLL_OT_start, BSLL_OT_stop,
            BSLL_OT_capture, BSLL_PT_panel)


def register():
    for cls in _classes:
        bpy.utils.register_class(cls)
    bpy.types.Scene.bs_blender_live_link = PointerProperty(type=BSLLSettings)


def unregister():
    global _client, _capture
    if _client is not None:
        _client.close()
        _client = None
    if _capture is not None:
        _capture.close()
        _capture = None
    if bpy.app.timers.is_registered(timer_tick):
        bpy.app.timers.unregister(timer_tick)
    if bpy.app.timers.is_registered(capture_tick):
        bpy.app.timers.unregister(capture_tick)
    del bpy.types.Scene.bs_blender_live_link
    for cls in reversed(_classes):
        bpy.utils.unregister_class(cls)
