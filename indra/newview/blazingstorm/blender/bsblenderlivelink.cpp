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

#include "lljoint.h"
#include "llquaternion.h"
#include "llviewercontrol.h"
#include "llvoavatar.h"

#include <boost/asio.hpp>
#include <boost/json.hpp>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
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
    std::unordered_map<std::string, LLQuaternion>& rotations,
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
    LLVector3 pelvis;
    LLVector3 previous_pelvis;
    bool previous_pelvis_valid = false;

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
        for (const auto& entry : previous_rotations)
        {
            if (LLJoint* joint = avatar->getJoint(entry.first))
            {
                joint->setRotation(entry.second);
            }
        }
        if (previous_pelvis_valid)
        {
            if (LLJoint* joint = avatar->getJoint("mPelvis"))
            {
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
    }

    void reply(const udp::endpoint& peer)
    {
        const std::string ack = "{\"protocol\":\"bs-blender-live-link\",\"version\":1,"
           "\"type\":\"ack\",\"session\":\"" + session + "\"}";
        boost::system::error_code ignored;
        socket->send_to(asio::buffer(ack), peer, 0, ignored);
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
        if (type->as_string() == "stop")
        {
            if (active && incoming_session == session && incoming_seq >= sequence)
                restore(avatar);
            return;
        }
        if (type->as_string() != "pose") return;
        const auto* pose = obj->if_contains("joints");
        if (!pose) return;
        std::unordered_map<std::string, LLQuaternion> incoming;
        bool incoming_pelvis_enabled = false;
        LLVector3 incoming_pelvis;
        if (!parse_pose(*pose, obj->if_contains("pelvis"), incoming, incoming_pelvis_enabled, incoming_pelvis)) return;
        // Prevent stale UDP packets from rolling the avatar backwards.
        if (active && session == incoming_session && incoming_seq <= sequence) return;
        if (active && session != incoming_session) restore(avatar);
        // Release any bones removed by the new packet.
        for (auto it = previous_rotations.begin(); it != previous_rotations.end();)
        {
            if (incoming.find(it->first) == incoming.end())
            {
                if (LLJoint* joint = avatar->getJoint(it->first)) joint->setRotation(it->second);
                it = previous_rotations.erase(it);
            }
            else ++it;
        }
        if (previous_pelvis_valid && !incoming_pelvis_enabled)
        {
            if (LLJoint* joint = avatar->getJoint("mPelvis")) joint->setPosition(previous_pelvis);
            previous_pelvis_valid = false;
        }
        rotations = std::move(incoming);
        has_pelvis = incoming_pelvis_enabled;
        pelvis = incoming_pelvis;
        session = incoming_session;
        sequence = incoming_seq;
        last_packet = Clock::now();
        active = true;
        reply(peer);
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
        link.socket.reset();
        return;
    }
    if (!link.open()) return;
    link.poll(avatar);
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
        if (link.previous_rotations.find(entry.first) == link.previous_rotations.end())
        {
            link.previous_rotations.emplace(entry.first, joint->getRotation());
        }
        joint->setRotation(entry.second);
    }
    if (link.has_pelvis)
    {
        if (LLJoint* joint = avatar->getJoint("mPelvis"))
        {
            if (!link.previous_pelvis_valid)
            {
                link.previous_pelvis = joint->getPosition();
                link.previous_pelvis_valid = true;
            }
            joint->setPosition(link.pelvis);
        }
    }
}
}
