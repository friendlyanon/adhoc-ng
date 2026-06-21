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
#include <utility>
#include <variant>
#include <vector>

#ifdef __linux__
#  include <array>

#  include <sys/socket.h>
#  include <sys/un.h>
#  include <unistd.h>
#endif

#include <boost/asio.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/system.hpp>
#include <ctre.hpp>
#include <fmt/format.h>
#include <scn/scan.h>

#include "database.hpp"
#include "game_server.hpp"
#include "registry.hpp"
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
  throw usage_error(
      fmt::format(R"(Usage: {0}{1}{0} [server-endpoint] [status-endpoint]

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
  started.)"
#ifdef __linux__
                  R"(

  On Linux, you can run the program in systemd socket-activation mode by
  passing the single argument "systemd". In this mode the program uses the
  sockets passed by systemd via the LISTEN_FDS and LISTEN_PID environment
  variables. Behavior:

    LISTEN_FDS = 2
      The first socket is used for the game server and the second for the
      status server.
    LISTEN_FDS = 1
      The first socket is used for the game server; the status server is not
      started.
    LISTEN_FDS = 0 (or not set)
      The server binds to the default game server port ({2}) and the
      status server is not started.

  If LISTEN_PID is set it must match the server process PID; otherwise the
  server exits with an error. Values of LISTEN_FDS greater than 2 are not
  supported.)"
#endif
                  ,
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
      : io_context(&io_context)
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

    auto acceptor =
        stream_protocol::acceptor {*io_context, socket.endpoint, REUSE_ADDRESS};
    if (socket.perms) {
      auto ignore = std::error_code {};
      fs::permissions(path, *socket.perms, ignore);
    }

    return acceptor;
  }

  result operator()(tcp::endpoint const& endpoint) const
  {
    return tcp::acceptor {*io_context, endpoint};
  }

  result operator()(auto&&) const { return {}; }

private:
  boost::asio::io_context* io_context;
};

}  // namespace

// Entrypoint
namespace
{

int try_main(std::span<std::string_view> argv, FILE* err_out)
{
  let program = argv.size() == 0 ? "<unknown>"sv : argv[0];
  let args = argv.subspan(1u);
  if (args.size() > 2 || has_help_argument(args)) {
    usage(program);
  }

  auto maybe_enter_systemd_mode = false;
#ifdef __linux__
  auto systemd_mode = false;
  int systemd_fds = 0;
  auto systemd_fd_list = std::array<int, 2> {{-1, -1}};
  maybe_enter_systemd_mode = args.size() == 1 && args[0] == "systemd"sv;

  if (maybe_enter_systemd_mode) {
    if (auto pid_env = std::getenv("LISTEN_PID")) {
      if (auto parsed = scn::scan_int<long>(pid_env)) {
        if (static_cast<long>(::getpid()) != parsed->value()) {
          throw std::runtime_error("LISTEN_PID does not match process PID");
        }
      } else {
        throw std::runtime_error("Invalid LISTEN_PID environment variable");
      }
    }

    if (auto fds_env = std::getenv("LISTEN_FDS")) {
      if (auto parsed = scn::scan_int<int>(fds_env)) {
        systemd_fds = parsed->value();
      } else {
        throw std::runtime_error("Invalid LISTEN_FDS environment variable");
      }
    }

    if (systemd_fds < 0 || systemd_fds > 2) {
      throw std::runtime_error("Unsupported LISTEN_FDS value; max 2 supported");
    }

    if (systemd_fds > 0) {
      systemd_mode = true;
      constexpr int SD_LISTEN_FDS_START = 3;
      for (int i = 0; i != systemd_fds; ++i) {
        systemd_fd_list[i] = SD_LISTEN_FDS_START + i;
      }
    }
  }
#endif

  let game_endpoint = [&]() -> parsed_endpoint
  {
    if (args.size() < 1 || maybe_enter_systemd_mode) {
      return tcp::endpoint {tcp::v4(), default_server_port};
    }

    return parse_endpoint(args[0]);
  }();

  let status_endpoint = [&]() -> parsed_endpoint
  {
    if (args.size() < 2) {
      return {};
    }

    return parse_endpoint(args[1]);
  }();

  let had_parsing_error = [&]
  {
    auto handler = handle_parsing_error {err_out, "server"sv};
    auto result = false;
    result = std::visit(handler, game_endpoint) || result;
    handler.arg = "status"sv;
    result = std::visit(handler, status_endpoint) || result;
    return result;
  }();
  if (had_parsing_error) {
    usage(program);
  }

  auto io_context = boost::asio::io_context {1};
  let acceptor_visitor = to_acceptor {io_context};
#ifdef __linux__
  auto game_acceptor = maybe_acceptor {};
  auto status_acceptor = maybe_acceptor {};
  if (systemd_mode) {
    auto fd_to_acceptor = [&](int fd) -> maybe_acceptor
    {
      if (fd < 0) {
        return {};
      }

      auto addr = sockaddr_storage {};
      socklen_t len = sizeof(addr);
      if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        throw std::runtime_error("getsockname failed for systemd socket");
      }

      if (addr.ss_family == AF_UNIX) {
        auto acceptor = stream_protocol::acceptor {io_context};
        acceptor.assign(stream_protocol(), fd);
        return acceptor;
      } else if (addr.ss_family == AF_INET || addr.ss_family == AF_INET6) {
        auto acceptor = tcp::acceptor {io_context};
        if (addr.ss_family == AF_INET) {
          acceptor.assign(tcp::v4(), fd);
        } else {
          acceptor.assign(tcp::v6(), fd);
        }
        return acceptor;
      }

      return {};
    };

    game_acceptor = fd_to_acceptor(systemd_fd_list[0]);
    if (systemd_fds != 1) {
      status_acceptor = fd_to_acceptor(systemd_fd_list[1]);
    }
  } else {
    game_acceptor = std::visit(acceptor_visitor, game_endpoint);
    status_acceptor = std::visit(acceptor_visitor, status_endpoint);
  }
#else
  auto game_acceptor = std::visit(acceptor_visitor, game_endpoint);
  auto status_acceptor = std::visit(acceptor_visitor, status_endpoint);
#endif

  auto db = adhoc::product_db {};
  auto reg = adhoc::registry {db};

  let spawn_game = [&](auto& acceptor)
  {
    let coro = [&]() -> awaitable<void>
    { co_await adhoc::run_game_server(acceptor, reg); };
    co_spawn(io_context, coro, detached);
  };

  let spawn_status = [&](auto& acceptor)
  {
    let coro = [&]() -> awaitable<void>
    { co_await adhoc::run_status_server(acceptor, reg); };
    co_spawn(io_context, coro, detached);
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
    if constexpr (std::is_same_v<T, tcp::acceptor>
                  || std::is_same_v<T, stream_protocol::acceptor>)
    {
      auto buf = fmt::memory_buffer {};
      describe_endpoint(acceptor, buf);
      fmt::println("Game server listening on {}",
                   std::string_view {buf.data(), buf.size()});
      spawn_game(acceptor);
    }
  };
  std::visit(start_game_server, game_acceptor);

  let start_status_server = [&](auto& acceptor)
  {
    using T = std::decay_t<decltype(acceptor)>;
    if constexpr (std::is_same_v<T, tcp::acceptor>
                  || std::is_same_v<T, stream_protocol::acceptor>)
    {
      auto buf = fmt::memory_buffer {};
      describe_endpoint(acceptor, buf);
      fmt::println("Status server listening on {}",
                   std::string_view {buf.data(), buf.size()});
      spawn_status(acceptor);
    }
  };
  std::visit(start_status_server, status_acceptor);

  auto signals = boost::asio::signal_set {io_context, SIGINT, SIGTERM};
#ifdef SIGBREAK
  signals.add(SIGBREAK);
#endif

  let close_acceptor = [](auto& acceptor)
  {
    using T = std::decay_t<decltype(acceptor)>;
    if constexpr (!std::is_same_v<T, std::monostate>) {
      auto ignore = error_code {};
      acceptor.close(ignore);
    }
  };

  let on_stop_signal = [&](error_code const&, int) -> void
  {
    fmt::println("Received stop signal, shutting down");
    reg.broadcast_shutdown();
    std::visit(close_acceptor, game_acceptor);
    std::visit(close_acceptor, status_acceptor);
    let drain_timer = std::make_shared<boost::asio::steady_timer>(io_context);
    drain_timer->expires_after(std::chrono::milliseconds(250));
    let on_drained = [&io_context, drain_timer](error_code const&)
    { io_context.stop(); };
    drain_timer->async_wait(on_drained);
  };
  signals.async_wait(on_stop_signal);

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
