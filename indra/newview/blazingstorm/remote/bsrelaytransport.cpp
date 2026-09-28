/**
 * @file bsrelaytransport.cpp
 * @brief Outbound TLS WebSocket client used by the Blazing Storm relay.
 */

#include "llviewerprecompiledheaders.h"

#include "blazingstorm/remote/bsrelaytransport.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl/context.hpp>
#include <boost/asio/ssl/host_name_verification.hpp>
#include <boost/asio/ssl/stream_base.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#ifdef LL_WINDOWS
#include <wincrypt.h>
#endif

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

namespace
{
    namespace asio = boost::asio;
    namespace beast = boost::beast;
    namespace websocket = beast::websocket;
    namespace ssl = asio::ssl;
    using tcp = asio::ip::tcp;

    struct RelayEndpoint
    {
        std::string host;
        std::string hostHeader;
        std::string port;
        std::string target;
    };

    bool parseWssUrl(const std::string& url, RelayEndpoint& endpoint, std::string& error)
    {
        static const std::string prefix = "wss://";
        if (url.compare(0, prefix.size(), prefix) != 0)
        {
            error = "Relay URL must begin with wss://.";
            return false;
        }

        const std::size_t authority_begin = prefix.size();
        std::size_t target_begin = url.find('/', authority_begin);
        const std::size_t query_begin = url.find('?', authority_begin);
        if (target_begin == std::string::npos
            || (query_begin != std::string::npos && query_begin < target_begin))
        {
            target_begin = query_begin;
        }

        const std::string authority = target_begin == std::string::npos
            ? url.substr(authority_begin)
            : url.substr(authority_begin, target_begin - authority_begin);

        if (authority.empty() || authority.find('@') != std::string::npos
            || authority.find('#') != std::string::npos
            || authority.find(' ') != std::string::npos)
        {
            error = "Relay URL has an invalid host.";
            return false;
        }

        endpoint.hostHeader = authority;
        endpoint.port = "443";

        if (authority.front() == '[')
        {
            const auto closing = authority.find(']');
            if (closing == std::string::npos || closing == 1)
            {
                error = "Relay URL has an invalid IPv6 host.";
                return false;
            }

            endpoint.host = authority.substr(1, closing - 1);
            if (closing + 1 < authority.size())
            {
                if (authority[closing + 1] != ':' || closing + 2 >= authority.size())
                {
                    error = "Relay URL has an invalid port.";
                    return false;
                }
                endpoint.port = authority.substr(closing + 2);
            }
        }
        else
        {
            const auto colon = authority.rfind(':');
            if (colon != std::string::npos)
            {
                if (colon == 0 || colon + 1 >= authority.size()
                    || authority.find(':') != colon)
                {
                    error = "Relay URL has an invalid host or port.";
                    return false;
                }
                endpoint.host = authority.substr(0, colon);
                endpoint.port = authority.substr(colon + 1);
            }
            else
            {
                endpoint.host = authority;
            }
        }

        if (endpoint.host.empty())
        {
            error = "Relay URL has an empty host.";
            return false;
        }

        for (const char ch : endpoint.port)
        {
            if (ch < '0' || ch > '9')
            {
                error = "Relay URL port must be numeric.";
                return false;
            }
        }

        if (target_begin == std::string::npos)
        {
            endpoint.target = "/";
        }
        else if (url[target_begin] == '?')
        {
            endpoint.target = "/" + url.substr(target_begin);
        }
        else
        {
            endpoint.target = url.substr(target_begin);
        }

        if (endpoint.target.empty() || endpoint.target.find('#') != std::string::npos)
        {
            error = "Relay URL has an invalid WebSocket target.";
            return false;
        }

        return true;
    }

#ifdef LL_WINDOWS
    bool addWindowsRootCertificates(ssl::context& context)
    {
        HCERTSTORE windows_store = CertOpenSystemStoreA(nullptr, "ROOT");
        if (!windows_store)
        {
            return false;
        }

        X509_STORE* openssl_store =
            SSL_CTX_get_cert_store(context.native_handle());
        bool added_any = false;
        PCCERT_CONTEXT certificate = nullptr;

        while ((certificate =
                    CertEnumCertificatesInStore(windows_store, certificate)) != nullptr)
        {
            const unsigned char* encoded = certificate->pbCertEncoded;
            X509* x509 = d2i_X509(
                nullptr,
                &encoded,
                static_cast<long>(certificate->cbCertEncoded));
            if (!x509)
            {
                ERR_clear_error();
                continue;
            }

            if (X509_STORE_add_cert(openssl_store, x509) == 1)
            {
                added_any = true;
            }
            else
            {
                // Duplicate roots are expected when OpenSSL already has part
                // of the Windows trust store.
                ERR_clear_error();
            }
            X509_free(x509);
        }

        CertCloseStore(windows_store, 0);
        return added_any;
    }
#endif
}

namespace BlazingStorm
{
    class RelayTransport::Impl
    {
    public:
        using TlsWebSocket =
            websocket::stream<beast::ssl_stream<beast::tcp_stream>>;

        Impl()
        : mSsl(ssl::context::tls_client),
          mResolver(mIo)
        {
        }

        ~Impl()
        {
            stop();
        }

        bool start(const std::string& relay_url,
                   const std::string& hello_json,
                   std::string& error)
        {
            if (hello_json.empty() || hello_json.size() > 8 * 1024)
            {
                error = "Relay handshake payload is invalid.";
                return false;
            }

            if (!parseWssUrl(relay_url, mEndpoint, error))
            {
                return false;
            }

            boost::system::error_code trust_error;
            mSsl.set_default_verify_paths(trust_error);
#ifdef LL_WINDOWS
            const bool windows_roots = addWindowsRootCertificates(mSsl);
            if (trust_error && !windows_roots)
            {
                error = "Could not load TLS root certificates: "
                    + trust_error.message();
                return false;
            }
#else
            if (trust_error)
            {
                error = "Could not load TLS root certificates: "
                    + trust_error.message();
                return false;
            }
#endif

            mSsl.set_verify_mode(ssl::verify_peer);
            mHello = hello_json;
            mStopping = false;
            mOpen = false;

            try
            {
                mThread = std::thread([this]()
                {
                    asio::post(mIo, [this]() { beginResolve(); });
                    mIo.run();
                });
            }
            catch (const std::exception& e)
            {
                error = std::string("Could not start relay network thread: ")
                    + e.what();
                return false;
            }

            return true;
        }

        void stop()
        {
            if (!mThread.joinable())
            {
                mOpen = false;
                return;
            }

            mStopping = true;
            mOpen = false;
            mIo.stop();
            mThread.join();
            mSocket.reset();
            mWrites.clear();
        }

        bool sendText(const std::string& payload)
        {
            if (!mOpen || payload.empty()
                || payload.size() > RelayTransport::MAX_MESSAGE_BYTES)
            {
                return false;
            }

            asio::post(mIo, [this, payload]()
            {
                if (mStopping || !mSocket || !mSocket->is_open())
                {
                    return;
                }

                const bool idle = mWrites.empty();
                mWrites.push_back(payload);
                if (idle)
                {
                    beginWrite();
                }
            });
            return true;
        }

        std::vector<Event> takeEvents()
        {
            std::lock_guard<std::mutex> lock(mEventMutex);
            std::vector<Event> result;
            result.reserve(mEvents.size());
            while (!mEvents.empty())
            {
                result.push_back(std::move(mEvents.front()));
                mEvents.pop_front();
            }
            return result;
        }

        bool isOpen() const
        {
            return mOpen;
        }

    private:
        void pushEvent(EventType type, std::string payload = {})
        {
            std::lock_guard<std::mutex> lock(mEventMutex);
            mEvents.push_back(Event{type, std::move(payload)});
        }

        void fail(const char* operation, const boost::system::error_code& error)
        {
            mOpen = false;
            if (!mStopping)
            {
                pushEvent(
                    EventType::Error,
                    std::string(operation) + ": " + error.message());
            }
            mIo.stop();
        }

        void beginResolve()
        {
            mResolver.async_resolve(
                mEndpoint.host,
                mEndpoint.port,
                [this](const boost::system::error_code& error,
                       tcp::resolver::results_type results)
                {
                    if (error)
                    {
                        fail("Relay DNS lookup failed", error);
                        return;
                    }

                    mSocket = std::make_unique<TlsWebSocket>(mIo, mSsl);
                    beast::get_lowest_layer(*mSocket).expires_after(
                        std::chrono::seconds(15));

                    beast::get_lowest_layer(*mSocket).async_connect(
                        results,
                        [this](const boost::system::error_code& connect_error,
                               const tcp::resolver::results_type::endpoint_type&)
                        {
                            if (connect_error)
                            {
                                fail("Relay TCP connection failed", connect_error);
                                return;
                            }
                            beginTlsHandshake();
                        });
                });
        }

        void beginTlsHandshake()
        {
            if (!mSocket)
            {
                return;
            }

            if (!SSL_set_tlsext_host_name(
                    mSocket->next_layer().native_handle(),
                    mEndpoint.host.c_str()))
            {
                const boost::system::error_code error(
                    static_cast<int>(::ERR_get_error()),
                    asio::error::get_ssl_category());
                fail("Relay TLS SNI setup failed", error);
                return;
            }

            mSocket->next_layer().set_verify_callback(
                ssl::host_name_verification(mEndpoint.host));

            mSocket->next_layer().async_handshake(
                ssl::stream_base::client,
                [this](const boost::system::error_code& error)
                {
                    if (error)
                    {
                        fail("Relay TLS handshake failed", error);
                        return;
                    }
                    beginWebSocketHandshake();
                });
        }

        void beginWebSocketHandshake()
        {
            beast::get_lowest_layer(*mSocket).expires_never();
            mSocket->set_option(
                websocket::stream_base::timeout::suggested(
                    beast::role_type::client));
            mSocket->set_option(websocket::stream_base::decorator(
                [](websocket::request_type& request)
                {
                    request.set(
                        boost::beast::http::field::user_agent,
                        "BlazingStormViewer/1");
                }));
            mSocket->read_message_max(RelayTransport::MAX_MESSAGE_BYTES);
            mSocket->text(true);

            mSocket->async_handshake(
                mEndpoint.hostHeader,
                mEndpoint.target,
                [this](const boost::system::error_code& error)
                {
                    if (error)
                    {
                        fail("Relay WebSocket handshake failed", error);
                        return;
                    }

                    mOpen = true;
                    mWrites.push_back(mHello);
                    beginWrite();
                    pushEvent(EventType::SocketConnected);
                    beginRead();
                });
        }

        void beginRead()
        {
            if (!mSocket || !mSocket->is_open() || mStopping)
            {
                return;
            }

            mSocket->async_read(
                mReadBuffer,
                [this](const boost::system::error_code& error,
                       std::size_t)
                {
                    if (error)
                    {
                        mOpen = false;
                        if (!mStopping)
                        {
                            if (error == websocket::error::closed)
                            {
                                pushEvent(
                                    EventType::Closed,
                                    "Relay connection closed.");
                                mIo.stop();
                            }
                            else
                            {
                                fail("Relay read failed", error);
                            }
                        }
                        return;
                    }

                    if (!mSocket->got_text())
                    {
                        pushEvent(
                            EventType::Error,
                            "Relay sent a binary message; closing fail-closed.");
                        mOpen = false;
                        mIo.stop();
                        return;
                    }

                    std::string payload =
                        beast::buffers_to_string(mReadBuffer.data());
                    mReadBuffer.consume(mReadBuffer.size());

                    if (payload.size() > RelayTransport::MAX_MESSAGE_BYTES)
                    {
                        pushEvent(
                            EventType::Error,
                            "Relay message exceeded the viewer size limit.");
                        mOpen = false;
                        mIo.stop();
                        return;
                    }

                    pushEvent(EventType::Message, std::move(payload));
                    beginRead();
                });
        }

        void beginWrite()
        {
            if (!mSocket || !mSocket->is_open() || mWrites.empty() || mStopping)
            {
                return;
            }

            mSocket->text(true);
            mSocket->async_write(
                asio::buffer(mWrites.front()),
                [this](const boost::system::error_code& error,
                       std::size_t)
                {
                    if (error)
                    {
                        fail("Relay write failed", error);
                        return;
                    }

                    mWrites.pop_front();
                    if (!mWrites.empty())
                    {
                        beginWrite();
                    }
                });
        }

        asio::io_context mIo;
        ssl::context mSsl;
        tcp::resolver mResolver;
        std::unique_ptr<TlsWebSocket> mSocket;
        RelayEndpoint mEndpoint;

        beast::flat_buffer mReadBuffer;
        std::deque<std::string> mWrites;
        std::string mHello;

        std::atomic<bool> mOpen{false};
        std::atomic<bool> mStopping{false};
        std::thread mThread;

        std::mutex mEventMutex;
        std::deque<Event> mEvents;
    };

    RelayTransport& RelayTransport::instance()
    {
        static RelayTransport transport;
        return transport;
    }

    RelayTransport::~RelayTransport()
    {
        disconnect();
    }

    bool RelayTransport::connect(
        const std::string& relay_url,
        const std::string& hello_json)
    {
        disconnect();

        auto impl = std::make_unique<Impl>();
        std::string error;
        if (!impl->start(relay_url, hello_json, error))
        {
            mLastStartError = error;
            return false;
        }

        mLastStartError.clear();
        mImpl = std::move(impl);
        return true;
    }

    void RelayTransport::disconnect()
    {
        if (mImpl)
        {
            mImpl->stop();
            mImpl.reset();
        }
    }

    bool RelayTransport::sendText(const std::string& payload)
    {
        return mImpl && mImpl->sendText(payload);
    }

    std::vector<RelayTransport::Event> RelayTransport::takeEvents()
    {
        return mImpl ? mImpl->takeEvents() : std::vector<Event>{};
    }

    bool RelayTransport::isOpen() const
    {
        return mImpl && mImpl->isOpen();
    }
}
