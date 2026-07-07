// SPDX-License-Identifier: AGPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <boost/asio.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/system.hpp>
#include <ctre.hpp>
#include <fmt/format.h>
#include <scn/scan.h>

#include "database.hpp"
#include "fwd_mov.hpp"
#include "game_server.hpp"
#include "registry.hpp"
#include "relay_server.hpp"
#include "status_server.hpp"
#include "utf8.hpp"

using namespace std::string_view_literals;

using boost::asio::awaitable;
using boost::asio::co_spawn;
using boost::asio::detached;
using boost::asio::ip::make_address_v4;
using boost::asio::ip::make_address_v6;
using boost::asio::ip::tcp;
using boost::asio::local::stream_protocol;
using boost::system::error_code;

namespace errc = boost::system::errc;
namespace fs = std::filesystem;

// Command line handling
namespace
{

constexpr std::uint16_t default_server_port = 27312;

class usage_error : public std::runtime_error
{
  using std::runtime_error::runtime_error;
};

[[noreturn]] void usage(std::string_view program)
{
  let base = fs::path(std::string(program)).filename().string();
  let quote = ctre::search<R"([ :()\[\]{}])">(base) ? "\""sv : ""sv;
  throw usage_error(fmt::format(
      R"(Usage: {0}{1}{0} [server-endpoint] [status-endpoint] [--relay [relay-endpoint]]

  Endpoint format:
    {2}
      Will bind to 0.0.0.0:{2} (IPv4)
    1.2.3.4:{2}
      Will bind to 1.2.3.4:{2} (IPv4)
    [::1]:{2}
      Will bind to [::1]:{2} (IPv6)
    unix:/path/to/socket
      Will bind to /path/to/socket (UNIX socket)
    unix:770:/path/to/socket
      Will bind to /path/to/socket with 770 permissions (UNIX socket)

  When server-endpoint is not provided, the server binds to 0.0.0.0:{2}.
  When status-endpoint is not provided, the status endpoint will not be
  started.
  When --relay is provided without an argument, the relay binds to the same
  address as the game server on the next port number.)",
      quote,
      base,
      default_server_port));
}

bool has_help_argument(std::span<std::string_view> args)
{
  if (args.size() == 0) {
    return false;
  }

  let& first = args[0];
  return first == "-h"sv || first == "-help"sv || first == "--help"sv
      || first == "/h"sv || first == "-?"sv || first == "/?"sv;
}

struct unix_socket
{
  std::optional<fs::perms> perms;
  stream_protocol::endpoint endpoint;
};

using parsed_endpoint =
    std::variant<std::monostate, error_code, tcp::endpoint, unix_socket>;

parsed_endpoint parse_endpoint(std::string_view string)
{
  if (let unix_match = ctre::match<R"(unix:(?:([0-7]{3}):)?(.+))">(string)) {
    let perms = [&]() -> std::optional<fs::perms>
    {
      if (let perms_match = unix_match.get<1>()) {
        return static_cast<fs::perms>(
            scn::scan_int<std::uint16_t>(perms_match.view(), 8)->value());
      }
      return {};
    }();
    return unix_socket {perms, {unix_match.get<2>().view()}};
  }

  let match = ctre::match<
      R"re((?:(?:(\d+\.\d+\.\d+\.\d+)|\[([:a-fA-F0-9]+)\]):)?(\d+))re">(string);
  if (!match) {
    return errc::make_error_code(errc::invalid_argument);
  }

  let port_match = match.get<3>().view();
  auto port = std::uint16_t {};
  if (let result = scn::scan_int<std::uint16_t>(port_match)) {
    port = result->value();
  } else {
    return errc::make_error_code(errc::value_too_large);
  }

  {
    auto ec = error_code {};
    if (let ipv4 = match.get<1>()) {
      let address = make_address_v4(ipv4.str(), ec);
      if (ec) {
        return ec;
      }

      return tcp::endpoint {address, port};
    } else if (let ipv6 = match.get<2>()) {
      let address = make_address_v6(ipv6.str(), ec);
      if (ec) {
        return ec;
      }

      return tcp::endpoint {address, port};
    }
  }

  return tcp::endpoint {tcp::v4(), port};
}

struct handle_parsing_error
{
  FILE* out;
  std::string_view arg;

  bool operator()(error_code const& ec) const
  {
    if (!ec) {
      return false;
    }

    fmt::println(out, "Error parsing {}-endpoint: {}", arg, ec.message());
    return true;
  }

  bool operator()(auto&&) const { return false; }
};

}  // namespace

// Server handling
namespace
{

using maybe_acceptor =
    std::variant<std::monostate, tcp::acceptor, stream_protocol::acceptor>;

class to_acceptor
{
public:
  explicit to_acceptor(boost::asio::io_context& io_context)
      : io_context_(&io_context)
  {
  }

  using result = maybe_acceptor;

  result operator()(unix_socket const& socket) const
  {
    let path = fs::path(socket.endpoint.path());
    {
      auto ignore = std::error_code {};
      fs::remove(path, ignore);
    }

    auto acceptor = stream_protocol::acceptor {
        *io_context_, socket.endpoint, REUSE_ADDRESS};
    if (socket.perms) {
      auto ignore = std::error_code {};
      fs::permissions(path, *socket.perms, ignore);
    }

    return acceptor;
  }

  result operator()(tcp::endpoint const& endpoint) const
  {
    return tcp::acceptor {*io_context_, endpoint};
  }

  result operator()(auto&&) const { return {}; }

private:
  boost::asio::io_context* io_context_;
};

template<class T>
constexpr bool is_acceptor_v = std::conjunction_v<  //
    std::is_same<T, tcp::acceptor>,
    std::is_same<T, stream_protocol::acceptor>>;

std::string_view as_str(let& x)
{
  return {x.data(), x.size()};
}

}  // namespace

// Entrypoint
namespace
{

int try_main(std::span<std::string_view> argv, FILE* err_out)
{
  let program = argv.size() == 0 ? "<unknown>"sv : argv[0];
  let args = argv.subspan(1u);
  if (has_help_argument(args)) {
    usage(program);
  }

  let relay_pos = std::find(args.begin(), args.end(), "--relay"sv);
  let has_relay = relay_pos != args.end();
  let relay_index = static_cast<std::size_t>(relay_pos - args.begin());
  let server_args = has_relay ? args.first(relay_index) : args;
  let relay_args = has_relay ? args.subspan(relay_index + 1)
                             : std::span<std::string_view> {};
  if (server_args.size() > 2 || relay_args.size() > 1) {
    usage(program);
  }

  let game_endpoint = [&]() -> parsed_endpoint
  {
    if (server_args.size() < 1) {
      return tcp::endpoint {tcp::v4(), default_server_port};
    }

    return parse_endpoint(server_args[0]);
  }();

  let status_endpoint = [&]() -> parsed_endpoint
  {
    if (server_args.size() < 2) {
      return {};
    }

    return parse_endpoint(server_args[1]);
  }();

  let relay_endpoint = [&]() -> parsed_endpoint
  {
    if (!has_relay) {
      return {};
    }

    if (!relay_args.empty()) {
      return parse_endpoint(relay_args[0]);
    }

    let next_port = [](auto const& value) -> parsed_endpoint
    {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, tcp::endpoint>) {
        let port = value.port();
        if (port == std::numeric_limits<std::uint16_t>::max()) {
          return errc::make_error_code(errc::value_too_large);
        }

        let relay_port = static_cast<std::uint16_t>(port + 1);
        return tcp::endpoint {value.address(), relay_port};
      } else {
        return errc::make_error_code(errc::invalid_argument);
      }
    };
    return std::visit(next_port, game_endpoint);
  }();

  let had_parsing_error = [&]
  {
    auto handler = handle_parsing_error {err_out, "server"sv};
    auto result = false;
    result = std::visit(handler, game_endpoint) || result;
    handler.arg = "status"sv;
    result = std::visit(handler, status_endpoint) || result;
    handler.arg = "relay"sv;
    result = std::visit(handler, relay_endpoint) || result;
    return result;
  }();
  if (had_parsing_error) {
    usage(program);
  }

  auto io_context = boost::asio::io_context {1};
  let acceptor_visitor = to_acceptor {io_context};

  auto game_acceptor = std::visit(acceptor_visitor, game_endpoint);
  auto status_acceptor = std::visit(acceptor_visitor, status_endpoint);
  auto relay_acceptor = std::visit(acceptor_visitor, relay_endpoint);

  auto db = adhoc::product_db {};
  auto reg = adhoc::registry {db};

  let spawn_game = [&](auto& acceptor)
  {
    auto coro = [&]() -> awaitable<void>
    { co_await adhoc::run_game_server(acceptor, reg); };
    co_spawn(io_context, MOV(coro), detached);
  };

  let spawn_status = [&](auto& acceptor)
  {
    auto coro = [&]() -> awaitable<void>
    { co_await adhoc::run_status_server(acceptor, reg); };
    co_spawn(io_context, MOV(coro), detached);
  };

  let spawn_relay = [&](auto& acceptor)
  {
    auto coro = [&]() -> awaitable<void>
    { co_await adhoc::run_relay_server(acceptor); };
    co_spawn(io_context, MOV(coro), detached);
  };

  let describe_endpoint = [](let& acceptor, fmt::memory_buffer& buf) -> void
  {
    using T = std::decay_t<decltype(acceptor)>;
    auto ignore = error_code {};
    let endpoint = acceptor.local_endpoint(ignore);
    if constexpr (std::is_same_v<T, tcp::acceptor>) {
      fmt::format_to(std::back_inserter(buf),
                     "{}:{}",
                     endpoint.address().to_string(),
                     endpoint.port());
    } else {
      fmt::format_to(std::back_inserter(buf), "unix:{}", endpoint.path());
    }
  };

  let start_game_server = [&](auto& acceptor)
  {
    using T = std::decay_t<decltype(acceptor)>;
    if constexpr (is_acceptor_v<T>) {
      auto buf = fmt::memory_buffer {};
      describe_endpoint(acceptor, buf);
      fmt::println("Game server listening on {}", as_str(buf));
      spawn_game(acceptor);
    }
  };
  std::visit(start_game_server, game_acceptor);

  let start_status_server = [&](auto& acceptor)
  {
    using T = std::decay_t<decltype(acceptor)>;
    if constexpr (is_acceptor_v<T>) {
      auto buf = fmt::memory_buffer {};
      describe_endpoint(acceptor, buf);
      fmt::println("Status server listening on {}", as_str(buf));
      spawn_status(acceptor);
    }
  };
  std::visit(start_status_server, status_acceptor);

  let start_relay_server = [&](auto& acceptor)
  {
    using T = std::decay_t<decltype(acceptor)>;
    if constexpr (is_acceptor_v<T>) {
      auto buf = fmt::memory_buffer {};
      describe_endpoint(acceptor, buf);
      fmt::println("Relay server listening on {}", as_str(buf));
      spawn_relay(acceptor);
    }
  };
  std::visit(start_relay_server, relay_acceptor);

  auto signals = boost::asio::signal_set {io_context, SIGINT, SIGTERM};
#ifdef SIGBREAK
  signals.add(SIGBREAK);
#endif

  let close_acceptor = [](auto& acceptor)
  {
    using T = std::decay_t<decltype(acceptor)>;
    if constexpr (is_acceptor_v<T>) {
      auto ignore = error_code {};
      acceptor.close(ignore);
    }
  };

  auto on_stop_signal = [&](error_code const&, int) -> void
  {
    fmt::println("Received stop signal, shutting down");
    reg.broadcast_shutdown();
    std::visit(close_acceptor, game_acceptor);
    std::visit(close_acceptor, status_acceptor);
    std::visit(close_acceptor, relay_acceptor);
    let drain_timer = std::make_shared<boost::asio::steady_timer>(io_context);
    drain_timer->expires_after(std::chrono::milliseconds(250));
    auto on_drained = [&io_context, drain_timer](error_code const&)
    { io_context.stop(); };
    drain_timer->async_wait(MOV(on_drained));
  };
  signals.async_wait(MOV(on_stop_signal));

  io_context.run();
  return 0;
}

}  // namespace

int main(int argc, char** argv)
{
  auto exit_code = 1;
  if (!set_utf8_output()) {
    return exit_code;
  }

  std::setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
  std::setvbuf(stderr, nullptr, _IOLBF, BUFSIZ);

#ifdef SIGPIPE
  std::signal(SIGPIPE, SIG_IGN);
#endif
#ifdef SIGHUP
  std::signal(SIGHUP, SIG_IGN);
#endif

  let err_out = stderr;
  let print = [&](char const* message)
  {
    let len = std::strlen(message);
    if (std::fwrite(message, 1, len, err_out) != len
        || std::fputc('\n', err_out) == EOF || std::fflush(err_out) != 0)
    {
      exit_code = 2;
      return false;
    }

    return true;
  };

  try {
    auto args = std::vector<std::string_view> {};
    if (argc > 0) {
      args.reserve(static_cast<std::size_t>(argc));
      for (int i = 0; i != argc; ++i) {
        args.emplace_back(argv[i]);
      }
    }
    return try_main(args, err_out);
  } catch (usage_error const& error) {
    if (print(error.what())) {
      return 64;
    }
  } catch (std::exception const& error) {
    print(error.what());
  }
  return exit_code;
}
