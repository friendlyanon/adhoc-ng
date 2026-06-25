// SPDX-License-Identifier: AGPL-3.0-or-later

#include <chrono>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <utility>

#include "session.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include "registry.hpp"

using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::redirect_error;
using boost::asio::use_awaitable;
using boost::system::error_code;

namespace adhoc
{

template<class Socket>
session_impl<Socket>::session_impl(Socket socket, registry& reg)
    : socket_(std::move(socket))
    , timer_(socket_.get_executor())
    , write_signal_(socket_.get_executor())
    , registry_(&reg)
{
}

template<class Socket>
void session_impl<Socket>::start()
{
  timer_.expires_after(std::chrono::seconds(SERVER_USER_TIMEOUT_SECONDS));

  let self = this->shared_from_this();
  let executor = socket_.get_executor();

  auto run_reader = [self]() -> awaitable<void> { co_await self->reader(); };
  auto run_writer = [self]() -> awaitable<void> { co_await self->writer(); };
  auto run_watchdog = [self]() -> awaitable<void> { co_await self->watchdog(); };

  co_spawn(executor, std::move(run_reader), detached);
  co_spawn(executor, std::move(run_writer), detached);
  co_spawn(executor, std::move(run_watchdog), detached);
}

template<class Socket>
void session_impl<Socket>::send_bytes(std::vector<std::byte> bytes)
{
  if (closed_ || bytes.empty()) {
    return;
  }
  write_queue_.push_back(std::move(bytes));
  write_signal_.cancel_one();
}

template<class Socket>
void session_impl<Socket>::close()
{
  if (closed_) {
    return;
  }
  closed_ = true;

  auto ec = error_code {};
  socket_.close(ec);
  timer_.cancel();
  write_signal_.cancel();
}

template<class Socket>
void session_impl<Socket>::consume_rx(std::size_t n)
{
  if (n > rxpos_) {
    n = rxpos_;
  }
  std::memmove(rx_, rx_ + n, rxpos_ - n);
  rxpos_ -= n;
}

template<class Socket>
awaitable<void> session_impl<Socket>::reader()
{
  auto ec = error_code {};

  while (!closed_) {
    if (rxpos_ >= sizeof(rx_)) {
      close();
      break;
    }

    let n = co_await socket_.async_read_some(
        boost::asio::buffer(rx_ + rxpos_, sizeof(rx_) - rxpos_),
        redirect_error(use_awaitable, ec));
    if (ec || n == 0) {
      break;
    }
    rxpos_ += n;
    timer_.expires_after(std::chrono::seconds(SERVER_USER_TIMEOUT_SECONDS));

    auto need_more = false;
    while (!need_more && rxpos_ > 0 && !closed_) {
      let op = rx_[0];
      if (game == nullptr) {
        if (op != OPCODE_LOGIN) {
          close();
          break;
        }
        if (rxpos_ < sizeof(login_packet_c2s)) {
          need_more = true;
          break;
        }
        auto pkt = login_packet_c2s {};
        std::memcpy(&pkt, rx_, sizeof(pkt));
        consume_rx(sizeof(pkt));
        if (!registry_->handle_login(*this, pkt)) {
          close();
          break;
        }
        continue;
      }

      switch (op) {
        case OPCODE_PING:
          consume_rx(1);
          break;

        case OPCODE_CONNECT:
          if (rxpos_ < sizeof(connect_packet_c2s)) {
            need_more = true;
            break;
          }
          {
            auto pkt = connect_packet_c2s {};
            std::memcpy(&pkt, rx_, sizeof(pkt));
            consume_rx(sizeof(pkt));
            if (!registry_->handle_connect(*this, pkt.group)) {
              close();
            }
          }
          break;

        case OPCODE_DISCONNECT:
          consume_rx(1);
          if (!registry_->handle_disconnect(*this)) {
            close();
          }
          break;

        case OPCODE_SCAN:
          consume_rx(1);
          if (!registry_->handle_scan(*this)) {
            close();
          }
          break;

        case OPCODE_CHAT:
          if (rxpos_ < sizeof(chat_packet_c2s)) {
            need_more = true;
            break;
          }
          {
            auto pkt = chat_packet_c2s {};
            std::memcpy(&pkt, rx_, sizeof(pkt));
            consume_rx(sizeof(pkt));
            let mlen = bounded_strlen(pkt.message, CHAT_MESSAGE_LEN);
            if (!registry_->handle_chat(*this,
                                        std::string_view(pkt.message, mlen)))
            {
              close();
            }
          }
          break;

        default:
          close();
          break;
      }
    }
  }

  close();
  registry_->close_connection(*this);
}

template<class Socket>
awaitable<void> session_impl<Socket>::writer()
{
  auto ec = error_code {};

  while (!closed_) {
    if (write_queue_.empty()) {
      write_signal_.expires_at(clock::time_point::max());
      co_await write_signal_.async_wait(redirect_error(use_awaitable, ec));
      ec = {};
      continue;
    }

    let bytes = std::move(write_queue_.front());
    write_queue_.pop_front();

    co_await boost::asio::async_write(
        socket_, boost::asio::buffer(bytes), redirect_error(use_awaitable, ec));
    if (ec) {
      close();
      break;
    }
  }
}

template<class Socket>
awaitable<void> session_impl<Socket>::watchdog()
{
  auto ec = error_code {};

  while (!closed_) {
    co_await timer_.async_wait(redirect_error(use_awaitable, ec));
    if (closed_) {
      break;
    }
    if (ec == boost::asio::error::operation_aborted) {
      ec = {};
      continue;
    }
    if (clock::now() >= timer_.expiry()) {
      close();
      break;
    }
  }
}

template class session_impl<boost::asio::ip::tcp::socket>;
template class session_impl<boost::asio::local::stream_protocol::socket>;

}  // namespace adhoc
