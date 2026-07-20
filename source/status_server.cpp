// SPDX-License-Identifier: AGPL-3.0-or-later

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

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
#include "fwd_mov.hpp"
#include "registry.hpp"
#include "relay_server.hpp"

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
        out += R"(\")"sv;
        break;
      case '\\':
        out += R"(\\)"sv;
        break;
      case '\b':
        out += R"(\b)"sv;
        break;
      case '\f':
        out += R"(\f)"sv;
        break;
      case '\n':
        out += R"(\n)"sv;
        break;
      case '\r':
        out += R"(\r)"sv;
        break;
      case '\t':
        out += R"(\t)"sv;
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

struct status_group_user
{
  std::string_view name;
  std::string_view mac;
};

struct status_group
{
  std::string_view name;
  std::vector<status_group_user> users;
};

struct status_game
{
  std::string_view code;
  std::string name;
  std::size_t user_count = 0;
  std::vector<status_group> groups;
  std::unordered_map<std::string_view, std::size_t> group_index;
};

std::vector<status_game> build_status_games(registry const& reg)
{
  let users = reg.snapshot_for_status();
  auto games = std::vector<status_game> {};
  auto game_index = std::unordered_map<std::string_view, std::size_t> {};
  games.reserve(users.size());
  game_index.reserve(users.size());

  let groupless = "Groupless"sv;
  for (let& user : users) {
    let index_pair = game_index.try_emplace(user.product_code, games.size());
    if (index_pair.second) {
      let& code = user.product_code;
      games.emplace_back(code, reg.display_name_for(code));
    }

    auto& game = games[index_pair.first->second];
    ++game.user_count;

    let group = user.group.value_or(groupless);
    let pair = game.group_index.try_emplace(group, game.groups.size());
    if (pair.second) {
      game.groups.emplace_back(group);
    }

    game.groups[pair.first->second].users.emplace_back(user.name, user.mac);
  }

  return games;
}

void append_port_array(std::string& out,
                       std::vector<std::uint16_t> const& ports)
{
  auto sorted = ports;
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());

  let out_it = std::back_inserter(out);
  auto first = true;
  for (let port : sorted) {
    if (!first) {
      out.push_back(',');
    }
    first = false;
    fmt::format_to(out_it, "{}", port);
  }
}

void build_status_json(std::string& out,
                       registry const& reg,
                       relay_directory const& relay)
{
  let games = build_status_games(reg);
  let out_it = std::back_inserter(out);

  out += R"({"games":[)"sv;
  auto first = true;
  for (let& game : games) {
    if (!first) {
      out.push_back(',');
    }
    first = false;

    out += R"({"name":")"sv;
    append_json_escaped(out, game.name);
    fmt::format_to(
        out_it, R"(","usercount":{},"game_ids":[")", game.user_count);
    append_json_escaped(out, game.code);
    out += R"("],"groups":[)"sv;

    auto first_group = true;
    for (let& group : game.groups) {
      if (!first_group) {
        out.push_back(',');
      }
      first_group = false;

      out += R"({"name":")"sv;
      append_json_escaped(out, group.name);
      fmt::format_to(
          out_it, R"(","usercount":{},"users":[)", group.users.size());

      auto first_user = true;
      for (let& user : group.users) {
        if (!first_user) {
          out.push_back(',');
        }
        first_user = false;

        out += R"({"name":")"sv;
        append_json_escaped(out, user.name);
        out.push_back('"');

        if (let ports = relay.find(user.mac)) {
          out += R"(,"pdp_ports":[)"sv;
          append_port_array(out, ports->pdp);
          out += R"(],"ptp_ports":[)"sv;
          append_port_array(out, ports->ptp);
          out.push_back(']');
        } else {
          out += R"(,"pdp_ports":[],"ptp_ports":[])"sv;
        }

        out.push_back('}');
      }

      out += "]}"sv;
    }

    out += "]}"sv;
  }

  out += "]}"sv;
}

template<class Stream>
awaitable<void> handle_status_connection(Stream stream,
                                         registry const& reg,
                                         relay_directory const& relay)
{
  auto ec = error_code {};
  auto buffer = beast::flat_buffer {};

  for (;;) {
    auto req = http::request<http::empty_body> {};

    ec = {};
    co_await http::async_read(
        stream, buffer, req, redirect_error(use_awaitable, ec));
    if (ec) {
      break;
    }

    auto res = http::response<http::string_body> {};
    res.version(req.version());
    let keep_alive = req.keep_alive();
    res.keep_alive(keep_alive);

    if (req.method() != http::verb::get || req.target() != "/data.json"sv) {
      res.result(http::status::not_found);
      res.set(http::field::content_type, "text/plain; charset=utf-8"sv);
      res.body() = "Not Found"sv;
    } else {
      res.result(http::status::ok);
      res.set(http::field::content_type, "application/json; charset=utf-8"sv);
      build_status_json(res.body(), reg, relay);
    }
    res.prepare_payload();

    ec = {};
    co_await http::async_write(stream, res, redirect_error(use_awaitable, ec));
    if (ec) {
      break;
    }

    if (!keep_alive) {
      break;
    }
  }

  if constexpr (std::is_same_v<Stream, tcp::socket>) {
    stream.shutdown(tcp::socket::shutdown_send, ec);
  } else {
    stream.shutdown(stream_protocol::socket::shutdown_send, ec);
  }
  stream.close(ec);
}

template<class Acceptor>
awaitable<void> run_status_server_generic(Acceptor& acceptor,
                                          registry const& reg,
                                          relay_directory const& relay)
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

    auto handler = [s = MOV(socket), &reg, &relay]() mutable -> awaitable<void>
    { co_await handle_status_connection<socket_type>(MOV(s), reg, relay); };
    co_spawn(acceptor.get_executor(), MOV(handler), detached);
  }
}

}  // namespace

awaitable<void> run_status_server(tcp::acceptor& acceptor,
                                  registry const& reg,
                                  relay_directory const& relay)
{
  return run_status_server_generic(acceptor, reg, relay);
}

awaitable<void> run_status_server(stream_protocol::acceptor& acceptor,
                                  registry const& reg,
                                  relay_directory const& relay)
{
  return run_status_server_generic(acceptor, reg, relay);
}

}  // namespace adhoc
