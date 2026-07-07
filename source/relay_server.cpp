// SPDX-License-Identifier: AGPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "relay_server.hpp"

#include <boost/asio/cancel_after.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>
#include <fmt/format.h>

#include "fwd_mov.hpp"
#include "heterogeneous.hpp"

namespace chrono = std::chrono;

using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::redirect_error;
using boost::asio::use_awaitable;
using boost::asio::ip::tcp;
using boost::system::error_code;

using u16 = std::uint16_t;

namespace adhoc
{

namespace
{

constexpr let AEMU_POSTOFFICE_PDP_BLOCK_MAX = 10u * 1024u;
constexpr let AEMU_POSTOFFICE_PTP_BLOCK_MAX = 50u * 1024u;

enum aemu_postoffice_init_type : std::int32_t
{
  AEMU_POSTOFFICE_INIT_PDP,
  AEMU_POSTOFFICE_INIT_PTP_LISTEN,
  AEMU_POSTOFFICE_INIT_PTP_CONNECT,
  AEMU_POSTOFFICE_INIT_PTP_ACCEPT,
};

#pragma pack(push, 1)
struct aemu_string
{
  char data[8];
};

struct aemu_postoffice_init
{
  std::int32_t init_type;
  aemu_string src_addr;
  u16 sport;
  aemu_string dst_addr;
  u16 dport;
};

struct aemu_postoffice_pdp
{
  aemu_string addr;
  u16 port;
  std::uint32_t size;
};

struct aemu_postoffice_ptp_connect
{
  aemu_string addr;
  u16 port;
};

struct aemu_postoffice_ptp_data
{
  std::uint32_t size;
};
#pragma pack(pop)

constexpr let mac_size = 6;

template<std::size_t N>
  requires(N == mac_size)
aemu_string to_str(char const (&mac)[N])
{
  auto s = aemu_string {};
  std::memcpy(s.data, mac, mac_size);
  return s;
}

std::string to_bytes(let& x)
{
  let bytes = std::bit_cast<std::array<char, sizeof(x)>>(x);
  return {bytes.data(), bytes.size()};
}

void write_to(void* dst, let& src)
{
  std::memcpy(dst, &src, sizeof(src));
}

constexpr std::uint64_t session_init_time_limit_ms = 5000;
constexpr std::uint64_t data_queue_size_limit_byte = 512000;
constexpr std::uint64_t connect_time_limit_ms = 5000;
constexpr int max_num_sessions = 5000;

enum class aemu_session_mode
{
  PDP,
  PTP_LISTEN,
  PTP_CONNECT,
  PTP_ACCEPT,
};

template<class T>
using relay_string_map = std::unordered_map<  //
    std::string,
    T,
    adhoc::transparent_string_hash,
    std::equal_to<>>;

template<class Socket>
std::string describe_peer(Socket& socket)
{
  auto peer_ec = error_code {};
  auto endpoint = socket.remote_endpoint(peer_ec);
  if (peer_ec) {
    return {"unknown"};
  }

  if constexpr (std::is_same_v<typename Socket::protocol_type, tcp>) {
    return fmt::format(
        "{}:{}", endpoint.address().to_string(), endpoint.port());
  } else {
    return endpoint.path();
  }
}

auto as_u8(auto x)
{
  return static_cast<std::uint8_t>(x);
}

struct mac
{
  char str[17];

  template<std::size_t N>
    requires(N >= mac_size)
  mac(char const (&in)[N])
  {
    fmt::format_to(str,
                   "{:02x}:{:02x}:{:02x}:{:02x}:{:02x}:{:02x}",
                   as_u8(in[0]),
                   as_u8(in[1]),
                   as_u8(in[2]),
                   as_u8(in[3]),
                   as_u8(in[4]),
                   as_u8(in[5]));
  }

  auto view() const { return std::string_view(str, sizeof(str)); }
};

std::string get_listen_session_name(mac mac, u16 port)
{
  return fmt::format("PTP_LISTEN {} {}", mac.view(), port);
}

std::string get_connect_session_name(mac src, u16 sport, mac dst, u16 dport)
{
  return fmt::format(
      "PTP_CONNECT {} {} {} {}", src.view(), sport, dst.view(), dport);
}

std::string get_accept_session_name(mac src, u16 sport, mac dst, u16 dport)
{
  return fmt::format(
      "PTP_ACCEPT {} {} {} {}", src.view(), sport, dst.view(), dport);
}

std::string get_pdp_session_name(mac mac, u16 port)
{
  return fmt::format("PDP {} {}", mac.view(), port);
}

template<class Socket>
class relay_session;

template<class Socket>
class relay_registry
{
public:
  using session = relay_session<Socket>;
  using session_ptr = std::shared_ptr<session>;
  using executor_type = typename Socket::executor_type;

  explicit relay_registry(executor_type executor)
      : executor_(MOV(executor))
  {
  }

  session_ptr find(std::string_view name)
  {
    let it = sessions_.find(name);
    return it == sessions_.end() ? session_ptr {} : it->second;
  }

  void add(std::string const& name, session_ptr s)
  {
    if (let it = sessions_.find(name); it != sessions_.end()) {
      fmt::println("replacing session {} from {} by session from {}",
                   name,
                   it->second->client_addr(),
                   s->client_addr());
      it->second->supersede();
    }

    sessions_.insert_or_assign(name, MOV(s));

    if (let w = waiters_.find(name); w != waiters_.end()) {
      for (let& timer : w->second) {
        timer->cancel();
      }
      waiters_.erase(w);
    }
  }

  void remove(std::string_view name, session const* who)
  {
    let it = sessions_.find(name);
    if (it != sessions_.end() && it->second.get() == who) {
      sessions_.erase(it);
    }
  }

  std::size_t active() const { return active_count_; }
  void inc_active() { ++active_count_; }
  void dec_active()
  {
    if (active_count_ > 0) {
      --active_count_;
    }
  }

  awaitable<session_ptr> await_session(std::string const& name,
                                       chrono::milliseconds timeout)
  {
    if (let found = find(name)) {
      co_return found;
    }
    if (timeout <= chrono::milliseconds::zero()) {
      co_return session_ptr {};
    }

    auto timer = std::make_shared<boost::asio::steady_timer>(executor_);
    timer->expires_after(timeout);
    waiters_[name].push_back(timer);

    auto ec = error_code {};
    co_await timer->async_wait(redirect_error(use_awaitable, ec));

    if (let w = waiters_.find(name); w != waiters_.end()) {
      auto& vec = w->second;
      std::erase(vec, timer);
      if (vec.empty()) {
        waiters_.erase(w);
      }
    }

    co_return find(name);
  }

private:
  executor_type executor_;
  relay_string_map<session_ptr> sessions_;
  relay_string_map<std::vector<std::shared_ptr<boost::asio::steady_timer>>>
      waiters_;
  std::size_t active_count_ = 0;
};

template<class Socket>
class relay_session : public std::enable_shared_from_this<relay_session<Socket>>
{
  template<class S>
  friend class relay_session;

public:
  relay_session(Socket socket,
                std::string client_addr,
                std::shared_ptr<relay_registry<Socket>> registry)
      : socket_(MOV(socket))
      , write_signal_(socket_.get_executor())
      , bond_signal_(socket_.get_executor())
      , registry_(MOV(registry))
      , client_addr_(MOV(client_addr))
      , create_time_(chrono::steady_clock::now())
  {
    fmt::println("{} connecting", client_addr_);
  }

  void start()
  {
    auto self = this->shared_from_this();

    auto run_ = [self]() -> awaitable<void> { co_await self->run(); };
    auto writer_ = [self]() -> awaitable<void> { co_await self->writer(); };

    co_spawn(socket_.get_executor(), MOV(run_), detached);
    co_spawn(socket_.get_executor(), MOV(writer_), detached);
  }

  std::string_view client_addr() const { return client_addr_; }
  bool established() const { return established_; }

  void queue_send(std::string data)
  {
    if (closed_ || data.empty()) {
      return;
    }
    queued_bytes_ += data.size();
    write_queue_.push_back(MOV(data));
    write_signal_.cancel_one();

    if (queued_bytes_ >= data_queue_size_limit_byte) {
      fmt::println(
          "session {} from {} has reached receive data buffer limit {}b",
          identifier_,
          client_addr_,
          data_queue_size_limit_byte);
      close();
    }
  }

  void bond()
  {
    established_ = true;
    bond_signal_.cancel();
  }

  void supersede()
  {
    superseded_ = true;
    close();
  }

  void close()
  {
    if (closed_) {
      return;
    }
    closed_ = true;
    auto ec = error_code {};
    socket_.close(ec);
    write_signal_.cancel();
    bond_signal_.cancel();
  }

private:
  awaitable<void> run()
  {
    auto self = this->shared_from_this();
    auto init = aemu_postoffice_init {};

    if (!co_await read_init(init)) {
      finish();
      co_return;
    }

    switch (init.init_type) {
      case AEMU_POSTOFFICE_INIT_PDP:
        mode_ = aemu_session_mode::PDP;
        set_endpoints(init.src_addr, init.sport, nullptr, 0);
        register_self(self);
        break;

      case AEMU_POSTOFFICE_INIT_PTP_LISTEN:
        mode_ = aemu_session_mode::PTP_LISTEN;
        set_endpoints(init.src_addr, init.sport, nullptr, 0);
        register_self(self);
        break;

      case AEMU_POSTOFFICE_INIT_PTP_CONNECT:
        mode_ = aemu_session_mode::PTP_CONNECT;
        set_endpoints(init.src_addr, init.sport, &init.dst_addr, init.dport);
        if (!co_await establish_connect(self, init)) {
          finish();
          co_return;
        }
        break;

      case AEMU_POSTOFFICE_INIT_PTP_ACCEPT:
        mode_ = aemu_session_mode::PTP_ACCEPT;
        set_endpoints(init.src_addr, init.sport, &init.dst_addr, init.dport);
        if (!establish_accept(self, init)) {
          finish();
          co_return;
        }
        break;

      default:
        fmt::println(
            "unknown init type {} from {}", init.init_type, client_addr_);
        finish();
        co_return;
    }

    co_await forward_loop();
    finish();
  }

  void register_self(std::shared_ptr<relay_session>& self)
  {
    identifier_ = get_identifier();
    registry_->add(identifier_, self);
    registered_ = true;
    fmt::println("created session {} for {}", identifier_, client_addr_);
  }

  awaitable<bool> establish_connect(std::shared_ptr<relay_session>& self,
                                    aemu_postoffice_init const& init)
  {
    let listen_name = get_listen_session_name(init.dst_addr.data, init.dport);
    auto listen =
        co_await registry_->await_session(listen_name, remaining_init_time());
    if (!listen) {
      fmt::println(
          "refusing client {}: missing listen session {} during ptp connect "
          "creation",
          client_addr_,
          listen_name);
      co_return false;
    }

    established_ = false;
    register_self(self);

    listen->queue_send(
        to_bytes(aemu_postoffice_ptp_connect {to_str(from_mac_), from_port_}));

    bond_signal_.expires_after(chrono::milliseconds(connect_time_limit_ms));
    auto ec = error_code {};
    co_await bond_signal_.async_wait(redirect_error(use_awaitable, ec));
    if (!established_) {
      fmt::println("ptp connect session {} timed out waiting for accept",
                   identifier_);
      co_return false;
    }
    co_return true;
  }

  bool establish_accept(std::shared_ptr<relay_session>& self,
                        aemu_postoffice_init const& init)
  {
    let connect_name = get_connect_session_name(
        init.dst_addr.data, init.dport, init.src_addr.data, init.sport);
    auto connect = registry_->find(connect_name);
    if (!connect || connect->established()) {
      fmt::println(
          "peer session {} not found, not creating ptp accept session for {}",
          connect_name,
          client_addr_);
      return false;
    }

    established_ = true;
    register_self(self);

    using Pkt = aemu_postoffice_ptp_connect;

    connect->queue_send(to_bytes(Pkt {to_str(from_mac_), from_port_}));
    queue_send(to_bytes(Pkt {to_str(connect->from_mac_), connect->from_port_}));

    fmt::println("bonding {} with {}", connect->identifier_, identifier_);
    connect->bond();
    return true;
  }

  awaitable<void> forward_loop()
  {
    if (mode_ == aemu_session_mode::PTP_LISTEN) {
      auto buf = std::array<char, 1024> {};
      auto ec = error_code {};
      while (!closed_) {
        co_await socket_.async_read_some(boost::asio::buffer(buf),
                                         redirect_error(use_awaitable, ec));
        if (ec) {
          break;
        }
      }
      co_return;
    }

    if (mode_ == aemu_session_mode::PDP) {
      while (!closed_) {
        auto header = aemu_postoffice_pdp {};
        if (!co_await read_exact(&header, sizeof(header))) {
          break;
        }
        let target = get_pdp_session_name(header.addr.data, header.port);
        let size = header.size;
        if (size > AEMU_POSTOFFICE_PDP_BLOCK_MAX * 2u) {
          fmt::println("session {} from {} sent an oversized pdp block",
                       identifier_,
                       client_addr_);
          break;
        }

        auto payload = std::string(sizeof(aemu_postoffice_pdp) + size, '\0');
        if (size > 0
            && !co_await read_exact(
                payload.data() + sizeof(aemu_postoffice_pdp), size))
        {
          break;
        }

        if (auto dst = registry_->find(target)) {
          write_to(payload.data(),
                   aemu_postoffice_pdp {to_str(from_mac_), from_port_, size});
          dst->queue_send(MOV(payload));
        }
      }
      co_return;
    }

    while (!closed_) {
      auto header = aemu_postoffice_ptp_data {};
      if (!co_await read_exact(&header, sizeof(header))) {
        break;
      }
      let size = header.size;
      if (size > AEMU_POSTOFFICE_PTP_BLOCK_MAX * 2u) {
        fmt::println("session {} from {} sent an oversized ptp block",
                     identifier_,
                     client_addr_);
        break;
      }

      auto payload = std::string(sizeof(aemu_postoffice_ptp_data) + size, '\0');
      if (size > 0
          && !co_await read_exact(
              payload.data() + sizeof(aemu_postoffice_ptp_data), size))
      {
        break;
      }

      if (auto dst = registry_->find(get_peer_identifier())) {
        write_to(payload.data(), aemu_postoffice_ptp_data {size});
        dst->queue_send(MOV(payload));
      }
    }
  }

  awaitable<void> writer()
  {
    auto ec = error_code {};
    while (!closed_) {
      if (write_queue_.empty()) {
        write_signal_.expires_at(chrono::steady_clock::time_point::max());
        co_await write_signal_.async_wait(redirect_error(use_awaitable, ec));
        ec = {};
        continue;
      }

      auto bytes = MOV(write_queue_.front());
      write_queue_.pop_front();
      queued_bytes_ -= bytes.size();

      co_await boost::asio::async_write(socket_,
                                        boost::asio::buffer(bytes),
                                        redirect_error(use_awaitable, ec));
      if (ec) {
        close();
        break;
      }
    }
  }

  awaitable<bool> read_init(aemu_postoffice_init& init)
  {
    auto ec = error_code {};
    let n = co_await boost::asio::async_read(
        socket_,
        boost::asio::buffer(&init, sizeof(init)),
        boost::asio::cancel_after(
            chrono::milliseconds(session_init_time_limit_ms),
            redirect_error(use_awaitable, ec)));

    if (ec == boost::asio::error::operation_aborted) {
      fmt::println("session creation for {} timed out", client_addr_);
      co_return false;
    }
    if (ec || n != sizeof(init)) {
      co_return false;
    }
    co_return true;
  }

  awaitable<bool> read_exact(void* dst, std::size_t n)
  {
    auto ec = error_code {};
    co_await boost::asio::async_read(socket_,
                                     boost::asio::buffer(dst, n),
                                     redirect_error(use_awaitable, ec));
    co_return !ec;
  }

  void finish()
  {
    if (finished_) {
      return;
    }
    finished_ = true;
    close();

    if (registered_ && !superseded_) {
      fmt::println("removing {} of {}", identifier_, client_addr_);
      registry_->remove(identifier_, this);

      let peer = get_peer_identifier();
      if (!peer.empty()) {
        if (auto p = registry_->find(peer)) {
          fmt::println(
              "removing {} of {} by peer relation", peer, p->client_addr());
          p->close();
        }
      }
    }

    registry_->dec_active();
  }

  void set_endpoints(aemu_string const& from_mac,
                     u16 from_port,
                     aemu_string const* to_mac,
                     u16 to_port)
  {
    std::memcpy(from_mac_, from_mac.data, sizeof(from_mac_));
    from_port_ = from_port;
    if (to_mac != nullptr) {
      std::memcpy(to_mac_, to_mac->data, sizeof(to_mac_));
    }
    to_port_ = to_port;
  }

  chrono::milliseconds remaining_init_time() const
  {
    let elapsed = chrono::duration_cast<chrono::milliseconds>(
        chrono::steady_clock::now() - create_time_);
    let limit = chrono::milliseconds(session_init_time_limit_ms);
    return elapsed >= limit ? chrono::milliseconds::zero() : limit - elapsed;
  }

  std::string get_identifier() const
  {
    switch (mode_) {
      case aemu_session_mode::PDP:
        return get_pdp_session_name(from_mac_, from_port_);
      case aemu_session_mode::PTP_LISTEN:
        return get_listen_session_name(from_mac_, from_port_);
      case aemu_session_mode::PTP_CONNECT:
        return get_connect_session_name(
            from_mac_, from_port_, to_mac_, to_port_);
      case aemu_session_mode::PTP_ACCEPT:
        return get_accept_session_name(
            from_mac_, from_port_, to_mac_, to_port_);
    }
    return {};
  }

  std::string get_peer_identifier() const
  {
    switch (mode_) {
      case aemu_session_mode::PDP:
      case aemu_session_mode::PTP_LISTEN:
        return {};
      case aemu_session_mode::PTP_CONNECT:
        return get_accept_session_name(
            to_mac_, to_port_, from_mac_, from_port_);
      case aemu_session_mode::PTP_ACCEPT:
        return get_connect_session_name(
            to_mac_, to_port_, from_mac_, from_port_);
    }
    return {};
  }

  Socket socket_;
  boost::asio::steady_timer write_signal_;
  boost::asio::steady_timer bond_signal_;
  std::shared_ptr<relay_registry<Socket>> registry_;
  std::string client_addr_;
  chrono::steady_clock::time_point create_time_;

  std::string identifier_;
  aemu_session_mode mode_ = aemu_session_mode::PDP;
  char from_mac_[mac_size] {};
  u16 from_port_ = 0;
  char to_mac_[mac_size] {};
  u16 to_port_ = 0;

  std::deque<std::string> write_queue_;
  std::size_t queued_bytes_ = 0;
  bool established_ = false;
  bool closed_ = false;
  bool registered_ = false;
  bool superseded_ = false;
  bool finished_ = false;
};

template<class Acceptor>
awaitable<void> run_relay_acceptor(Acceptor& acceptor)
{
  using socket_type = typename Acceptor::protocol_type::socket;

  auto registry =
      std::make_shared<relay_registry<socket_type>>(acceptor.get_executor());
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

    if (registry->active() >= static_cast<std::size_t>(max_num_sessions)) {
      auto close_ec = error_code {};
      socket.close(close_ec);
      continue;
    }

    auto peer_addr = describe_peer(socket);
    registry->inc_active();
    std::make_shared<relay_session<socket_type>>(
        MOV(socket), MOV(peer_addr), registry)
        ->start();
  }

  co_return;
}

}  // namespace

awaitable<void> run_relay_server(tcp::acceptor& acceptor)
{
  co_await run_relay_acceptor(acceptor);
}

awaitable<void> run_relay_server(
    boost::asio::local::stream_protocol::acceptor& acceptor)
{
  co_await run_relay_acceptor(acceptor);
}

}  // namespace adhoc
