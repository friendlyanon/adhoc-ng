set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)
set(path "${CMAKE_CURRENT_LIST_DIR}/../../i686-linux-musl/bin/i686-linux-musl-")
get_filename_component(CMAKE_C_COMPILER "${path}gcc" ABSOLUTE CACHE)
get_filename_component(CMAKE_CXX_COMPILER "${path}g++" ABSOLUTE CACHE)

set(CMAKE_CXX_FLAGS -DFMT_USE_INT128=0)
