#include "connection.h"

#include "error.h"
#include "logger.h"
#include "utils.h"

#include <boost/lexical_cast.hpp>

#define ERRLOG(level) LOG(CerrWriter, level)

using namespace MADF;
using Caster::Connection;
using Caster::parseChunkLength;

namespace pls = std::placeholders;
namespace bs = boost::system;
namespace ba = boost::asio;

namespace {

using StringPair = std::pair<std::string, std::string>;

StringPair splitString(const std::string& src, char delimiter);
StringPair trimStringPair(const StringPair& pair);

}

Connection::Connection(ba::io_service& ioService,
                       const std::string& server, uint16_t port)
    : m_server(server),
      m_port(port),
      m_uri("/"),
      m_timeout(0),
      m_socket(ioService),
      m_timeouter(ioService),
      m_resolver(ioService),
      m_response(1024),
      m_chunked(false),
      m_active(false)
{
}

Connection::Connection(ba::io_service& ioService,
                       const std::string& server, uint16_t port,
                       const std::string& mountpoint)
    : m_server(server),
      m_port(port),
      m_timeout(0),
      m_socket(ioService),
      m_timeouter(ioService),
      m_resolver(ioService),
      m_response(1024),
      m_chunked(false),
      m_active(false)
{
    if (mountpoint[0] != '/')
        m_uri = "/";
    m_uri += mountpoint;
}

void Connection::start()
{
    tcp::resolver::query query(m_server, boost::lexical_cast<std::string>(m_port));
    m_resolver.async_resolve(
            query,
            std::bind(
                &Connection::m_handleResolve,
                shared_from_this(),
                pls::_1,
                pls::_2
            )
    );
}

void Connection::start(unsigned timeout)
{
    m_timeout = timeout;
    start();
    restartTimer();
}

void Connection::setCredentials(const std::string& login,
                                const std::string& password)
{
    m_auth = Authenticator(login, password);
}

void Connection::m_handleResolve(const bs::error_code& error,
                                 tcp::resolver::iterator it)
{
    restartTimer();
    if (!error) {
        if (it != tcp::resolver::iterator()) {
            m_socket.async_connect(
                *it,
                std::bind(
                    &Connection::m_handleConnect,
                    shared_from_this(),
                    pls::_1,
                    it
                )
            );
        } else {
            if (m_errorCallback)
                m_errorCallback(
                    bs::error_code(
                        resolveError,
                        CasterCategory::getInstance()
                    )
                );
            m_shutdown();
        }
    } else {
        if (m_errorCallback)
            m_errorCallback(error);
        m_shutdown();
    }
}

void Connection::m_handleConnect(const bs::error_code& error,
                                 tcp::resolver::iterator it)
{
    restartTimer();
    if (!error) {
        m_prepareRequest();
        ba::async_write(
            m_socket,
            m_request,
            ba::transfer_all(),
            std::bind(
                &Connection::m_handleWriteRequest,
                shared_from_this(),
                pls::_1
            )
        );
    } else {
        ++it;
        if (it != tcp::resolver::iterator()) {
            m_socket.async_connect(
                *it,
                std::bind(
                    &Connection::m_handleConnect,
                    shared_from_this(),
                    pls::_1,
                    it
                )
            );
        } else {
            if (m_errorCallback)
                m_errorCallback(error);
            m_shutdown();
        }
    }
}

void Connection::m_handleWriteRequest(const bs::error_code& error)
{
    restartTimer();
    if (!error) {
        ba::async_read_until(
            m_socket,
            m_response,
            "\r\n",
            std::bind(
                &Connection::m_handleReadStatus,
                shared_from_this(),
                pls::_1
            )
        );
    } else if (error != ba::error::operation_aborted) {
        if (m_errorCallback)
            m_errorCallback(error);
        m_shutdown();
    }
}

void Connection::m_handleWriteData(const bs::error_code& error)
{
    restartTimer();
    if (error && error != ba::error::operation_aborted) {
        if (m_errorCallback)
            m_errorCallback(error);
        m_shutdown();
    }
}

void Connection::m_handleReadStatus(const bs::error_code& error)
{
    restartTimer();
    if (!error) {
        std::istream statusStream(&m_response);
        std::string proto;
        statusStream >> proto;
        unsigned code;
        statusStream >> code;
        std::string message;
        std::getline(statusStream, message);
        if (code != 200) {
            ERRLOG(logError) << "Invalid status string:\n"
                          << proto << " " << code << " " << message;
            if (m_errorCallback)
                m_errorCallback(
                    bs::error_code(
                        invalidStatus,
                        CasterCategory::getInstance()
                    )
                );
            m_shutdown();
            return;
        }
        if (proto == "ICY") {
            m_active = true;
            ba::async_read(
                m_socket,
                m_response,
                std::bind(
                    &Connection::m_handleReadData,
                    shared_from_this(),
                    pls::_1
                )
            );
        } else {
            ba::async_read_until(
                m_socket,
                m_response,
                "\r\n\r\n",
                std::bind(
                    &Connection::m_handleReadHeaders,
                    shared_from_this(),
                    pls::_1
                )
            );
        }
    } else if (error != ba::error::operation_aborted) {
        if (m_errorCallback)
            m_errorCallback(error);
        m_shutdown();
    }
}

void Connection::m_handleReadHeaders(const bs::error_code& error)
{
    restartTimer();
    if (!error) {
        std::istream headersStream(&m_response);
        std::string header;
        while (std::getline(headersStream, header) && header != "\r") {
            const StringPair pair(trimStringPair(splitString(header, ':')));
            m_headers.insert(pair);
            if (pair.first == "Transfer-Encoding" &&
                pair.second == "chunked") {
                ERRLOG(logDebug) << "Transfer-Encoding: chunked";
                m_chunked = true;
            }
        }
        if (m_headersCallback)
            m_headersCallback();
        m_active = true;
        if (m_chunked) {
            ba::async_read_until(
                m_socket,
                m_response,
                "\r\n",
                std::bind(
                    &Connection::m_handleReadChunkLength,
                    shared_from_this(),
                    pls::_1
                )
            );
        } else {
            ba::async_read(
                m_socket,
                m_response,
                std::bind(
                    &Connection::m_handleReadData,
                    shared_from_this(),
                    pls::_1
                )
            );
        }
    } else if (error != ba::error::operation_aborted) {
        if (m_errorCallback)
            m_errorCallback(error);
        m_shutdown();
    }
}

void Connection::m_handleReadData(const bs::error_code& error)
{
    restartTimer();

    if (m_response.size()) {
        if (m_dataCallback)
            m_dataCallback(m_response.data());
        m_response.consume(m_response.size());
    }

    if (!error) {
        ba::async_read(
            m_socket,
            m_response,
            ba::transfer_at_least(1),
            std::bind(
                &Connection::m_handleReadData,
                shared_from_this(),
                pls::_1
            )
        );
    } else if (error == ba::error::eof) {
        if (m_eofCallback)
            m_eofCallback();
        m_shutdown();
    } else if (error != ba::error::operation_aborted) {
        if (m_errorCallback)
            m_errorCallback(error);
        m_shutdown();
    }
}

void Connection::m_handleReadChunkLength(const bs::error_code& error)
{
    restartTimer();

    if (!error) {
        size_t length = 0;
        m_response.consume(parseChunkLength(m_response.data(), length));
        if (length == 0) {
            if (m_eofCallback)
                m_eofCallback();
            m_shutdown();
        } else {
            if (m_response.size() < length + 2) {
                ba::async_read(
                    m_socket,
                    m_response,
                    ba::transfer_at_least(length + 2 - m_response.size()),
                    std::bind(
                        &Connection::m_handleReadChunkData,
                        shared_from_this(),
                        pls::_1,
                        length
                    )
                );
            } else {
                m_handleReadChunkData(bs::error_code(), length);
            }
        }
    } else if (error == ba::error::eof) {
        if (m_eofCallback)
            m_eofCallback();
        m_shutdown();
    } else if (error != ba::error::operation_aborted) {
        if (m_errorCallback)
            m_errorCallback(error);
        m_shutdown();
    }
}

void Connection::m_handleReadChunkData(const bs::error_code& error,
                                       size_t size)
{
    restartTimer();

    if (size > 0) {
        if (size > m_response.size()) {
            if (m_dataCallback)
                m_dataCallback(m_response.data());
        } else {
            if (m_dataCallback)
                m_dataCallback(buffer(m_response.data(), size));
        }
    }
    if (!error) {
        if (size > m_response.size()) {
            const size_t remainder = size + 2 - m_response.size();
            m_response.consume(m_response.size());
            ba::async_read(
                m_socket,
                m_response,
                ba::transfer_at_least(remainder),
                std::bind(
                    &Connection::m_handleReadChunkData,
                    shared_from_this(),
                    pls::_1,
                    remainder - 2
                )
            );
        } else {
            m_response.consume(size + 2);
            ba::async_read_until(
                m_socket,
                m_response,
                "\r\n",
                std::bind(
                    &Connection::m_handleReadChunkLength,
                    shared_from_this(),
                    pls::_1
                )
            );
        }
    } else if (error == ba::error::eof) {
        if (m_eofCallback)
            m_eofCallback();
        m_shutdown();
    } else if (error != ba::error::operation_aborted) {
        if (m_errorCallback)
            m_errorCallback(error);
        m_shutdown();
    }
}

void Connection::m_shutdown()
{
    m_active = false;
    if (!m_socket.is_open())
        return;
    bs::error_code ec;
    m_socket.shutdown(tcp::socket::shutdown_both, ec);
    m_socket.close(ec);
}

void Connection::handleTimeout(const bs::error_code& ec)
{
    if (ec == ba::error::operation_aborted)
        return;
    if (!m_socket.is_open())
        return;

    // Check whether the deadline has passed. We compare the deadline against
    // the current time since a new asynchronous operation may have moved the
    // deadline before this actor had a chance to run.
    if (m_timeouter.expires_at() > ba::deadline_timer::traits_type::now())
        m_timeouter.async_wait(std::bind(&Connection::handleTimeout, shared_from_this(), pls::_1));

    ERRLOG(logInfo) << "Connection timeout detected, shutting it down" << std::endl;
    if (m_errorCallback)
        m_errorCallback(
            bs::error_code(
                connectionTimeout,
                CasterCategory::getInstance()
            )
        );
    m_shutdown();
    return;
}

void Connection::restartTimer()
{
    if (!m_timeout)
        return;

    m_timeouter.expires_from_now(boost::posix_time::seconds(m_timeout));
    m_timeouter.async_wait(std::bind(&Connection::handleTimeout, shared_from_this(), pls::_1));
}

namespace {

inline
StringPair splitString(const std::string& src, const char delimiter)
{
    const size_t pos = src.find_first_of(delimiter);
    if (pos != std::string::npos) {
        return std::make_pair(src.substr(0, pos), src.substr(pos + 1));
    } else {
        return std::make_pair(src, "");
    }
}

inline
StringPair trimStringPair(const StringPair& pair)
{
    const size_t lpos = pair.second.find_first_not_of(" \t");
    const size_t rpos = pair.second.find_last_not_of(" \t\r\n", std::string::npos, 4);
    return std::make_pair(pair.first, pair.second.substr(lpos, rpos - lpos + 1));
}

}
