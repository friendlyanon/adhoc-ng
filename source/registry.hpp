// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <boost/asio/ip/address_v4.hpp>

#include "heterogeneous.hpp"
#include "protocol.hpp"

namespace adhoc
{

class user_session;
class product_db;
struct game_node;
struct group_node;

struct group_node
{
  std::string name;  // raw bytes, length <= ADHOCCTL_GROUPNAME_LEN
  game_node* game = nullptr;
  std::vector<user_session*> players;  // front() is the first joiner (founder)
};

struct game_node
{
  product_code code {};
  std::string code_str;
  std::unordered_map<std::string,
                     std::unique_ptr<group_node>,
                     adhoc::transparent_string_hash,
                     std::equal_to<>>
      groups;
  std::vector<user_session*> users;
};

struct status_user
{
  std::string_view name;
  std::string_view product_code;
  std::optional<std::string_view> group;
};

class registry
{
public:
  explicit registry(product_db& db);
  ~registry();

  registry(registry const&) = delete;
  registry& operator=(registry const&) = delete;

  bool try_open_connection();

  void close_connection(user_session& session);

  bool handle_login(user_session& session, login_packet_c2s const& pkt);
  bool handle_connect(user_session& session, group_name const& group);
  bool handle_disconnect(user_session& session);
  bool handle_scan(user_session& session);
  bool handle_chat(user_session& session, std::string_view message);

  void broadcast_shutdown();

  std::vector<status_user> snapshot_for_status() const;

  std::string display_name_for(std::string_view code) const;

private:
  void leave_group(user_session& session);
  void destroy_game_if_empty(game_node* game);

  product_db* db_;
  std::unordered_map<std::string,
                     std::unique_ptr<game_node>,
                     adhoc::transparent_string_hash,
                     std::equal_to<>>
      games_;
  std::size_t connection_count_ = 0;
  bool shutdown_broadcasted_ = false;
};

}  // namespace adhoc
