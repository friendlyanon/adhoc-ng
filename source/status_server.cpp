// SPDX-License-Identifier: AGPL-3.0-or-later

#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include "status_server.hpp"

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>
#include <fmt/format.h>

#include "database.hpp"
#include "registry.hpp"

using namespace std::string_view_literals;

namespace beast = boost::beast;
namespace http = boost::beast::http;
using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::redirect_error;
using boost::asio::use_awaitable;
using boost::asio::ip::tcp;
using boost::asio::local::stream_protocol;
using boost::system::error_code;

namespace adhoc
{

namespace
{

void append_json_escaped(std::string& out, std::string_view s)
{
  auto out_it = std::back_inserter(out);
  for (char c : s) {
    let uc = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        fmt::format_to(out_it, R"(\")");
        break;
      case '\\':
        fmt::format_to(out_it, R"(\\)");
        break;
      case '\b':
        fmt::format_to(out_it, R"(\b)");
        break;
      case '\f':
        fmt::format_to(out_it, R"(\f)");
        break;
      case '\n':
        fmt::format_to(out_it, R"(\n)");
        break;
      case '\r':
        fmt::format_to(out_it, R"(\r)");
        break;
      case '\t':
        fmt::format_to(out_it, R"(\t)");
        break;
      default:
        if (uc < 0x20) {
          fmt::format_to(out_it, R"(\u{:04x})", static_cast<unsigned>(uc));
        } else {
          out.push_back(c);
        }
    }
  }
}

void build_status_json(std::string& out, registry& reg)
{
  let users = reg.snapshot_for_status();
  auto out_it = std::back_inserter(out);

  fmt::format_to(out_it, R"({{"user_count":{},"users":[)", users.size());

  auto first = true;
  for (let& user : users) {
    if (!first) {
      out.push_back(',');
    }
    first = false;

    auto code = product_code {};
    let n = std::min(user.product_code.size(), PRODUCT_CODE_LENGTH);
    std::memcpy(code.data, user.product_code.data(), n);
    let display = reg.display_name_for(code);

    fmt::format_to(out_it, R"({{"name":")");
    append_json_escaped(out, user.name);
    fmt::format_to(out_it, R"(","game":{{"product_code":")");
    append_json_escaped(out, user.product_code);
    fmt::format_to(out_it, R"(","display_name":")");
    append_json_escaped(out, display);
    fmt::format_to(out_it, R"("}},"group":)");
    if (user.group) {
      out.push_back('"');
      append_json_escaped(out, *user.group);
      out.push_back('"');
    } else {
      fmt::format_to(out_it, "null");
    }
    out.push_back('}');
  }

  fmt::format_to(out_it, "]}}");
}

template<class Stream>
awaitable<void> handle_status_connection(Stream stream, registry& reg)
{
  auto ec = error_code {};
  auto buffer = beast::flat_buffer {};
  auto req = http::request<http::empty_body> {};

  co_await http::async_read(
      stream, buffer, req, redirect_error(use_awaitable, ec));
  if (ec) {
    co_return;
  }

  auto res = http::response<http::string_body> {};
  res.version(req.version());
  res.keep_alive(false);

  if (req.method() != http::verb::get || req.target() != "/"sv) {
    res.result(http::status::not_found);
    res.set(http::field::content_type, "text/plain; charset=utf-8"sv);
    res.body() = "Not Found"sv;
  } else {
    res.result(http::status::ok);
    res.set(http::field::content_type, "application/json; charset=utf-8"sv);
    build_status_json(res.body(), reg);
  }
  res.prepare_payload();

  co_await http::async_write(stream, res, redirect_error(use_awaitable, ec));

  if constexpr (std::is_same_v<Stream, tcp::socket>) {
    stream.shutdown(tcp::socket::shutdown_send, ec);
  } else {
    stream.shutdown(stream_protocol::socket::shutdown_send, ec);
  }
  stream.close(ec);
}

template<class Acceptor>
awaitable<void> run_status_server_generic(Acceptor& acceptor, registry& reg)
{
  using socket_type = typename Acceptor::protocol_type::socket;
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

    auto handler = [s = std::move(socket), &reg]() mutable -> awaitable<void>
    { co_await handle_status_connection<socket_type>(std::move(s), reg); };
    co_spawn(acceptor.get_executor(), std::move(handler), detached);
  }
}

}  // namespace

awaitable<void> run_status_server(tcp::acceptor& acceptor, registry& reg)
{
  return run_status_server_generic(acceptor, reg);
}

awaitable<void> run_status_server(stream_protocol::acceptor& acceptor,
                                  registry& reg)
{
  return run_status_server_generic(acceptor, reg);
}

}  // namespace adhoc
