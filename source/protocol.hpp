// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace adhoc
{

inline constexpr auto ETHER_ADDR_LEN = 6zu;
inline constexpr auto ADHOCCTL_GROUPNAME_LEN = 8zu;
inline constexpr auto ADHOCCTL_NICKNAME_LEN = 128zu;
inline constexpr auto PRODUCT_CODE_LENGTH = 9zu;
inline constexpr auto CHAT_MESSAGE_LEN = 64zu;

inline constexpr auto SERVER_USER_MAXIMUM = 1024zu;
inline constexpr int SERVER_USER_TIMEOUT_SECONDS = 15;
inline constexpr char SERVER_SHUTDOWN_MESSAGE[] =
    "PROMETHEUS HUB IS SHUTTING DOWN!";

enum opcode : std::uint8_t
{
  OPCODE_PING = 0,
  OPCODE_LOGIN = 1,
  OPCODE_CONNECT = 2,
  OPCODE_DISCONNECT = 3,
  OPCODE_SCAN = 4,
  OPCODE_SCAN_COMPLETE = 5,
  OPCODE_CONNECT_BSSID = 6,
  OPCODE_CHAT = 7,
};

constexpr std::size_t bounded_strlen(let* data, std::size_t max) noexcept
{
  auto len = 0zu;
  while (len != max && data[len] != 0) {
    ++len;
  }
  return len;
}

#pragma pack(push, 1)

struct ether_addr
{
  std::uint8_t data[ETHER_ADDR_LEN];
};

struct group_name
{
  std::uint8_t data[ADHOCCTL_GROUPNAME_LEN];
};

struct nickname
{
  std::uint8_t data[ADHOCCTL_NICKNAME_LEN];
};

struct product_code
{
  char data[PRODUCT_CODE_LENGTH];
};

struct chat_message
{
  char data[CHAT_MESSAGE_LEN];
};

struct packet_base
{
  std::uint8_t opcode;
};

// C2S

struct login_packet_c2s
{
  packet_base base;
  ether_addr mac;
  nickname name;
  product_code game;
};

struct connect_packet_c2s
{
  packet_base base;
  group_name group;
};

struct chat_packet_c2s
{
  packet_base base;
  chat_message message;
};

// S2C

struct connect_packet_s2c
{
  packet_base base;
  nickname name;
  ether_addr mac;
  std::uint32_t ip;  // network byte order
};

struct disconnect_packet_s2c
{
  packet_base base;
  std::uint32_t ip;
};

struct scan_packet_s2c
{
  packet_base base;
  group_name group;
  ether_addr mac;
};

struct connect_bssid_packet_s2c
{
  packet_base base;
  ether_addr mac;
};

struct chat_packet_s2c
{
  chat_packet_c2s base;
  nickname name;
};

#pragma pack(pop)

static_assert(sizeof(login_packet_c2s) == 144);
static_assert(sizeof(connect_packet_c2s) == 9);
static_assert(sizeof(chat_packet_c2s) == 65);
static_assert(sizeof(connect_packet_s2c) == 139);
static_assert(sizeof(disconnect_packet_s2c) == 5);
static_assert(sizeof(scan_packet_s2c) == 15);
static_assert(sizeof(connect_bssid_packet_s2c) == 7);
static_assert(sizeof(chat_packet_s2c) == 193);

}  // namespace adhoc
