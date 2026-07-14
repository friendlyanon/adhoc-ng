// SPDX-License-Identifier: AGPL-3.0-or-later

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "database.hpp"

#include <fmt/format.h>
#include <sqlite3.h>

#include "string.hpp"

using namespace std::string_view_literals;

using std::out_ptr;
using std::string_view;

namespace adhoc
{

namespace
{

string_view product_code_str(product_code const& code)
{
  return {code.data, bounded_strlen(code.data, PRODUCT_CODE_LENGTH)};
}

bool ckd_(sqlite3* db, int actual, int expected, int line)
{
  if (actual != expected) {
    if ((expected == SQLITE_ROW && actual == SQLITE_DONE)
        || (expected == SQLITE_DONE && actual == SQLITE_ROW))
    {
      return false;
    }

    let msg = sqlite3_errmsg(db);
    throw std::runtime_error(
        fmt::format("SQLite operation failed on line {}: {}", line, msg));
  }

  return true;
}

void bind_(sqlite3* db, sqlite3_stmt* stmt, int idx, string_view str, int line)
{
  let ret = sqlite3_bind_text(
      stmt, idx, str.data(), static_cast<int>(str.size()), SQLITE_STATIC);
  ckd_(db, ret, SQLITE_OK, line);
}

#define ckd(actual, expected) (ckd_(db_, (actual), (expected), __LINE__))
#define bind(stmt, idx, str) (bind_(db_, (stmt), (idx), (str), __LINE__))

struct statement_deleter
{
  void operator()(sqlite3_stmt* stmt) const
  {
    if (stmt != nullptr) {
      sqlite3_finalize(stmt);
    }
  }
};

using statement_ptr = std::unique_ptr<sqlite3_stmt, statement_deleter>;

statement_ptr prepare_(sqlite3* db, string_view sql, int line)
{
  auto stmt = statement_ptr {};
  let ret = sqlite3_prepare_v2(
      db, sql.data(), static_cast<int>(sql.size()), out_ptr(stmt), nullptr);
  ckd_(db, ret, SQLITE_OK, line);
  return stmt;
}

#define prepare(sql) (prepare_(db_, (sql), __LINE__))

char const path[] = "database.db";

}  // namespace

product_db::product_db()
{
  if (!std::filesystem::exists(path)) {
    fmt::println("Database file '{}' not found; product lookups will be no-ops",
                 path);
    return;
  }

  char buffer[256];
  let to_msg = [&](char const* error)
  {
    let n = bounded_strlen(error, sizeof(buffer));
    return string_view(cstr(std::memcpy(buffer, error, n)), n);
  };

  if (let ret = sqlite3_open_v2(path, &db_, SQLITE_OPEN_READWRITE, nullptr);
      ret != SQLITE_OK)
  {
    let msg = [&]() -> string_view
    {
      if (db_) {
        let msg_ = to_msg(sqlite3_errmsg(db_));
        sqlite3_close(db_);
        return msg_;
      }

      return sqlite3_errstr(ret);
    }();
    throw std::runtime_error(
        fmt::format("Failed to open database '{}': {}", path, msg));
  }

  let schema =
      "CREATE TABLE IF NOT EXISTS productids ("
      "  id TEXT PRIMARY KEY,"
      "  name TEXT NOT NULL"
      ");"
      "CREATE TABLE IF NOT EXISTS crosslinks ("
      "  id_from TEXT PRIMARY KEY,"
      "  id_to TEXT NOT NULL"
      ");";

  if (sqlite3_exec(db_, schema, nullptr, nullptr, nullptr) != SQLITE_OK) {
    let msg = to_msg(sqlite3_errmsg(db_));
    sqlite3_close(db_);
    throw std::runtime_error(
        fmt::format("Failed to initialize database schema: {}", msg));
  }
}

product_db::~product_db()
{
  if (db_ != nullptr) {
    sqlite3_close(db_);
  }
}

void product_db::apply_crosslink(product_code& code)
{
  let id = product_code_str(code);
  let stmt = prepare("SELECT id_to FROM crosslinks WHERE id_from = ?;"sv);
  let stmt_ptr = stmt.get();
  bind(stmt_ptr, 1, id);
  if (ckd(sqlite3_step(stmt_ptr), SQLITE_DONE)) {
    return;
  }

  let dst = cstr(sqlite3_column_text(stmt_ptr, 0));
  if (dst == nullptr) {
    return;
  }

  let bytes = sqlite3_column_bytes(stmt_ptr, 0);
  let n = std::min(static_cast<std::size_t>(bytes), PRODUCT_CODE_LENGTH);
  let new_id = string_view(dst, n);
  if (new_id == id) {
    return;
  }

  code = {};
  std::memcpy(code.data, dst, n);
  fmt::println("Crosslinked {} to {}", id, new_id);
}

std::string product_db::display_name_for(string_view code) const
{
  let stmt = prepare("SELECT name FROM productids WHERE id = ?;"sv);
  let stmt_ptr = stmt.get();
  bind(stmt_ptr, 1, code);
  if (ckd(sqlite3_step(stmt_ptr), SQLITE_ROW)) {
    if (let name = cstr(sqlite3_column_text(stmt_ptr, 0))) {
      let n = sqlite3_column_bytes(stmt_ptr, 0);
      return {name, static_cast<std::size_t>(n)};
    }
  }

  return std::string(code);
}

std::string product_db::display_name_for(product_code const& code) const
{
  return display_name_for(product_code_str(code));
}

void product_db::record_unknown_product(product_code const& code)
{
  let id = product_code_str(code);

  {
    let stmt = prepare("SELECT 1 FROM productids WHERE id = ?;"sv);
    let stmt_ptr = stmt.get();
    bind(stmt_ptr, 1, id);
    if (ckd(sqlite3_step(stmt_ptr), SQLITE_ROW)) {
      return;
    }
  }

  {
    let stmt = prepare("INSERT INTO productids (id, name) VALUES (?, ?);"sv);
    let stmt_ptr = stmt.get();
    bind(stmt_ptr, 1, id);
    bind(stmt_ptr, 2, id);
    ckd(sqlite3_step(stmt_ptr), SQLITE_DONE);
    fmt::println("Added unknown product id {} to database", id);
  }
}

}  // namespace adhoc
