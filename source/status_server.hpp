// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/local/stream_protocol.hpp>

namespace adhoc
{

class registry;

boost::asio::awaitable<void> run_status_server(
    boost::asio::ip::tcp::acceptor& acceptor, registry const& reg);

boost::asio::awaitable<void> run_status_server(
    boost::asio::local::stream_protocol::acceptor& acceptor,
    registry const& reg);

}  // namespace adhoc
