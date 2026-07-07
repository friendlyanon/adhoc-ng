cmake_minimum_required(VERSION 3.25...3.30)

project(adhoc-ng CXX)

add_executable(
    adhoc-server-pro
    source/main.cpp
    source/utf8.cpp
    source/database.cpp
    source/registry.cpp
    source/session.cpp
    source/game_server.cpp
    source/status_server.cpp
    source/relay_server.cpp
    source/resource.rc
)
target_include_directories(adhoc-server-pro PRIVATE source)
target_compile_features(adhoc-server-pro PRIVATE cxx_std_23)
set_property(TARGET adhoc-server-pro PROPERTY CXX_SCAN_FOR_MODULES OFF)
target_compile_definitions(
    adhoc-server-pro PRIVATE
    let=auto\ const
    "REUSE_ADDRESS=\$<NOT:\$<BOOL:${WIN32}>>"
)
if(WIN32)
  target_compile_definitions(adhoc-server-pro PRIVATE _WIN32_WINNT=0x0A00)
endif()
if(MSVC)
  target_compile_options(adhoc-server-pro PRIVATE /utf-8)
  set(DISABLE_MANIFEST_FLAG /MANIFEST:NO CACHE STRING "")
  mark_as_advanced(DISABLE_MANIFEST_FLAG)
  target_link_options(adhoc-server-pro PRIVATE ${DISABLE_MANIFEST_FLAG})
endif()

find_package(ctre REQUIRED)
find_package(Boost REQUIRED)
find_package(fmt REQUIRED)
find_package(scn REQUIRED)
find_path(SQLITE3_INCLUDE_DIR sqlite3.h REQUIRED)
find_library(SQLITE3_LIBRARY sqlite3 REQUIRED)
target_include_directories(adhoc-server-pro SYSTEM PRIVATE "${SQLITE3_INCLUDE_DIR}")
target_link_libraries(
    adhoc-server-pro
    PRIVATE ctre::ctre Boost::headers fmt::fmt scn::scn "${SQLITE3_LIBRARY}"
)

install(TARGETS adhoc-server-pro DESTINATION .)
