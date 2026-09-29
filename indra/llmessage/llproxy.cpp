/**
 * @file llproxy.cpp
 * @brief UDP and HTTP proxy communications
 *
 * $LicenseInfo:firstyear=2011&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2011, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "llproxy.h"

#include <string>
#include <curl/curl.h>
#include "httpcommon.h"
#include "llapr.h"
#include "llhost.h"

// Static class variable instances

// We want this to be static to avoid excessive indirection on every
// incoming packet just to do a simple bool test. The getter for this
// member is also static
bool LLProxy::sUDPProxyEnabled = false;
LLProxy* LLProxy::sProxyInstance = NULL;

// Some helpful TCP static functions.
static apr_status_t tcp_blocking_handshake(LLSocket::ptr_t handle, const char* dataout, apr_size_t outlen, char* datain, apr_size_t inlen); // Do a TCP data handshake
static apr_status_t tcp_blocking_receive(LLSocket::ptr_t handle, char* datain, apr_size_t inlen); // Receive an exact amount of control-channel data
static LLSocket::ptr_t tcp_open_channel(LLHost host, std::string* error_message); // Open a TCP channel to a given host
static void tcp_close_channel(LLSocket::ptr_t* handle_ptr); // Close an open TCP channel

LLProxy::LLProxy():
        mHTTPProxyEnabled(false),
        mProxyMutex(),
        mUDPProxy(),
        mTCPProxy(),
        mLastSocksError(),
        mHTTPProxy(),
        mProxyType(LLPROXY_SOCKS),
        mAuthMethodSelected(METHOD_NOAUTH),
        mSocksUsername(),
        mSocksPassword()
{}

LLProxy::~LLProxy()
{
    if (ll_apr_is_initialized())
    {
        // locks mutex
        stopSOCKSProxy();
        disableHTTPProxy();
    }
    // The primary safety of sProxyInstance is the fact that by the
    // point SUBSYSTEM_CLEANUP(LLProxy) gets called, nothing should
    // be capable of using proxy
    sProxyInstance = NULL;
}

void LLProxy::initSingleton()
{
    sProxyInstance = this;
}

/**
 * @brief Open the SOCKS 5 TCP control channel.
 *
 * Perform a SOCKS 5 authentication and UDP association with the proxy server.
 *
 * @param proxy The SOCKS 5 server to connect to.
 * @return SOCKS_OK if successful, otherwise a socks error code from llproxy.h.
 */
S32 LLProxy::proxyHandshake(LLHost proxy, U16 client_udp_port)
{
    apr_status_t result;
    const LLSocks5AuthType selected_auth = getSelectedAuthMethod();

    /* SOCKS 5 Auth request */
    socks_auth_request_t socks_auth_request;
    socks_auth_response_t socks_auth_response;

    socks_auth_request.version     = SOCKS_VERSION;
    socks_auth_request.num_methods = 1;
    socks_auth_request.methods     = selected_auth;

    result = tcp_blocking_handshake(mProxyControlChannel,
                                    reinterpret_cast<const char*>(&socks_auth_request),
                                    sizeof(socks_auth_request),
                                    reinterpret_cast<char*>(&socks_auth_response),
                                    sizeof(socks_auth_response));
    if (result != APR_SUCCESS)
    {
        LL_WARNS("Proxy") << "SOCKS authentication request failed, error on TCP control channel: " << result << LL_ENDL;
        stopSOCKSProxy();
        return SOCKS_CONNECT_ERROR;
    }

    if (socks_auth_response.version != SOCKS_VERSION)
    {
        LL_WARNS("Proxy") << "SOCKS server returned unexpected protocol version "
                           << (S32)socks_auth_response.version << LL_ENDL;
        stopSOCKSProxy();
        return SOCKS_CONNECT_ERROR;
    }

    if (socks_auth_response.method == AUTH_NOT_ACCEPTABLE)
    {
        LL_WARNS("Proxy") << "SOCKS 5 server refused all our authentication methods." << LL_ENDL;
        stopSOCKSProxy();
        return SOCKS_NOT_ACCEPTABLE;
    }

    if (socks_auth_response.method != selected_auth)
    {
        LL_WARNS("Proxy") << "SOCKS 5 server selected an authentication method we did not offer: "
                           << (S32)socks_auth_response.method << LL_ENDL;
        stopSOCKSProxy();
        return SOCKS_NOT_ACCEPTABLE;
    }

    /* SOCKS 5 USERNAME/PASSWORD authentication */
    if (selected_auth == METHOD_PASSWORD)
    {
        std::string socks_username(getSocksUser());
        std::string socks_password(getSocksPwd());
        const U32 request_size = static_cast<U32>(socks_username.size() + socks_password.size() + 3);
        char* password_auth = new char[request_size];
        password_auth[0] = 0x01;
        password_auth[1] = static_cast<char>(socks_username.size());
        memcpy(&password_auth[2], socks_username.c_str(), socks_username.size());
        password_auth[socks_username.size() + 2] = static_cast<char>(socks_password.size());
        memcpy(&password_auth[socks_username.size() + 3], socks_password.c_str(), socks_password.size());

        authmethod_password_reply_t password_reply;

        result = tcp_blocking_handshake(mProxyControlChannel,
                                        password_auth,
                                        request_size,
                                        reinterpret_cast<char*>(&password_reply),
                                        sizeof(password_reply));
        delete[] password_auth;

        if (result != APR_SUCCESS)
        {
            LL_WARNS("Proxy") << "SOCKS authentication failed, error on TCP control channel: " << result << LL_ENDL;
            stopSOCKSProxy();
            return SOCKS_CONNECT_ERROR;
        }

        if (password_reply.version != 0x01 || password_reply.status != AUTH_SUCCESS)
        {
            LL_WARNS("Proxy") << "SOCKS username/password authentication failed." << LL_ENDL;
            stopSOCKSProxy();
            return SOCKS_AUTH_FAIL;
        }
    }

    /* SOCKS5 UDP ASSOCIATE request */
    socks_command_request_t connect_request;
    socks_command_response_t connect_reply;

    connect_request.version  = SOCKS_VERSION;
    connect_request.command  = COMMAND_UDP_ASSOCIATE;
    connect_request.reserved = FIELD_RESERVED;
    connect_request.atype    = ADDRESS_IPV4;
    connect_request.address  = htonl(0);
    connect_request.port     = htons(client_udp_port);
    // The proxy startup is intentionally delayed until the viewer messaging
    // socket exists, so we can provide its real UDP listen port. The address
    // remains 0.0.0.0 because the OS may choose the outbound interface and a
    // NAT gateway may change the externally visible address.

    LL_INFOS("Proxy") << "Requesting SOCKS 5 UDP association for local UDP port "
                       << client_udp_port << LL_ENDL;

    result = tcp_blocking_handshake(mProxyControlChannel,
                                    reinterpret_cast<const char*>(&connect_request),
                                    sizeof(connect_request),
                                    reinterpret_cast<char*>(&connect_reply),
                                    sizeof(connect_reply));
    if (result != APR_SUCCESS)
    {
        char status_text[MAX_STRING] = {};
        apr_strerror(result, status_text, sizeof(status_text));

        if (APR_STATUS_IS_EOF(result))
        {
            mLastSocksError =
                "The proxy closed the SOCKS control connection when UDP ASSOCIATE was requested. "
                "This usually means the proxy does not provide SOCKS5 UDP relay support.";
        }
        else
        {
            mLastSocksError = llformat(
                "UDP ASSOCIATE failed on the SOCKS control connection: %s (APR %d, OS %d)",
                status_text,
                (S32)result,
                (S32)APR_TO_OS_ERROR(result));
        }

        LL_WARNS("Proxy") << mLastSocksError << LL_ENDL;
        stopSOCKSProxy();
        return SOCKS_UDP_FWD_NOT_GRANTED;
    }

    if (connect_reply.version != SOCKS_VERSION || connect_reply.reserved != FIELD_RESERVED)
    {
        LL_WARNS("Proxy") << "SOCKS UDP ASSOCIATE returned an invalid reply header." << LL_ENDL;
        stopSOCKSProxy();
        return SOCKS_CONNECT_ERROR;
    }

    if (connect_reply.reply != REPLY_REQUEST_GRANTED)
    {
        mLastSocksError = llformat(
            "The proxy rejected UDP ASSOCIATE with SOCKS5 reply code %d.",
            (S32)connect_reply.reply);
        LL_WARNS("Proxy") << mLastSocksError << LL_ENDL;
        stopSOCKSProxy();
        return connect_reply.reply == REPLY_RULESET_FAIL ? SOCKS_NOT_PERMITTED : SOCKS_UDP_FWD_NOT_GRANTED;
    }

    // RFC 1928 says BND.ADDR/BND.PORT identify the UDP relay endpoint.
    // Older viewer code ignored BND.ADDR and always used the TCP proxy IP,
    // which breaks proxies that place their UDP relay on a different address.
    switch (connect_reply.atype)
    {
        case ADDRESS_IPV4:
        {
            U32 relay_address = 0;
            result = tcp_blocking_receive(mProxyControlChannel,
                                          reinterpret_cast<char*>(&relay_address),
                                          sizeof(relay_address));
            if (result != APR_SUCCESS)
            {
                LL_WARNS("Proxy") << "Failed reading SOCKS UDP relay IPv4 address: " << result << LL_ENDL;
                stopSOCKSProxy();
                return SOCKS_CONNECT_ERROR;
            }

            // Some SOCKS implementations return 0.0.0.0 to mean the control
            // connection's server address. Preserve compatibility with them.
            mUDPProxy.setAddress(relay_address != 0 ? relay_address : proxy.getAddress());
            break;
        }

        case ADDRESS_HOSTNAME:
        {
            U8 hostname_length = 0;
            result = tcp_blocking_receive(mProxyControlChannel,
                                          reinterpret_cast<char*>(&hostname_length),
                                          sizeof(hostname_length));
            if (result != APR_SUCCESS || hostname_length == 0)
            {
                LL_WARNS("Proxy") << "Failed reading SOCKS UDP relay hostname length." << LL_ENDL;
                stopSOCKSProxy();
                return SOCKS_CONNECT_ERROR;
            }

            char relay_hostname[MAXHOSTNAMELEN] = {};
            result = tcp_blocking_receive(mProxyControlChannel,
                                          relay_hostname,
                                          hostname_length);
            if (result != APR_SUCCESS)
            {
                LL_WARNS("Proxy") << "Failed reading SOCKS UDP relay hostname: " << result << LL_ENDL;
                stopSOCKSProxy();
                return SOCKS_CONNECT_ERROR;
            }
            relay_hostname[hostname_length] = '\0';

            LLHost relay_host;
            if (!relay_host.setHostByName(relay_hostname))
            {
                LL_WARNS("Proxy") << "Unable to resolve SOCKS UDP relay hostname: "
                                   << relay_hostname << LL_ENDL;
                stopSOCKSProxy();
                return SOCKS_INVALID_HOST;
            }
            mUDPProxy.setAddress(relay_host.getAddress());
            break;
        }

        case ADDRESS_IPV6:
            // LLHost and the viewer UDP stack are currently IPv4-only.
            LL_WARNS("Proxy") << "SOCKS server returned an IPv6 UDP relay address, which this viewer network stack cannot represent yet." << LL_ENDL;
            stopSOCKSProxy();
            return SOCKS_UDP_FWD_NOT_GRANTED;

        default:
            LL_WARNS("Proxy") << "SOCKS server returned an unknown UDP relay address type: "
                               << (S32)connect_reply.atype << LL_ENDL;
            stopSOCKSProxy();
            return SOCKS_UDP_FWD_NOT_GRANTED;
    }

    U16 relay_port = 0;
    result = tcp_blocking_receive(mProxyControlChannel,
                                  reinterpret_cast<char*>(&relay_port),
                                  sizeof(relay_port));
    if (result != APR_SUCCESS)
    {
        LL_WARNS("Proxy") << "Failed reading SOCKS UDP relay port: " << result << LL_ENDL;
        stopSOCKSProxy();
        return SOCKS_CONNECT_ERROR;
    }

    mUDPProxy.setPort(ntohs(relay_port));
    if (!mUDPProxy.isOk())
    {
        LL_WARNS("Proxy") << "SOCKS server returned an unusable UDP relay endpoint." << LL_ENDL;
        stopSOCKSProxy();
        return SOCKS_UDP_FWD_NOT_GRANTED;
    }

    LL_INFOS("Proxy") << "SOCKS 5 UDP relay connected on " << mUDPProxy << LL_ENDL;
    return SOCKS_OK;
}

/**
 * @brief Initiates a SOCKS 5 proxy session.
 *
 * Performs basic checks on host to verify that it is a valid address. Opens the control channel
 * and then negotiates the proxy connection with the server. Closes any existing SOCKS
 * connection before proceeding. Also disables an HTTP proxy if it is using SOCKS as the proxy.
 *
 *
 * @param host Socks server to connect to.
 * @return SOCKS_OK if successful, otherwise a SOCKS error code defined in llproxy.h.
 */
S32 LLProxy::startSOCKSProxy(LLHost host, U16 client_udp_port)
{
    if (host.isOk())
    {
        mTCPProxy = host;
    }
    else
    {
        return SOCKS_INVALID_HOST;
    }

    // Close any running SOCKS connection.
    stopSOCKSProxy();
    mLastSocksError.clear();

    mProxyControlChannel = tcp_open_channel(mTCPProxy, &mLastSocksError);
    if (!mProxyControlChannel)
    {
        return SOCKS_HOST_CONNECT_FAILED;
    }

    S32 status = proxyHandshake(mTCPProxy, client_udp_port);

    if (status != SOCKS_OK)
    {
        // Shut down the proxy if any of the above steps failed.
        stopSOCKSProxy();
    }
    else
    {
        // Connection was successful.
        sUDPProxyEnabled = true;
    }

    return status;
}

/**
 * @brief Stop using the SOCKS 5 proxy.
 *
 * This will stop sending UDP packets through the SOCKS 5 proxy
 * and will also stop the HTTP proxy if it is configured to use SOCKS.
 * The proxy control channel will also be disconnected.
 */
void LLProxy::stopSOCKSProxy()
{
    sUDPProxyEnabled = false;

    // If the SOCKS proxy is requested to stop and we are using that for HTTP as well
    // then we must shut down any HTTP proxy operations. But it is allowable if web
    // proxy is being used to continue proxying HTTP.

    if (LLPROXY_SOCKS == getHTTPProxyType())
    {
        disableHTTPProxy();
    }

    if (mProxyControlChannel)
    {
        tcp_close_channel(&mProxyControlChannel);
    }
}

/**
 * @brief Set the proxy's SOCKS authentication method to none.
 */
void LLProxy::setAuthNone()
{
    LLMutexLock lock(&mProxyMutex);

    mAuthMethodSelected = METHOD_NOAUTH;
}

/**
 * @brief Set the proxy's SOCKS authentication method to password.
 *
 * Check whether the lengths of the supplied username
 * and password conform to the lengths allowed by the
 * SOCKS protocol.
 *
 * @param   username The SOCKS username to send.
 * @param   password The SOCKS password to send.
 * @return  Return true if applying the settings was successful. No changes are made if false.
 *
 */
bool LLProxy::setAuthPassword(const std::string &username, const std::string &password)
{
    if (username.length() > SOCKSMAXUSERNAMELEN || password.length() > SOCKSMAXPASSWORDLEN ||
            username.length() < SOCKSMINUSERNAMELEN || password.length() < SOCKSMINPASSWORDLEN)
    {
        LL_WARNS("Proxy") << "Invalid SOCKS 5 password or username length." << LL_ENDL;
        return false;
    }

    LLMutexLock lock(&mProxyMutex);

    mAuthMethodSelected = METHOD_PASSWORD;
    mSocksUsername      = username;
    mSocksPassword      = password;

    return true;
}

/**
 * @brief Enable the HTTP proxy for either SOCKS or HTTP.
 *
 * Check the supplied host to see if it is a valid IP and port.
 *
 * @param httpHost Proxy server to connect to.
 * @param type Is the host a SOCKS or HTTP proxy.
 * @return Return true if applying the setting was successful. No changes are made if false.
 */
bool LLProxy::enableHTTPProxy(LLHost httpHost, LLHttpProxyType type)
{
    if (!httpHost.isOk())
    {
        LL_WARNS("Proxy") << "Invalid SOCKS 5 Server" << LL_ENDL;
        return false;
    }

    LLMutexLock lock(&mProxyMutex);

    mHTTPProxy        = httpHost;
    mProxyType        = type;

    mHTTPProxyEnabled = true;

    return true;
}

/**
 * @brief Enable the HTTP proxy without changing the proxy settings.
 *
 * This should not be called unless the proxy has already been set up.
 *
 * @return Return true only if the current settings are valid and the proxy was enabled.
 */
bool LLProxy::enableHTTPProxy()
{
    bool ok;

    LLMutexLock lock(&mProxyMutex);

    ok = (mHTTPProxy.isOk());
    if (ok)
    {
        mHTTPProxyEnabled = true;
    }

    return ok;
}

/**
 * @brief Disable the HTTP proxy.
 */
void LLProxy::disableHTTPProxy()
{
    LLMutexLock lock(&mProxyMutex);

    mHTTPProxyEnabled = false;
}

/**
 * @brief Get the currently selected HTTP proxy type
 */
LLHttpProxyType LLProxy::getHTTPProxyType() const
{
    LLMutexLock lock(&mProxyMutex);
    return mProxyType;
}

/**
 * @brief Get the SOCKS 5 password.
 */
std::string LLProxy::getSocksPwd() const
{
    LLMutexLock lock(&mProxyMutex);
    return mSocksPassword;
}

/**
 * @brief Get the SOCKS 5 username.
 */
std::string LLProxy::getSocksUser() const
{
    LLMutexLock lock(&mProxyMutex);
    return mSocksUsername;
}

/**
 * @brief Get the currently selected SOCKS 5 authentication method.
 *
 * @return Returns either none or password.
 */
LLSocks5AuthType LLProxy::getSelectedAuthMethod() const
{
    LLMutexLock lock(&mProxyMutex);
    return mAuthMethodSelected;
}

/**
 * @brief Stop the LLProxy and make certain that any APR pools and classes are deleted before terminating APR.
 *
 * Deletes the LLProxy singleton, destroying the APR pool used by the control channel as well as .
 */
//static
void LLProxy::cleanupClass()
{
    if (instanceExists())
    {
        getInstance()->stopSOCKSProxy();
        deleteSingleton();
    }
}

/**
 * @brief Apply proxy settings to a CuRL request if an HTTP proxy is enabled.
 *
 * This method has been designed to be safe to call from
 * any thread in the viewer.  This allows requests in the
 * texture fetch thread to be aware of the proxy settings.
 * When the HTTP proxy is enabled, the proxy mutex will
 * be locked every time this method is called.
 *
 * @param handle A pointer to a valid CURL request, before it has been performed.
 */
void LLProxy::applyProxySettings(CURL* handle)
{
    // Do a faster unlocked check to see if we are supposed to proxy.
    if (sProxyInstance && sProxyInstance->mHTTPProxyEnabled)
    {
        // We think we should proxy, lock the proxy mutex. sProxyInstance is not protected by mutex
        LLMutexLock lock(&sProxyInstance->mProxyMutex);
        // Now test again to verify that the proxy wasn't disabled between the first check and the lock.
        if (sProxyInstance->mHTTPProxyEnabled)
        {
            LLCore::LLHttp::check_curl_code(curl_easy_setopt(handle, CURLOPT_PROXY, sProxyInstance->mHTTPProxy.getIPString().c_str()), CURLOPT_PROXY);
            LLCore::LLHttp::check_curl_code(curl_easy_setopt(handle, CURLOPT_PROXYPORT, sProxyInstance->mHTTPProxy.getPort()), CURLOPT_PROXYPORT);

            if (sProxyInstance->mProxyType == LLPROXY_SOCKS)
            {
                // Resolve destination hostnames through the SOCKS server as well as proxying the connection.
                LLCore::LLHttp::check_curl_code(curl_easy_setopt(handle, CURLOPT_PROXYTYPE, CURLPROXY_SOCKS5_HOSTNAME), CURLOPT_PROXYTYPE);
                if (sProxyInstance->mAuthMethodSelected == METHOD_PASSWORD)
                {
                    std::string auth_string = sProxyInstance->mSocksUsername + ":" + sProxyInstance->mSocksPassword;
                    LLCore::LLHttp::check_curl_code(curl_easy_setopt(handle, CURLOPT_PROXYUSERPWD, auth_string.c_str()), CURLOPT_PROXYUSERPWD);
                }
            }
            else
            {
                LLCore::LLHttp::check_curl_code(curl_easy_setopt(handle, CURLOPT_PROXYTYPE, CURLPROXY_HTTP), CURLOPT_PROXYTYPE);
            }
        }
    }
}

/**
 * @brief Send one TCP packet and receive one in return.
 *
 * This operation is done synchronously with a 100ms timeout. Therefore, it should not be used when a blocking
 * operation would impact the operation of the viewer.
 *
 * @param handle_ptr    Pointer to a connected LLSocket of type STREAM_TCP.
 * @param dataout       Data to send.
 * @param outlen        Length of dataout.
 * @param datain        Buffer for received data. Undefined if return value is not APR_SUCCESS.
 * @param maxinlen      Maximum possible length of received data.  Short reads are allowed.
 * @return              Indicates APR status code of exchange. APR_SUCCESS if exchange was successful, -1 if invalid data length was received.
 */
static apr_status_t tcp_send_exact(apr_socket_t* apr_socket, const char* dataout, apr_size_t outlen)
{
    apr_size_t sent = 0;
    while (sent < outlen)
    {
        apr_size_t chunk = outlen - sent;
        const apr_status_t rv = apr_socket_send(apr_socket, dataout + sent, &chunk);
        if (rv != APR_SUCCESS)
        {
            return rv;
        }
        if (chunk == 0)
        {
            return APR_EOF;
        }
        sent += chunk;
    }
    return APR_SUCCESS;
}

static apr_status_t tcp_receive_exact(apr_socket_t* apr_socket, char* datain, apr_size_t inlen)
{
    apr_size_t received = 0;
    while (received < inlen)
    {
        apr_size_t chunk = inlen - received;
        const apr_status_t rv = apr_socket_recv(apr_socket, datain + received, &chunk);
        if (rv != APR_SUCCESS)
        {
            return rv;
        }
        if (chunk == 0)
        {
            return APR_EOF;
        }
        received += chunk;
    }
    return APR_SUCCESS;
}

/**
 * @brief Send one SOCKS control message and receive an exact-sized reply.
 *
 * SOCKS servers can be remote, and TCP is allowed to split small writes and
 * reads. Use a practical timeout and loop until the full protocol message has
 * been transferred instead of assuming one send/recv call completes it.
 */
static apr_status_t tcp_blocking_handshake(LLSocket::ptr_t handle,
                                           const char* dataout,
                                           apr_size_t outlen,
                                           char* datain,
                                           apr_size_t inlen)
{
    apr_socket_t* apr_socket = handle->getSocket();
    handle->setBlocking(5000000); // 5 seconds

    apr_status_t rv = tcp_send_exact(apr_socket, dataout, outlen);
    if (rv == APR_SUCCESS)
    {
        rv = tcp_receive_exact(apr_socket, datain, inlen);
    }

    if (rv != APR_SUCCESS)
    {
        char buf[MAX_STRING];
        LL_WARNS("Proxy") << "SOCKS control-channel exchange failed, status: "
                           << rv << " " << apr_strerror(rv, buf, MAX_STRING) << LL_ENDL;
        ll_apr_warn_status(rv);
    }

    handle->setNonBlocking();
    return rv;
}

/**
 * @brief Receive an exact-sized continuation of a SOCKS control reply.
 */
static apr_status_t tcp_blocking_receive(LLSocket::ptr_t handle, char* datain, apr_size_t inlen)
{
    apr_socket_t* apr_socket = handle->getSocket();
    handle->setBlocking(5000000); // 5 seconds

    const apr_status_t rv = tcp_receive_exact(apr_socket, datain, inlen);
    if (rv != APR_SUCCESS)
    {
        char buf[MAX_STRING];
        LL_WARNS("Proxy") << "SOCKS control-channel receive failed, status: "
                           << rv << " " << apr_strerror(rv, buf, MAX_STRING) << LL_ENDL;
        ll_apr_warn_status(rv);
    }

    handle->setNonBlocking();
    return rv;
}

/**
 * @brief Open a LLSocket and do a blocking connect to the chosen host.
 *
 * Checks for a successful connection, and makes sure the connection is closed if it fails.
 *
 * @param host      The host to open the connection to.
 * @return          The created socket.  Will evaluate as NULL if the connection is unsuccessful.
 */
static LLSocket::ptr_t tcp_open_channel(LLHost host, std::string* error_message)
{
    static const S32 SOCKS_CONNECT_TIMEOUT_US = 5000000; // 5 seconds for Internet-hosted proxies

    if (error_message)
    {
        error_message->clear();
    }

    LLSocket::ptr_t socket = LLSocket::create(NULL, LLSocket::STREAM_TCP);
    if (!socket)
    {
        if (error_message)
        {
            *error_message = "The viewer could not create the TCP socket.";
        }
        LL_WARNS("Proxy") << "Unable to create SOCKS TCP control socket." << LL_ENDL;
        return socket;
    }

    apr_status_t connect_status = APR_SUCCESS;
    const bool connected = socket->blockingConnect(host, SOCKS_CONNECT_TIMEOUT_US, &connect_status);
    if (!connected)
    {
        char status_text[MAX_STRING] = {};
        apr_strerror(connect_status, status_text, sizeof(status_text));

        if (error_message)
        {
            *error_message = llformat("%s (APR %d, OS %d)",
                                      status_text,
                                      (S32)connect_status,
                                      (S32)APR_TO_OS_ERROR(connect_status));
        }

        LL_WARNS("Proxy") << "Unable to connect to SOCKS 5 proxy TCP endpoint "
                           << host << ". APR status " << connect_status
                           << ", OS status " << APR_TO_OS_ERROR(connect_status)
                           << ": " << status_text << LL_ENDL;
        tcp_close_channel(&socket);
    }

    return socket;
}

/**
 * @brief Close the socket.
 *
 * @param handle_ptr The handle of the socket being closed. A pointer-to-pointer to avoid increasing the use count.
 */
static void tcp_close_channel(LLSocket::ptr_t* handle_ptr)
{
    LL_DEBUGS("Proxy") << "Resetting proxy LLSocket handle, use_count == " << handle_ptr->use_count() << LL_ENDL;
    handle_ptr->reset();
}
