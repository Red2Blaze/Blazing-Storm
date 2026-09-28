/**
 * @file bsrelaytransport.h
 * @brief Outbound TLS WebSocket client used by the Blazing Storm relay.
 */

#ifndef BS_RELAY_TRANSPORT_H
#define BS_RELAY_TRANSPORT_H

#include <memory>
#include <string>
#include <vector>

namespace BlazingStorm
{
    class RelayTransport final
    {
    public:
        enum class EventType
        {
            SocketConnected,
            Message,
            Closed,
            Error
        };

        struct Event
        {
            EventType type = EventType::Error;
            std::string payload;
        };

        static constexpr std::size_t MAX_MESSAGE_BYTES = 128 * 1024;

        static RelayTransport& instance();

        // Opens an outbound WSS connection. hello_json is sent as the first
        // WebSocket text message after TLS and WebSocket handshakes complete.
        bool connect(const std::string& relay_url, const std::string& hello_json);
        void disconnect();

        // Queues one complete WebSocket text message. The socket itself is
        // owned exclusively by the relay network thread.
        bool sendText(const std::string& payload);

        std::vector<Event> takeEvents();
        bool isOpen() const;
        const std::string& lastStartError() const { return mLastStartError; }

    private:
        class Impl;

        RelayTransport() = default;
        ~RelayTransport();
        RelayTransport(const RelayTransport&) = delete;
        RelayTransport& operator=(const RelayTransport&) = delete;

        std::unique_ptr<Impl> mImpl;
        std::string mLastStartError;
    };
}

#endif // BS_RELAY_TRANSPORT_H
