/**
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Copyright (C) 2026 Blazing Storm contributors
 * $/LicenseInfo$
 */

/**
 * Blazing Storm local-only Blender pose preview.
 * LGPL-2.1-or-later. No outbound grid messages and no external listener.
 */
#include "llviewerprecompiledheaders.h"
#include "blazingstorm/blender/bsblenderlivelink.h"

#include "fsposeranimator.h"
#include "lljoint.h"
#include "llquaternion.h"
#include "llviewercontrol.h"
#include "llvoavatar.h"

#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace
{
using Clock = std::chrono::steady_clock;
namespace asio = boost::asio;
namespace json = boost::json;
using udp = asio::ip::udp;
constexpr unsigned short PORT = 27779;
constexpr std::size_t MAX_PACKET_BYTES = 32768;
constexpr std::size_t MAX_JOINTS = 256;
constexpr auto TIMEOUT = std::chrono::milliseconds(1500);
constexpr auto POSER_STREAM_TIMEOUT = std::chrono::milliseconds(3500);
constexpr auto POSER_STREAM_INTERVAL = std::chrono::milliseconds(50);
constexpr auto POSER_STREAM_HEARTBEAT = std::chrono::milliseconds(1000);
constexpr const char* PROTOCOL = "bs-blender-live-link";

bool extract_number(const json::value& v, float& out)
{
    double n = 0.0;
    if (v.is_double()) n = v.as_double();
    else if (v.is_int64()) n = static_cast<double>(v.as_int64());
    else if (v.is_uint64()) n = static_cast<double>(v.as_uint64());
    else return false;
    if (!std::isfinite(n) || n < -1000000.0 || n > 1000000.0) return false;
    out = static_cast<float>(n);
    return true;
}

bool sequence_from_json(const json::value& v, std::uint64_t& value)
{
    if (v.is_uint64()) { value = v.as_uint64(); return true; }
    if (v.is_int64() && v.as_int64() >= 0)
    {
        value = static_cast<std::uint64_t>(v.as_int64());
        return true;
    }
    return false;
}

bool parse_pose(const json::value& payload, const json::value* pelvis_payload,
    const json::value* positions_payload,
    std::unordered_map<std::string, LLQuaternion>& rotations,
    std::unordered_map<std::string, LLVector3>& positions,
    bool& has_pelvis, LLVector3& pelvis)
{
    auto* object = payload.if_object();
    if (!object || object->size() > MAX_JOINTS) return false;
    rotations.clear();
    for (const auto& entry : *object)
    {
        const std::string name(entry.key().data(), entry.key().size());
        if (name.size() < 2 || name.size() > 80 || name[0] != 'm') return false;
        const auto* q = entry.value().if_array();
        if (!q || q->size() != 4) return false;
        float values[4];
        for (std::size_t i = 0; i != 4; ++i)
        {
            if (!extract_number((*q)[i], values[i])) return false;
        }
        const double length_sq = double(values[0]) * values[0]
            + double(values[1]) * values[1] + double(values[2]) * values[2]
            + double(values[3]) * values[3];
        if (length_sq < 0.000001 || length_sq > 4.0) return false;
        rotations.emplace(name, LLQuaternion(values[1], values[2], values[3], values[0]));
    }
    positions.clear();
    if (positions_payload)
    {
        const auto* object = positions_payload->if_object();
        if (!object || object->size() > MAX_JOINTS) return false;
        for (const auto& entry : *object)
        {
            const std::string name(entry.key().data(), entry.key().size());
            if (name.size() < 2 || name.size() > 80 || name[0] != 'm') return false;
            const auto* v = entry.value().if_array();
            if (!v || v->size() != 3) return false;
            float p[3];
            for (int axis = 0; axis < 3; ++axis)
            {
                if (!extract_number((*v)[axis], p[axis]) || std::abs(p[axis]) > 5.f)
                    return false;
            }
            positions.emplace(name, LLVector3(p[0], p[1], p[2]));
        }
    }
    has_pelvis = false;
    if (const auto* v = pelvis_payload)
    {
        const auto* p = v->if_array();
        if (!p || p->size() != 3) return false;
        float n[3];
        for (int i = 0; i < 3; ++i)
        {
            if (!extract_number((*p)[i], n[i]) || std::abs(n[i]) > 5.0f) return false;
        }
        pelvis.set(n[0], n[1], n[2]);
        has_pelvis = true;
    }
    return true;
}
}

namespace BlazingStorm
{
struct BlenderLiveLink::Impl
{
    asio::io_context io;
    std::unique_ptr<udp::socket> socket;
    bool enabled = false;
    bool active = false;
    bool has_pelvis = false;
    std::string session;
    std::uint64_t sequence = 0;
    Clock::time_point last_packet{};
    std::unordered_map<std::string, LLQuaternion> rotations;
    std::unordered_map<std::string, LLQuaternion> previous_rotations;
    // New per-bone translations are offsets relative to the original local
    // joint positions, never absolute coordinates from another skeleton.
    std::unordered_map<std::string, LLVector3> positions;
    std::unordered_map<std::string, LLVector3> previous_positions;
    LLVector3 pelvis;
    LLVector3 previous_pelvis;
    bool previous_pelvis_valid = false;

    // Separate, explicit viewer->Blender stream, with a short-lived lease.
    // The peer is accepted only through the loopback-only receive socket.
    bool poser_streaming = false;
    udp::endpoint poser_peer;
    std::string poser_session;
    Clock::time_point poser_last_subscribe{};
    Clock::time_point poser_last_send{};
    Clock::time_point poser_last_check{};
    std::string last_poser_payload;
    std::uint64_t poser_stream_sequence = 0;

    // Authority is negotiated per joint. Poser takes control only when its
    // actual target changes; Blender takes it back with a higher edit revision.
    // Neither the persistent Poser modified flag nor preview refresh is an edit.
    struct PoserTarget
    {
        LLQuaternion rotation;
        LLVector3 position;
    };
    std::unordered_map<std::string, PoserTarget> last_poser_targets;
    // Keep temporary Blender preview changes out of the persistent Poser
    // state when Live Link stops, unless the user has since edited in Poser.
    std::unordered_map<std::string, PoserTarget> original_poser_targets;
    std::unordered_map<std::string, std::uint64_t> blender_edit_revisions;
    std::unordered_set<std::string> blender_owned;

    void observe_poser_edits(LLVOAvatar* avatar)
    {
        static FSPoserAnimator poser;
        FSPosingMotion* motion = poser.getPosingMotionForLiveLink(avatar);
        std::unordered_set<std::string> currently_edited;
        if (motion && !motion->isStopped())
        {
            for (S32 i = 0; i < static_cast<S32>(MAX_JOINTS); ++i)
            {
                LLJoint* joint = avatar->getCharacterJoint(i);
                if (!joint) break;
                const std::string& name = joint->getName();
                if (name.size() < 2 || name[0] != 'm') continue;
                FSJointPose* pose = motion->getJointPoseByJointName(name);
                if (!pose || !motion->currentlyPosingJoint(pose)
                    || !pose->getJointModified()) continue;
                currently_edited.insert(name);
                const PoserTarget current{pose->getTargetRotation(), pose->getTargetPosition()};
                const auto previous = last_poser_targets.find(name);
                bool changed = previous == last_poser_targets.end();
                if (!changed)
                {
                    // Quaternions q and -q represent the same rotation.
                    float delta = 0.f, opposite = 0.f;
                    for (int j = 0; j < 4; ++j)
                    {
                        const float a = previous->second.rotation.mQ[j];
                        const float b = current.rotation.mQ[j];
                        delta += (a - b) * (a - b);
                        opposite += (a + b) * (a + b);
                    }
                    changed = std::min(delta, opposite) > 0.00000001f
                        || (current.position - previous->second.position).lengthSquared() > 0.00000001f;
                }
                if (changed)
                {
                    blender_owned.erase(name);
                    original_poser_targets.erase(name); // user edit supersedes temporary baseline
                    last_poser_targets[name] = current;
                }
            }
        }
        for (auto it = last_poser_targets.begin(); it != last_poser_targets.end();)
        {
            if (currently_edited.count(it->first) == 0)
                it = last_poser_targets.erase(it);
            else ++it;
        }
    }

    void rebase_poser_target(LLVOAvatar* avatar, const std::string& name,
                             const LLQuaternion* rotation, const LLVector3* position)
    {
        static FSPoserAnimator poser;
        FSPosingMotion* motion = poser.getPosingMotionForLiveLink(avatar);
        if (!motion || motion->isStopped()) return;
        FSJointPose* pose = motion->getJointPoseByJointName(name);
        if (!pose || !motion->currentlyPosingJoint(pose)) return;
        if (original_poser_targets.count(name) == 0)
            original_poser_targets.emplace(name,
                PoserTarget{pose->getTargetRotation(), pose->getTargetPosition()});
        if (rotation) pose->setLiveLinkTargetRotation(*rotation);
        if (position) pose->setLiveLinkTargetPosition(*position);
        // A remote preview is NOT a Poser user edit. Remember this target
        // so the next observer pass does not accidentally transfer control.
        if (pose->getJointModified())
            last_poser_targets[name] =
                PoserTarget{pose->getTargetRotation(), pose->getTargetPosition()};
    }

    void restore_poser_targets(LLVOAvatar* avatar)
    {
        static FSPoserAnimator poser;
        FSPosingMotion* motion = poser.getPosingMotionForLiveLink(avatar);
        if (motion && !motion->isStopped())
        {
            for (const auto& entry : original_poser_targets)
            {
                FSJointPose* pose = motion->getJointPoseByJointName(entry.first);
                if (!pose || !motion->currentlyPosingJoint(pose)) continue;
                pose->setLiveLinkTargetRotation(entry.second.rotation);
                pose->setLiveLinkTargetPosition(entry.second.position);
            }
        }
        original_poser_targets.clear();
        last_poser_targets.clear();
    }

    // Read the Poser motion target rather than the rendered skeleton (which
    // already contains Blender's preview). Do not treat getJointModified()
    // alone as permanently locking a bone.
    bool poser_controls_joint(LLVOAvatar* avatar, const std::string& name) const
    {
        static FSPoserAnimator poser;
        FSPosingMotion* motion = poser.getPosingMotionForLiveLink(avatar);
        if (!motion || motion->isStopped()) return false;
        FSJointPose* joint_pose = motion->getJointPoseByJointName(name);
        return joint_pose && motion->currentlyPosingJoint(joint_pose)
            && joint_pose->getJointModified()
            && blender_owned.count(name) == 0;
    }

    void stop_poser_stream()
    {
        poser_streaming = false;
        poser_session.clear();
        last_poser_payload.clear();
        poser_stream_sequence = 0;
    }

    void reply_poser_stream_ack(const udp::endpoint& peer, const std::string& session_id)
    {
        json::object message;
        message["protocol"] = PROTOCOL;
        message["version"] = 1;
        message["type"] = "poser_stream_ack";
        message["session"] = session_id;
        const std::string wire = json::serialize(message);
        boost::system::error_code ignored;
        socket->send_to(asio::buffer(wire), peer, 0, ignored);
    }

    bool open()
    {
        if (socket && socket->is_open()) return true;
        boost::system::error_code error;
        socket = std::make_unique<udp::socket>(io);
        socket->open(udp::v4(), error);
        if (!error) socket->bind(udp::endpoint(asio::ip::address_v4::loopback(), PORT), error);
        if (!error) socket->non_blocking(true, error);
        if (error)
        {
            LL_WARNS("BlenderLiveLink") << "Unable to bind local Blender port "
                << PORT << ": " << error.message() << LL_ENDL;
            socket.reset();
            return false;
        }
        LL_INFOS("BlenderLiveLink") << "Listening on local port " << PORT << LL_ENDL;
        return true;
    }

    void restore(LLVOAvatar* avatar)
    {
        restore_poser_targets(avatar);
        for (const auto& entry : previous_rotations)
        {
            if (LLJoint* joint = avatar->getJoint(entry.first))
            {
                if (!poser_controls_joint(avatar, entry.first))
                    joint->setRotation(entry.second);
            }
        }
        for (const auto& entry : previous_positions)
        {
            if (LLJoint* joint = avatar->getJoint(entry.first))
            {
                if (!poser_controls_joint(avatar, entry.first))
                    joint->setPosition(entry.second);
            }
        }
        previous_positions.clear();
        positions.clear();
        if (previous_pelvis_valid)
        {
            if (LLJoint* joint = avatar->getJoint("mPelvis"))
            {
                if (!poser_controls_joint(avatar, "mPelvis"))
                    joint->setPosition(previous_pelvis);
            }
        }
        previous_rotations.clear();
        previous_pelvis_valid = false;
        rotations.clear();
        active = false;
        has_pelvis = false;
        session.clear();
        sequence = 0;
        blender_owned.clear();
        blender_edit_revisions.clear();
    }

    void reply(const udp::endpoint& peer)
    {
        const std::string ack = "{\"protocol\":\"bs-blender-live-link\",\"version\":1,"
           "\"type\":\"ack\",\"session\":\"" + session + "\"}";
        boost::system::error_code ignored;
        socket->send_to(asio::buffer(ack), peer, 0, ignored);
    }

    void reply_snapshot(const udp::endpoint& peer, LLVOAvatar* avatar,
                        const std::string& requested_session, bool stream = false)
    {
        json::object message;
        message["protocol"] = PROTOCOL;
        message["version"] = 1;
        message["type"] = stream ? "poser_frame" : "pose_snapshot";
        message["session"] = requested_session;

        json::object joints;
        json::object offsets;
        // Read directly from Firestorm's live motion (not a stale rendered
        // avatar pose, a separate animator instance's UI selection, or the
        // last Blender preview). This lets Poser drive a rig even when no
        // Blender->viewer preview is running.
        static FSPoserAnimator poser;
        FSPosingMotion* motion = poser.getPosingMotionForLiveLink(avatar);
        const bool poser_active = motion && !motion->isStopped();
        S32 posed_joint_count = 0;
        S32 edited_joint_count = 0;
        if (poser_active)
        {
            for (S32 i = 0; i < static_cast<S32>(MAX_JOINTS); ++i)
            {
                LLJoint* joint = avatar->getCharacterJoint(i);
                if (!joint) break;
                const std::string& name = joint->getName();
                if (name.size() < 2 || name[0] != 'm') continue;
                FSJointPose* pose = motion->getJointPoseByJointName(name);
                if (!pose || !motion->currentlyPosingJoint(pose)) continue;
                ++posed_joint_count;
                // Only emit bones explicitly changed with Poser. Every
                // avatar joint can be registered with the Poser motion,
                // but unmodified ones must continue playing Blender tracks.
                if (!pose->getJointModified()) continue;
                ++edited_joint_count;
                // Do not echo a joint which Blender has subsequently edited.
                if (blender_owned.count(name) != 0) continue;
                const LLQuaternion rotation = pose->getTargetRotation();
                joints[name] = json::array{double(rotation.mQ[VW]),
                    double(rotation.mQ[VX]), double(rotation.mQ[VY]),
                    double(rotation.mQ[VZ])};
                const LLVector3 offset = pose->getTargetPosition()
                    - joint->getDefaultPosition();
                if (offset.lengthSquared() > 0.000001f)
                {
                    offsets[name] = json::array{double(offset.mV[VX]),
                        double(offset.mV[VY]), double(offset.mV[VZ])};
                }
            }
        }
        message["joints"] = std::move(joints);
        message["positions"] = std::move(offsets);
        // Diagnostics tell Blender whether the stream is connected but Poser
        // has not started, or whether edited bones failed to match the rig.
        message["poser_active"] = poser_active;
        message["posed_count"] = static_cast<std::int64_t>(posed_joint_count);
        message["edited_count"] = static_cast<std::int64_t>(edited_joint_count);
        const auto now = Clock::now();
        if (stream)
        {
            const std::string payload = json::serialize(message);
            if (payload == last_poser_payload
                && now - poser_last_send < POSER_STREAM_HEARTBEAT) return;
            last_poser_payload = payload;
            message["seq"] = static_cast<std::int64_t>(++poser_stream_sequence);
        }
        const std::string wire = json::serialize(message);
        if (wire.size() > MAX_PACKET_BYTES) return;
        boost::system::error_code ignored;
        socket->send_to(asio::buffer(wire), peer, 0, ignored);
        if (stream && !ignored) poser_last_send = now;
    }

    void ingest(const char* buffer, std::size_t count, const udp::endpoint& peer,
                LLVOAvatar* avatar)
    {
        boost::system::error_code error;
        const auto value = json::parse(boost::core::string_view(buffer, count), error);
        if (error) return;
        const auto* obj = value.if_object();
        if (!obj) return;
        const auto* proto = obj->if_contains("protocol");
        const auto* ver = obj->if_contains("version");
        const auto* type = obj->if_contains("type");
        const auto* sender_session = obj->if_contains("session");
        const auto* seq = obj->if_contains("seq");
        if (!proto || !proto->is_string() || proto->as_string() != PROTOCOL
            || !ver || !ver->is_int64() || ver->as_int64() != 1
            || !type || !type->is_string()
            || !sender_session || !sender_session->is_string()
            || sender_session->as_string().size() != 32
            || !seq) return;
        const std::string incoming_session(sender_session->as_string().data(), sender_session->as_string().size());
        for (char ch : incoming_session)
        {
            if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'))) return;
        }
        std::uint64_t incoming_seq = 0;
        if (!sequence_from_json(*seq, incoming_seq)) return;
        if (type->as_string() == "poser_stream_start")
        {
            // Streams stay independent, with edit-based per-joint handoff.
            // Each client listens on its own ephemeral loopback UDP port.
            if (!poser_streaming || incoming_session != poser_session || peer != poser_peer)
            {
                poser_stream_sequence = 0;
                last_poser_payload.clear();
                poser_last_send = Clock::time_point{};
                poser_last_check = Clock::time_point{};
            }
            poser_streaming = true;
            poser_session = incoming_session;
            poser_peer = peer;
            poser_last_subscribe = Clock::now();
            reply_poser_stream_ack(peer, incoming_session);
            return;
        }
        if (type->as_string() == "poser_stream_stop")
        {
            if (poser_streaming && incoming_session == poser_session && peer == poser_peer)
                stop_poser_stream();
            return;
        }
        if (type->as_string() == "snapshot_request")
        {
            // Snapshot reads only joints actually edited in Firestorm Poser,
            // not Blender's latest skeleton override.
            reply_snapshot(peer, avatar, incoming_session);
            return;
        }
        if (type->as_string() == "stop")
        {
            if (active && incoming_session == session && incoming_seq >= sequence)
                restore(avatar);
            return;
        }
        if (type->as_string() != "pose") return;
        // Both channels may be active. Per-joint ownership below avoids
        // feeding Blender's preview back over the Poser-only stream.
        const auto* pose = obj->if_contains("joints");
        if (!pose) return;
        std::unordered_map<std::string, LLQuaternion> incoming;
        std::unordered_map<std::string, LLVector3> incoming_positions;
        bool incoming_pelvis_enabled = false;
        LLVector3 incoming_pelvis;
        if (!parse_pose(*pose, obj->if_contains("pelvis"), obj->if_contains("positions"),
            incoming, incoming_positions, incoming_pelvis_enabled, incoming_pelvis)) return;
        // Prevent stale UDP packets from rolling the avatar backwards.
        if (active && session == incoming_session && incoming_seq <= sequence) return;
        if (active && session != incoming_session) restore(avatar);
        // A revision number denotes a *new local Blender edit*. Repeated
        // packets/heartbeats with the same number never steal Poser ownership.
        // Parse and validate the entire optional map before changing authority.
        std::unordered_map<std::string, std::uint64_t> edits;
        if (const auto* edit_value = obj->if_contains("blender_edits"))
        {
            const auto* edit_object = edit_value->if_object();
            if (!edit_object || edit_object->size() > MAX_JOINTS) return;
            for (const auto& entry : *edit_object)
            {
                const std::string name(entry.key().data(), entry.key().size());
                std::uint64_t revision = 0;
                if (name.size() < 2 || name.size() > 80 || name[0] != 'm'
                    || incoming.find(name) == incoming.end()
                    || !sequence_from_json(entry.value(), revision) || revision == 0) return;
                edits.emplace(name, revision);
            }
        }
        for (const auto& entry : edits)
        {
            const auto found = blender_edit_revisions.find(entry.first);
            if (found == blender_edit_revisions.end() || entry.second > found->second)
            {
                blender_edit_revisions[entry.first] = entry.second;
                blender_owned.insert(entry.first);
            }
        }
        // Release any bones removed by the new packet.
        for (auto it = previous_rotations.begin(); it != previous_rotations.end();)
        {
            if (incoming.find(it->first) == incoming.end())
            {
                if (LLJoint* joint = avatar->getJoint(it->first))
                {
                    if (!poser_controls_joint(avatar, it->first))
                        joint->setRotation(it->second);
                }
                it = previous_rotations.erase(it);
            }
            else ++it;
        }
        // Unpose joints that are no longer translated by the new selection.
        for (auto it = previous_positions.begin(); it != previous_positions.end();)
        {
            if (incoming_positions.find(it->first) == incoming_positions.end())
            {
                if (LLJoint* joint = avatar->getJoint(it->first))
                    if (!poser_controls_joint(avatar, it->first))
                        joint->setPosition(it->second);
                it = previous_positions.erase(it);
            }
            else ++it;
        }
        if (previous_pelvis_valid && !incoming_pelvis_enabled)
        {
            if (LLJoint* joint = avatar->getJoint("mPelvis"))
            {
                if (!poser_controls_joint(avatar, "mPelvis"))
                    joint->setPosition(previous_pelvis);
            }
            previous_pelvis_valid = false;
        }
        rotations = std::move(incoming);
        positions = std::move(incoming_positions);
        has_pelvis = incoming_pelvis_enabled;
        pelvis = incoming_pelvis;
        session = incoming_session;
        sequence = incoming_seq;
        last_packet = Clock::now();
        active = true;
        reply(peer);
    }

    void maybe_stream_poser(LLVOAvatar* avatar)
    {
        if (!poser_streaming) return;
        const auto now = Clock::now();
        if (now - poser_last_subscribe > POSER_STREAM_TIMEOUT)
        {
            stop_poser_stream();
            return;
        }
        if (now - poser_last_check < POSER_STREAM_INTERVAL) return;
        poser_last_check = now;
        reply_snapshot(poser_peer, avatar, poser_session, true);
    }

    void poll(LLVOAvatar* avatar)
    {
        if (!socket || !socket->is_open()) return;
        // Bound work on the main/render thread: never block to wait for Blender.
        for (int i = 0; i < 12; ++i)
        {
            std::array<char, MAX_PACKET_BYTES + 1> packet{};
            udp::endpoint peer;
            boost::system::error_code error;
            std::size_t count = socket->receive_from(asio::buffer(packet), peer, 0, error);
            if (error == asio::error::would_block || error == asio::error::try_again) break;
            if (error) break;
            if (!peer.address().is_v4() || !peer.address().to_v4().is_loopback()
                || count > MAX_PACKET_BYTES) continue;
            ingest(packet.data(), count, peer, avatar);
        }
    }
};

BlenderLiveLink::BlenderLiveLink() : mImpl(std::make_unique<Impl>()) {}
BlenderLiveLink::~BlenderLiveLink() = default;

BlenderLiveLink& BlenderLiveLink::instance()
{
    static BlenderLiveLink instance;
    return instance;
}

void BlenderLiveLink::apply(LLVOAvatar* avatar)
{
    if (!avatar || !avatar->isSelf()) return;
    Impl& link = *mImpl;
    if (!gSavedSettings.getBOOL("BSBlenderLiveLinkEnabled"))
    {
        if (link.active) link.restore(avatar);
        link.stop_poser_stream();
        link.socket.reset();
        return;
    }
    if (!link.open()) return;
    link.observe_poser_edits(avatar);
    link.poll(avatar);
    link.maybe_stream_poser(avatar);
    if (!link.active) return;
    if (Clock::now() - link.last_packet > TIMEOUT)
    {
        link.restore(avatar);
        return;
    }
    for (const auto& entry : link.rotations)
    {
        LLJoint* joint = avatar->getJoint(entry.first);
        if (!joint) continue;
        if (link.poser_controls_joint(avatar, entry.first))
        {
            // Poser's target rotations override the Blender preview for
            // edited joints. Keep all the other Blender tracks running.
            link.previous_rotations.erase(entry.first);
            continue;
        }
        if (link.previous_rotations.find(entry.first) == link.previous_rotations.end())
        {
            link.previous_rotations.emplace(entry.first, joint->getRotation());
        }
        link.rebase_poser_target(avatar, entry.first, &entry.second, nullptr);
        joint->setRotation(entry.second);
    }
    for (const auto& entry : link.positions)
    {
        if (LLJoint* joint = avatar->getJoint(entry.first))
        {
            if (link.poser_controls_joint(avatar, entry.first))
            {
                link.previous_positions.erase(entry.first);
                continue;
            }
            auto found = link.previous_positions.find(entry.first);
            if (found == link.previous_positions.end())
            {
                found = link.previous_positions.emplace(entry.first, joint->getPosition()).first;
            }
            const LLVector3 target = found->second + entry.second;
            link.rebase_poser_target(avatar, entry.first, nullptr, &target);
            joint->setPosition(target);
        }
    }
    // Legacy clients may still send an absolute pelvis value.
    if (link.has_pelvis && link.positions.find("mPelvis") == link.positions.end())
    {
        if (LLJoint* joint = avatar->getJoint("mPelvis"))
        {
            if (link.poser_controls_joint(avatar, "mPelvis")) return;
            if (!link.previous_pelvis_valid)
            {
                link.previous_pelvis = joint->getPosition();
                link.previous_pelvis_valid = true;
            }
            link.rebase_poser_target(avatar, "mPelvis", nullptr, &link.pelvis);
            joint->setPosition(link.pelvis);
        }
    }
}
}
