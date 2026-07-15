// SPDX-License-Identifier: AGPL-3.0-or-later

#include <algorithm>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "registry.hpp"

#include <fmt/base.h>

#include "database.hpp"
#include "fwd_mov.hpp"
#include "session.hpp"
#include "string.hpp"

using namespace std::string_view_literals;

namespace adhoc
{

namespace
{

std::string_view product_code_str(product_code const& c)
{
  return {c.data, bounded_strlen(c.data, PRODUCT_CODE_LENGTH)};
}

std::string_view group_name_str(group_name const& g)
{
  return {cstr(g.data), bounded_strlen(g.data, ADHOCCTL_GROUPNAME_LEN)};
}

std::string_view nickname_str(nickname const& n)
{
  return {cstr(n.data), bounded_strlen(n.data, ADHOCCTL_NICKNAME_LEN)};
}

template<typename T>
T data_from(let& view)
{
  auto t = T {};
  let copy_len = std::min(view.size(), std::size(t.data));
  std::memcpy(t.data, view.data(), copy_len);
  return t;
}

bool valid_product_code(product_code const& code)
{
  for (auto i = 0zu; i != PRODUCT_CODE_LENGTH; ++i) {
    let c = code.data[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
      return false;
    }
  }
  return true;
}

bool valid_group_name(group_name const& g)
{
  for (auto i = 0zu; i != ADHOCCTL_GROUPNAME_LEN; ++i) {
    let c = g.data[i];
    if (c == 0) {
      break;
    }
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9'))
    {
      continue;
    }
    return false;
  }
  return true;
}

bool valid_login_packet(login_packet_c2s const& pkt)
{
  if (!valid_product_code(pkt.game)) {
    return false;
  }

  static constexpr std::uint8_t broadcast[ETHER_ADDR_LEN] = {
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  static constexpr std::uint8_t zero[ETHER_ADDR_LEN] = {0, 0, 0, 0, 0, 0};
  if (std::memcmp(pkt.mac.data, broadcast, ETHER_ADDR_LEN) == 0
      || std::memcmp(pkt.mac.data, zero, ETHER_ADDR_LEN) == 0)
  {
    return false;
  }

  return pkt.name.data[0] != 0;
}

void vector_remove(auto& v, let& value)
{
  let it = std::find(v.begin(), v.end(), value);
  if (it != v.end()) {
    v.erase(it);
  }
}

}  // namespace

registry::registry(product_db& db)
    : db_(&db)
{
}

registry::~registry() = default;

bool registry::try_open_connection()
{
  if (connection_count_ >= SERVER_USER_MAXIMUM) {
    return false;
  }
  ++connection_count_;
  return true;
}

void registry::close_connection(user_session& session)
{
  if (session.reaped) {
    return;
  }
  session.reaped = true;

  if (session.group != nullptr) {
    leave_group(session);
  }

  if (session.game != nullptr) {
    let game = std::exchange(session.game, nullptr);
    vector_remove(game->users, &session);
    fmt::println("{} ({}) stopped playing {}",
                 nickname_str(session.name),
                 session.peer_label,
                 game->code_str);
    destroy_game_if_empty(game);
  } else {
    fmt::println("Dropped connection to {}", session.peer_label);
  }

  if (connection_count_ != 0) {
    --connection_count_;
  }
}

bool registry::handle_login(user_session& session, login_packet_c2s const& pkt)
{
  if (session.game != nullptr) {
    return false;  // already logged in
  }
  if (!valid_login_packet(pkt)) {
    fmt::println("Invalid login packet from {}", session.peer_label);
    return false;
  }

  let code = [&]
  {
    auto code_ = pkt.game;
    db_->apply_crosslink(code_);
    return code_;
  }();
  db_->record_unknown_product(code);

  let key = product_code_str(code);
  let it = games_.find(key);
  game_node* game = nullptr;
  if (it == games_.end()) {
    auto node = std::make_unique<game_node>();
    node->code = code;
    node->code_str = key;
    game = node.get();
    games_.emplace(key, MOV(node));
  } else {
    game = it->second.get();
  }

  session.mac = pkt.mac;
  session.name = pkt.name;
  session.game = game;
  game->users.push_back(&session);

  fmt::println("{} ({}) started playing {}",
               nickname_str(session.name),
               session.peer_label,
               game->code_str);

  return true;
}

bool registry::handle_connect(user_session& session,
                              group_name const& group_name)
{
  if (session.game == nullptr) {
    return false;
  }
  if (!valid_group_name(group_name)) {
    fmt::println("{} ({}) attempted to join invalid group on {}",
                 nickname_str(session.name),
                 session.peer_label,
                 session.game->code_str);
    return false;
  }
  if (session.group != nullptr) {
    fmt::println("{} ({}) attempted to join a group while already in one",
                 nickname_str(session.name),
                 session.peer_label);
    return false;
  }

  let key = group_name_str(group_name);
  group_node* group = nullptr;
  let it = session.game->groups.find(key);
  if (it == session.game->groups.end()) {
    auto node = std::make_unique<group_node>();
    node->name = key;
    node->game = session.game;
    group = node.get();
    session.game->groups.emplace(key, MOV(node));
  } else {
    group = it->second.get();
  }

  for (let peer : group->players) {
    peer->send_bytes(packet_bytes(connect_packet_s2c {
        {OPCODE_CONNECT}, session.name, session.mac, session.ip_be}));

    session.send_bytes(packet_bytes(connect_packet_s2c {
        {OPCODE_CONNECT}, peer->name, peer->mac, peer->ip_be}));
  }

  group->players.push_back(&session);
  session.group = group;

  session.send_bytes(packet_bytes(connect_bssid_packet_s2c {
      {OPCODE_CONNECT_BSSID},
      group->players.empty() ? session.mac : group->players.front()->mac}));

  fmt::println("{} ({}) joined {} group {}",
               nickname_str(session.name),
               session.peer_label,
               session.game->code_str,
               group->name);
  return true;
}

void registry::leave_group(user_session& session)
{
  let group = std::exchange(session.group, nullptr);
  if (group == nullptr) {
    return;
  }
  vector_remove(group->players, &session);

  {
    let bytes = packet_bytes(
        disconnect_packet_s2c {{OPCODE_DISCONNECT}, session.ip_be});
    for (let peer : group->players) {
      peer->send_bytes(bytes);
    }
  }

  fmt::println("{} ({}) left {} group {}",
               nickname_str(session.name),
               session.peer_label,
               session.game != nullptr
                   ? std::string_view {session.game->code_str}
                   : "?"sv,
               group->name);

  if (group->players.empty()) {
    if (session.game != nullptr) {
      session.game->groups.erase(group->name);
    }
  }
}

bool registry::handle_disconnect(user_session& session)
{
  if (session.game == nullptr) {
    return false;
  }
  if (session.group == nullptr) {
    fmt::println("{} ({}) attempted to leave a group without joining one first",
                 nickname_str(session.name),
                 session.peer_label);
    return false;
  }
  leave_group(session);
  return true;
}

bool registry::handle_scan(user_session& session)
{
  if (session.game == nullptr) {
    return false;
  }
  if (session.group != nullptr) {
    fmt::println("{} ({}) attempted to scan while in a group",
                 nickname_str(session.name),
                 session.peer_label);
    return false;
  }

  for (let& [group_key, group] : session.game->groups) {
    let& p = group->players;
    let mac = p.empty() ? session.mac : p.front()->mac;
    session.send_bytes(packet_bytes(scan_packet_s2c {
        {OPCODE_SCAN}, data_from<group_name>(group->name), mac}));
  }

  session.send_bytes(
      std::vector {{static_cast<std::byte>(OPCODE_SCAN_COMPLETE)}});

  fmt::println("{} ({}) requested scan ({} groups in {})",
               nickname_str(session.name),
               session.peer_label,
               session.game->groups.size(),
               session.game->code_str);
  return true;
}

bool registry::handle_chat(user_session& session, std::string_view message)
{
  if (session.game == nullptr) {
    return false;
  }
  if (session.group == nullptr) {
    fmt::println("{} ({}) attempted to chat without joining a group",
                 nickname_str(session.name),
                 session.peer_label);
    return false;
  }

  let bytes = packet_bytes(chat_packet_s2c {
      {{OPCODE_CHAT}, data_from<chat_message>(message)}, session.name});

  auto recipients = 0zu;
  for (let peer : session.group->players) {
    if (peer != &session) {
      peer->send_bytes(bytes);
      ++recipients;
    }
  }

  if (recipients != 0) {
    fmt::println("{} ({}) sent chat to {} peers in {}/{}",
                 nickname_str(session.name),
                 session.peer_label,
                 recipients,
                 session.game->code_str,
                 session.group->name);
  }
  return true;
}

void registry::broadcast_shutdown()
{
  if (shutdown_broadcasted_) {
    return;
  }
  shutdown_broadcasted_ = true;

  let msg = std::string_view(SERVER_SHUTDOWN_MESSAGE);
  let bytes = packet_bytes(
      chat_packet_s2c {{{OPCODE_CHAT}, data_from<chat_message>(msg)}, {}});

  for (let& [game_key, game] : games_) {
    for (let& [group_key, group] : game->groups) {
      for (let peer : group->players) {
        peer->send_bytes(bytes);
      }
    }
  }
}

void registry::destroy_game_if_empty(game_node* game)
{
  if (game != nullptr && game->users.empty()) {
    games_.erase(game->code_str);
  }
}

std::vector<status_user> registry::snapshot_for_status() const
{
  auto out = std::vector<status_user> {};
  out.reserve(connection_count_);

  for (let& [game_key, game] : games_) {
    let& code = game->code_str;
    for (let session : game->users) {
      let mac = std::string_view {cstr(session->mac.data), ETHER_ADDR_LEN};
      auto& u = out.emplace_back(nickname_str(session->name), mac, code);
      if (let group = session->group) {
        u.group = group->name;
      }
    }
  }
  return out;
}

std::string registry::display_name_for(std::string_view code) const
{
  return db_->display_name_for(code);
}

}  // namespace adhoc
