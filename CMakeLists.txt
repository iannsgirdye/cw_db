cmake_minimum_required(VERSION 3.16)
project(cw_db CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

# Dependencies: apt packages when available; Crow via FetchContent in build/_deps/.

include(FetchContent)

find_package(nlohmann_json 3.11.0 QUIET)
if(NOT nlohmann_json_FOUND)
    message(STATUS "nlohmann_json not found via apt — FetchContent")
    FetchContent_Declare(
        nlohmann_json
        GIT_REPOSITORY https://github.com/nlohmann/json.git
        GIT_TAG        v3.11.3
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

find_path(ASIO_INCLUDE_DIR asio.hpp
    PATHS /usr/include /usr/local/include
    PATH_SUFFIXES asio include
)
if(NOT ASIO_INCLUDE_DIR)
    message(STATUS "Asio headers not found via apt — FetchContent")
    FetchContent_Declare(
        asio
        GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
        GIT_TAG        asio-1-30-2
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(asio)
    set(ASIO_INCLUDE_DIR ${asio_SOURCE_DIR}/asio/include)
endif()

add_library(asio_standalone INTERFACE)
target_include_directories(asio_standalone INTERFACE ${ASIO_INCLUDE_DIR})
target_compile_definitions(asio_standalone INTERFACE ASIO_STANDALONE ASIO_HEADER_ONLY)

FetchContent_Declare(
    Crow
    GIT_REPOSITORY https://github.com/CrowCpp/Crow.git
    GIT_TAG        v1.2.0
    GIT_SHALLOW    TRUE
)
set(ASIO_INCLUDE_DIR ${ASIO_INCLUDE_DIR} CACHE STRING "" FORCE)
FetchContent_MakeAvailable(Crow)

# ---------- SQL: flex/bison (system) + project grammar src/sql/grammar/ ----------

find_package(FLEX REQUIRED)
find_package(BISON REQUIRED)

set(GRAMMAR_DIR ${CMAKE_SOURCE_DIR}/src/sql/grammar)
set(GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})

BISON_TARGET(SqlParser ${GRAMMAR_DIR}/sql.y
             ${GENERATED_DIR}/sql_parser.cpp
             DEFINES_FILE ${GENERATED_DIR}/sql_parser.h)
FLEX_TARGET(SqlLexer ${GRAMMAR_DIR}/sql.l
            ${GENERATED_DIR}/sql_lexer.cpp)
ADD_FLEX_BISON_DEPENDENCY(SqlLexer SqlParser)

# ---------- Core DBMS ----------

set(CORE_SOURCES
    src/core/value.cpp
    src/core/string_pool.cpp
    src/core/dbms.cpp
    src/storage/pager.cpp
    src/storage/bulk_build_guard.cpp
    src/storage/disk_bsp_index.cpp
    src/storage/table.cpp
    src/storage/database.cpp
    ${FLEX_SqlLexer_OUTPUTS}
    ${BISON_SqlParser_OUTPUTS}
    src/sql/parser.cpp
    src/sql/executor.cpp
    src/net/logger.cpp
    src/net/telemetry.cpp
)

add_library(cw_db_core STATIC ${CORE_SOURCES})
target_include_directories(cw_db_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${GENERATED_DIR}
)
find_package(Threads REQUIRED)

target_link_libraries(cw_db_core PUBLIC
    Threads::Threads
    nlohmann_json::nlohmann_json
)
target_compile_features(cw_db_core PUBLIC cxx_std_20)
# Generated sources above already depend on FLEX/BISON custom commands.

add_executable(prog src/apps/main.cpp)
target_link_libraries(prog PRIVATE cw_db_core)

option(CW_DB_BUILD_SERVER "Build the HTTP server/client tools" ON)
if(CW_DB_BUILD_SERVER)
    add_executable(prog-server src/apps/server_main.cpp src/net/server.cpp)
    target_link_libraries(prog-server PRIVATE cw_db_core Crow::Crow asio_standalone)

    add_executable(prog-client src/apps/client_main.cpp)
    target_link_libraries(prog-client PRIVATE cw_db_core nlohmann_json::nlohmann_json)
endif()

add_executable(test_bulk_build_guard tests/test_bulk_build_guard.cpp)
target_link_libraries(test_bulk_build_guard PRIVATE cw_db_core)
