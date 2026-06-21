// SPDX-License-Identifier: AGPL-3.0-or-later

#include <cstddef>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "database.hpp"

#include <fmt/format.h>
#include <sqlite3.h>

using namespace std::string_literals;

using std::out_ptr;

namespace adhoc
{

namespace
{

std::string product_code_to_string(product_code const& code)
{
  return std::string(code.data, bounded_strlen(code.data, PRODUCT_CODE_LENGTH));
}

char const path[] = "database.db";

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

}  // namespace

product_db::product_db()
{
  if (!std::filesystem::exists(path)) {
    fmt::println("Database file '{}' not found; product lookups will be no-ops",
                 path);
    return;
  }

  if (sqlite3_open_v2(path, &db_, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
  {
    let msg = db_ ? sqlite3_errmsg(db_) : "unknown error";
    if (db_ != nullptr) {
      sqlite3_close(db_);
      db_ = nullptr;
    }
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

  char* err = nullptr;
  if (sqlite3_exec(db_, schema, nullptr, nullptr, &err) != SQLITE_OK) {
    let msg = err != nullptr ? err : "unknown error"s;
    sqlite3_free(err);
    sqlite3_close(std::exchange(db_, nullptr));
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
  let sql = "SELECT id_to FROM crosslinks WHERE id_from = ?;";
  auto stmt = statement_ptr {};
  if (sqlite3_prepare_v2(db_, sql, -1, out_ptr(stmt), nullptr) != SQLITE_OK) {
    return;
  }

  let id = product_code_to_string(code);
  let stmt_ptr = stmt.get();
  sqlite3_bind_text(stmt_ptr, 1, id.c_str(), -1, SQLITE_STATIC);
  if (sqlite3_step(stmt_ptr) == SQLITE_ROW) {
    let dst = cstr(sqlite3_column_text(stmt_ptr, 0));
    if (dst != nullptr) {
      let n = bounded_strlen(dst, PRODUCT_CODE_LENGTH);
      for (auto i = 0zu; i != PRODUCT_CODE_LENGTH; ++i) {
        code.data[i] = (i < n) ? dst[i] : 0;
      }
      fmt::println("Crosslinked {} to {}", id, std::string_view(dst, n));
    }
  }
}

std::string product_db::display_name_for(product_code const& code)
{
  let id = product_code_to_string(code);

  let sql = "SELECT name FROM productids WHERE id = ?;";
  auto stmt = statement_ptr {};
  if (sqlite3_prepare_v2(db_, sql, -1, out_ptr(stmt), nullptr) == SQLITE_OK) {
    let stmt_ptr = stmt.get();
    sqlite3_bind_text(stmt_ptr, 1, id.c_str(), -1, SQLITE_STATIC);
    if (sqlite3_step(stmt_ptr) == SQLITE_ROW) {
      let name = cstr(sqlite3_column_text(stmt_ptr, 0));
      if (name != nullptr) {
        let n = sqlite3_column_bytes(stmt_ptr, 0);
        return {name, static_cast<std::size_t>(n)};
      }
    }
  }
  return id;
}

void product_db::record_unknown_product(product_code const& code)
{
  let id = product_code_to_string(code);
  let id_cstr = id.c_str();

  let sql = "SELECT 1 FROM productids WHERE id = ?;";
  auto stmt = statement_ptr {};
  if (sqlite3_prepare_v2(db_, sql, -1, out_ptr(stmt), nullptr) == SQLITE_OK) {
    let stmt_ptr = stmt.get();
    sqlite3_bind_text(stmt_ptr, 1, id_cstr, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt_ptr) == SQLITE_ROW) {
      return;
    }
  }

  let ins = "SELECT 1 FROM productids WHERE id = ?;";
  if (sqlite3_prepare_v2(db_, ins, -1, out_ptr(stmt), nullptr) == SQLITE_OK) {
    let stmt_ptr = stmt.get();
    sqlite3_bind_text(stmt_ptr, 1, id_cstr, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt_ptr, 2, id_cstr, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt_ptr) == SQLITE_DONE) {
      fmt::println("Added unknown product id {} to database", id);
    }
  }
}

}  // namespace adhoc
