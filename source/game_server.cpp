// SPDX-License-Identifier: AGPL-3.0-or-later

#include <bit>
#include <memory>
#include <string_view>

#include "game_server.hpp"

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <fmt/base.h>

#include "fwd_mov.hpp"
#include "registry.hpp"
#include "session.hpp"

using namespace std::string_view_literals;

using boost::asio::awaitable;
using boost::asio::redirect_error;
using boost::asio::use_awaitable;
using boost::asio::ip::tcp;
using boost::asio::local::stream_protocol;
using boost::system::error_code;

namespace adhoc
{

awaitable<void> run_game_server(tcp::acceptor& acceptor, registry& reg)
{
  auto ec = error_code {};

  while (acceptor.is_open()) {
    auto socket =
        co_await acceptor.async_accept(redirect_error(use_awaitable, ec));
    if (ec) {
      if (ec == boost::asio::error::operation_aborted) {
        break;
      }
      continue;
    }

    auto ep_ec = error_code {};
    let endpoint = socket.remote_endpoint(ep_ec);
    if (ep_ec) {
      socket.close(ep_ec);
      continue;
    }
    if (!reg.try_open_connection()) {
      socket.close(ep_ec);
      continue;
    }

    let address = endpoint.address();
    auto track = address.is_v4();
    auto ip_be = std::uint32_t {};
    auto label = std::string {};

    if (track) {
      let v4 = address.to_v4();
      let bytes = v4.to_bytes();
      ip_be = std::bit_cast<std::uint32_t>(bytes);
      label = v4.to_string();
    } else {
      label = address.to_string();
    }

    using Session = session_impl<tcp::socket>;
    let session_ptr = std::make_shared<Session>(MOV(socket), reg);
    let session = session_ptr.get();
    session->ip_be = ip_be;
    session->peer_label = MOV(label);

    fmt::println("New connection from {}", session->peer_label);
    session->start();
  }
}

awaitable<void> run_game_server(stream_protocol::acceptor& acceptor,
                                registry& reg)
{
  auto ec = error_code {};

  while (acceptor.is_open()) {
    auto socket =
        co_await acceptor.async_accept(redirect_error(use_awaitable, ec));
    if (ec) {
      if (ec == boost::asio::error::operation_aborted) {
        break;
      }
      continue;
    }

    if (!reg.try_open_connection()) {
      auto close_ec = error_code {};
      socket.close(close_ec);
      continue;
    }

    using Session = session_impl<stream_protocol::socket>;
    let session_ptr = std::make_shared<Session>(MOV(socket), reg);
    let session = session_ptr.get();
    session->peer_label = "unix"sv;

    fmt::println("New connection from {}", session->peer_label);
    session->start();
  }
}

}  // namespace adhoc
