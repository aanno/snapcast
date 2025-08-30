/***
    This file is part of snapcast
    Copyright (C) 2014-2025  Johannes Pohl

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
***/

#include "stream_session_tcp_zerocopy.hpp"
#include "common/aixlog.hpp"

// 3rd party headers
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

// standard headers
#include <iostream>

using namespace std;
using namespace streamreader;

static constexpr auto LOG_TAG = "StreamSessionTcpZeroCopy";

StreamSessionTcpZeroCopy::StreamSessionTcpZeroCopy(StreamMessageReceiver* receiver, const ServerSettings& server_settings, tcp::socket&& socket)
    : StreamSession(socket.get_executor(), server_settings, receiver), zerocopy_socket_(std::move(socket))
{
    if (zerocopy_socket_.isZeroCopyEnabled())
    {
        LOG(INFO, LOG_TAG) << "Zerocopy enabled for client connection\n";
    }
    else
    {
        LOG(DEBUG, LOG_TAG) << "Zerocopy not available, using regular TCP\n";
    }
}

StreamSessionTcpZeroCopy::~StreamSessionTcpZeroCopy()
{
    LOG(DEBUG, LOG_TAG) << "~StreamSessionTcpZeroCopy\n";
    stop();
}

void StreamSessionTcpZeroCopy::start()
{
    readNext();
}

void StreamSessionTcpZeroCopy::stop()
{
    LOG(DEBUG, LOG_TAG) << "stop\n";
    if (zerocopy_socket_.is_open())
    {
        boost::system::error_code ec;
        zerocopy_socket_.shutdown(tcp::socket::shutdown_both, ec);
        if (ec)
            LOG(ERROR, LOG_TAG) << "Error in socket shutdown: " << ec.message() << "\n";
        zerocopy_socket_.close(ec);
        if (ec)
            LOG(ERROR, LOG_TAG) << "Error in socket close: " << ec.message() << "\n";
        LOG(DEBUG, LOG_TAG) << "stopped\n";
    }
}

std::string StreamSessionTcpZeroCopy::getIP()
{
    try
    {
        return zerocopy_socket_.remote_endpoint().address().to_string();
    }
    catch (...)
    {
        return "0.0.0.0";
    }
}

void StreamSessionTcpZeroCopy::readNext()
{
    zerocopy_socket_.async_read_some(boost::asio::buffer(buffer_, base_msg_size_),
        [this, self = shared_from_this()](boost::system::error_code ec, std::size_t length) mutable
        {
            if (ec)
            {
                LOG(ERROR, LOG_TAG) << "Error reading message header of length " << length << ": " << ec.message() << "\n";
                messageReceiver_->onDisconnect(this);
                return;
            }

            baseMessage_.deserialize(buffer_.data());
            LOG(DEBUG, LOG_TAG) << "getNextMessage: " << baseMessage_.type << ", size: " << baseMessage_.size 
                                << ", id: " << baseMessage_.id << ", refers: " << baseMessage_.refersTo << "\n";
            
            if (baseMessage_.type > message_type::kLast)
            {
                LOG(ERROR, LOG_TAG) << "unknown message type received: " << baseMessage_.type 
                                    << ", size: " << baseMessage_.size << "\n";
                messageReceiver_->onDisconnect(this);
                return;
            }
            else if (baseMessage_.size > msg::max_size)
            {
                LOG(ERROR, LOG_TAG) << "received message of type " << baseMessage_.type 
                                    << " too large: " << baseMessage_.size << "\n";
                messageReceiver_->onDisconnect(this);
                return;
            }

            if (baseMessage_.size > buffer_.size())
                buffer_.resize(baseMessage_.size);

            // Read the message body using boost::asio::async_read for complete read
            boost::asio::async_read(zerocopy_socket_, boost::asio::buffer(buffer_, baseMessage_.size), 
                [this, self](boost::system::error_code ec, std::size_t length) mutable
                {
                    if (ec)
                    {
                        LOG(ERROR, LOG_TAG) << "Error reading message body of length " << length 
                                            << ": " << ec.message() << "\n";
                        messageReceiver_->onDisconnect(this);
                        return;
                    }

                    tv now;
                    baseMessage_.received = now;
                    if (messageReceiver_ != nullptr)
                        messageReceiver_->onMessageReceived(shared_from_this(), baseMessage_, buffer_.data());
                    readNext();
                });
        });
}

void StreamSessionTcpZeroCopy::sendAsync(const shared_const_buffer& buffer, WriteHandler&& handler)
{
    if (zerocopy_socket_.isZeroCopyEnabled())
    {
        // Use zerocopy for PCM chunks which are typically large
        if (buffer.message().is_pcm_chunk && buffer.message().data.size() > 1024)
        {
            auto zerocopy_buffer = extractBuffer(buffer);
            zerocopy_socket_.async_send_zerocopy(zerocopy_buffer,
                [self = shared_from_this(), buffer, handler = std::move(handler)]
                (boost::system::error_code ec, std::size_t length)
                {
                    if (handler)
                        handler(ec, length);
                });
            return;
        }
    }

    // Fallback to regular async_write for small messages or if zerocopy failed
    zerocopy_socket_.async_write_some(buffer,
        [self = shared_from_this(), buffer, handler = std::move(handler)]
        (boost::system::error_code ec, std::size_t length)
        {
            if (handler)
                handler(ec, length);
        });
}

std::shared_ptr<std::vector<char>> StreamSessionTcpZeroCopy::extractBuffer(const shared_const_buffer& buffer)
{
    // Extract the data from shared_const_buffer into a new vector
    // This is necessary because shared_const_buffer uses boost::asio::const_buffer
    const auto& msg_data = buffer.message().data;
    return std::make_shared<std::vector<char>>(msg_data.begin(), msg_data.end());
}