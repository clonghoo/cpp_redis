#include "cpp_redis/network/tcp_client.hpp"

#include <iostream>

namespace cpp_redis {

namespace network {

boost::asio::io_service tcp_client::m_io_service;

tcp_client::tcp_client(void)
: m_socket(m_io_service)
, m_is_connected(false)
, m_read_buffer(READ_SIZE) {}

tcp_client::~tcp_client(void) {
    if (m_io_thread.joinable())
        m_io_thread.join();
}

bool
tcp_client::connect(const std::string& host, unsigned int port) {
    if (m_is_connected)
        throw tcp_client_error("Already connected");

    std::condition_variable conn_cond_var;

    //! resolve host name
    boost::asio::ip::tcp::endpoint endpoint(boost::asio::ip::address::from_string(host), port);

    //! async connect
    bool success;
    m_socket.async_connect(endpoint, [&](boost::system::error_code error) {
        success = not error;
        conn_cond_var.notify_one();

        if (not error) {
            m_is_connected = true;
            async_read();
        }
    });

    //! start loop
    m_io_thread = std::thread([this]() { m_io_service.run(); });

    //! wait for async connect result
    std::mutex conn_mutex;
    std::unique_lock<std::mutex> lock(conn_mutex);
    conn_cond_var.wait(lock);

    return success;
}

void
tcp_client::disconnect(void) {
    if (not m_is_connected)
        throw tcp_client_error("Not connected");

    m_is_connected = false;
    m_io_service.post([this]() { m_socket.close(); });

    if (m_io_thread.joinable())
        m_io_thread.join();
}

void
tcp_client::async_read(void) {
    boost::asio::async_read(m_socket, boost::asio::buffer(m_read_buffer.data(), READ_SIZE),
        [](const boost::system::error_code& error, std::size_t bytes) -> std::size_t {
            //! break if bytes have been received, continue otherwise
            return error or bytes ? 0 : READ_SIZE;
        },
        [=](boost::system::error_code error, std::size_t length) {
            if (error) {
                process_disconnection();
                return ;
            }

            std::lock_guard<std::mutex> lock(m_receive_handler_mutex);
            if (m_receive_handler)
                m_receive_handler(*this, { m_read_buffer.begin(), m_read_buffer.begin() + length });

            //! keep waiting for incoming bytes
            async_read();
        });
}

void
tcp_client::send(const std::string& buffer) {
    send(std::vector<char>{ buffer.begin(), buffer.end() });
}

void
tcp_client::send(const std::vector<char>& buffer) {
    if (not m_is_connected)
        throw tcp_client_error("Not connected");

    if (not buffer.size())
        return ;

    std::lock_guard<std::mutex> lock(m_write_buffer_mutex);

    bool bytes_in_buffer = m_write_buffer.size() > 0;

    //! concat buffer
    m_write_buffer.insert(m_write_buffer.end(), buffer.begin(), buffer.end());

    //! if there were already bytes in buffer, simply return
    //! async_write callback will process the new buffer
    if (bytes_in_buffer)
        return;

    async_write();
}

void
tcp_client::async_write(void) {
    boost::asio::async_write(m_socket, boost::asio::buffer(m_write_buffer.data(), m_write_buffer.size()),
        [this](boost::system::error_code error, std::size_t length) {
            if (error) {
                process_disconnection();
                return ;
            }

            std::lock_guard<std::mutex> lock(m_write_buffer_mutex);
            m_write_buffer.erase(m_write_buffer.begin(), m_write_buffer.begin() + length);

            if (m_write_buffer.size())
                async_write();
        });
}

void
tcp_client::process_disconnection(void) {
    m_is_connected = false;
    m_socket.close();

    std::lock_guard<std::mutex> lock(m_disconnection_handler_mutex);
    if (m_disconnection_handler)
        m_disconnection_handler(*this);
}

void
tcp_client::set_receive_handler(const receive_handler& handler) {
    std::lock_guard<std::mutex> lock(m_receive_handler_mutex);

    m_receive_handler = handler;
}

void
tcp_client::set_disconnection_handler(const disconnection_handler& handler) {
    std::lock_guard<std::mutex> lock(m_disconnection_handler_mutex);

    m_disconnection_handler = handler;
}

bool
tcp_client::is_connected(void) {
    return m_is_connected;
}

} //! network

} //! cpp_redis
