// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/steady_timer.hpp>

#include "protocol.hpp"

namespace adhoc
{

class registry;
class product_db;
struct game_node;
struct group_node;

class user_session
{
public:
  virtual ~user_session() = default;

  virtual void send_bytes(std::vector<std::byte> bytes) = 0;

  virtual void close() = 0;

  // Set by the accept code.
  bool tracks_address = false;
  std::uint32_t address_v4_host = 0;  // only valid if tracks_address
  std::uint32_t ip_be = 0;  // network-order IPv4 for wire packets
  std::string peer_label;  // for logs

  // Populated by registry on successful LOGIN.
  ether_addr mac {};
  nickname name {};
  game_node* game = nullptr;
  group_node* group = nullptr;

  // Set to true in registry::close_connection.
  bool reaped = false;
};

template<class T>
  requires std::is_trivial_v<T> && std::is_standard_layout_v<T>
inline auto packet_bytes(T const& pkt)
{
  auto out = std::vector<std::byte>(sizeof(T));
  std::memcpy(out.data(), &pkt, sizeof(T));
  return out;
}

template<class Socket>
class session_impl
    : public user_session
    , public std::enable_shared_from_this<session_impl<Socket>>
{
public:
  session_impl(Socket socket, registry& reg);

  void start();

  void send_bytes(std::vector<std::byte> bytes) override;
  void close() override;

private:
  using clock = std::chrono::steady_clock;

  boost::asio::awaitable<void> reader();
  boost::asio::awaitable<void> writer();
  boost::asio::awaitable<void> watchdog();

  void consume_rx(std::size_t n);

  Socket socket_;
  boost::asio::steady_timer timer_;  // last-recv timeout
  boost::asio::steady_timer write_signal_;  // wake-up for writer
  registry* registry_;

  std::uint8_t rx_[1024] {};
  std::size_t rxpos_ = 0;

  std::deque<std::vector<std::byte>> write_queue_;
  bool closed_ = false;
};

extern template class session_impl<boost::asio::ip::tcp::socket>;
extern template class session_impl<boost::asio::local::stream_protocol::socket>;

}  // namespace adhoc
