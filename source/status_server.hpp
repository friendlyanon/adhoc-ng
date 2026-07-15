// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/local/stream_protocol.hpp>

namespace adhoc
{

class registry;
class relay_directory;

boost::asio::awaitable<void> run_status_server(
    boost::asio::ip::tcp::acceptor& acceptor,
    registry const& reg,
    relay_directory const& relay);

boost::asio::awaitable<void> run_status_server(
    boost::asio::local::stream_protocol::acceptor& acceptor,
    registry const& reg,
    relay_directory const& relay);

}  // namespace adhoc
