// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <string>
#include <string_view>

#include "protocol.hpp"

struct sqlite3;

namespace adhoc
{

// Tables (auto-created on open):
//   productids(id TEXT PRIMARY KEY, name TEXT NOT NULL)
//   crosslinks(id_from TEXT PRIMARY KEY, id_to TEXT NOT NULL)
class product_db
{
public:
  product_db();
  ~product_db();

  product_db(product_db const&) = delete;
  product_db& operator=(product_db const&) = delete;

  // If a crosslink exists for `code`, rewrite it in place.
  void apply_crosslink(product_code& code);

  // Returns the human-readable name, falling back to the product code itself.
  std::string display_name_for(std::string_view code) const;
  std::string display_name_for(product_code const& code) const;

  // If the product id isn't known, insert it (id == name == code).
  void record_unknown_product(product_code const& code);

private:
  sqlite3* db_ = nullptr;
};

}  // namespace adhoc
