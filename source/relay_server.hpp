// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/local/stream_protocol.hpp>

#include "heterogeneous.hpp"

namespace adhoc
{

class relay_directory
{
public:
  enum class port_kind
  {
    pdp,
    ptp,
  };

  struct port_set
  {
    std::vector<std::uint16_t> pdp;
    std::vector<std::uint16_t> ptp;
  };

  bool add(std::string_view mac, port_kind kind, std::uint16_t port);
  void remove(std::string_view mac, port_kind kind, std::uint16_t port);

  port_set const* find(std::string_view mac) const;

private:
  std::unordered_map<std::string,
                     port_set,
                     transparent_string_hash,
                     std::equal_to<>>
      by_mac_;
};

boost::asio::awaitable<void> run_relay_server(
    boost::asio::ip::tcp::acceptor& acceptor, relay_directory& directory);

boost::asio::awaitable<void> run_relay_server(
    boost::asio::local::stream_protocol::acceptor& acceptor,
    relay_directory& directory);

}  // namespace adhoc
