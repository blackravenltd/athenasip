cmake_minimum_required(VERSION 3.31)
project(athenasip LANGUAGES CXX)

# Set C++ Standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use new policy behaviours
cmake_policy(SET CMP0177 NEW)
cmake_policy(SET CMP0167 NEW)

# Options
option(ATHENA_BUILD_TESTING "Build the test targets" OFF)

# FetchContent module
include(FetchContent)

# Use /usr/local also
list(APPEND CMAKE_PREFIX_PATH "/usr/local")

# Find Boost libraries - note that Boost.Beast and Boost.JSON are header-only and part of Boost.
find_package(Boost 1.87 REQUIRED COMPONENTS system thread json)
message(STATUS "Boost Include Directory: ${Boost_INCLUDE_DIRS}")

# Find OpenSSL
if(APPLE)
  set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl@3")
  set(OPENSSL_LIBRARIES "/usr/local/opt/openssl@3/lib/libssl.dylib;/usr/local/opt/openssl@3/lib/libcrypto.dylib")
  set(OPENSSL_INCLUDE_DIR "/usr/local/opt/openssl@3/include")
  link_directories(/usr/local/opt/openssl@3/lib)
endif()

find_package(OpenSSL REQUIRED)
message(STATUS "OpenSSL Include Directory: ${OPENSSL_INCLUDE_DIR}")

# Find MySQL C++ Connector
find_package(mysql-concpp REQUIRED)
message(STATUS "MySQL Library Directory: ${MYSQL_CONCPP_INCLUDE_DIR}")

# Find libpqxx using pkg-config
find_package(PkgConfig REQUIRED)
pkg_check_modules(PQXX REQUIRED libpqxx)
message(STATUS "libpqxx Include Directory: ${PQXX_INCLUDE_DIRS}")

# Add libpqxx include directories to the project
link_directories(${PQXX_LIBRARY_DIRS})

# YAML: Use yaml-cpp instead of RapidYAML
FetchContent_Declare(
  yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG 0.8.0
)
FetchContent_MakeAvailable(yaml-cpp)

# tinyxml2
FetchContent_Declare(
  tinyxml2
  GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
  GIT_TAG 9.0.0  
)
FetchContent_MakeAvailable(tinyxml2)

# SQLite3
find_package(SQLite3 REQUIRED)
message(STATUS "SQLite3 Include Directory: ${SQLite3_INCLUDE_DIRS}")
message(STATUS "SQLite3 Libraries: ${SQLite3_LIBRARIES}")

# Lua
find_package(Lua REQUIRED)
message(STATUS "Lua Include Directory: ${LUA_INCLUDE_DIR}")
message(STATUS "Lua Libraries: ${LUA_LIBRARIES}")

# Testing

# Collect all source files except main.cpp
file(GLOB_RECURSE ATHENA_SOURCES src/*.cpp)
list(REMOVE_ITEM ATHENA_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

# Create the library for testing
add_library(athenasip_lib ${ATHENA_SOURCES})

# Include directories
target_include_directories(athenasip_lib PUBLIC 
    ${PQXX_INCLUDE_DIRS}
    ${Boost_INCLUDE_DIRS}
    ${SQLite3_INCLUDE_DIRS}
    ${LUA_INCLUDE_DIR}
)

# Link libraries
target_link_libraries(athenasip_lib PUBLIC
    Boost::system
    Boost::thread
    Boost::json
    OpenSSL::SSL
    OpenSSL::Crypto
    mysql::concpp
    yaml-cpp::yaml-cpp
    tinyxml2::tinyxml2
    ${LUA_LIBRARIES}
    ${PQXX_LIBRARIES} 
    ${SQLite3_LIBRARIES}
)

target_compile_features(athenasip_lib PUBLIC cxx_std_17)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(athenasip_lib PUBLIC pq)
endif()

# Make Main Executable
add_executable(athenasip ${ATHENA_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
target_include_directories(athenasip PUBLIC 
    ${LUA_INCLUDE_DIR}
)
target_link_libraries(athenasip PRIVATE 
  Boost::system
  Boost::thread
  Boost::json
  OpenSSL::SSL
  OpenSSL::Crypto
  mysql::concpp
  yaml-cpp::yaml-cpp
  tinyxml2::tinyxml2
  ${LUA_LIBRARIES}
  ${PQXX_LIBRARIES} 
  ${SQLite3_LIBRARIES}
)
target_compile_features(athenasip PRIVATE cxx_std_17)

if(ATHENA_BUILD_TESTING)
  enable_testing()
  find_package(GTest REQUIRED)
  message(STATUS "Building GoogleTest tests")

  file(GLOB_RECURSE ATHENA_TEST_SOURCES tests/*.cpp)
  add_executable(athenasip_tests ${ATHENA_TEST_SOURCES})
  target_include_directories(athenasip_tests PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(athenasip_tests PRIVATE 
      athenasip_lib
      GTest::gtest
      GTest::gtest_main
  )
  target_compile_features(athenasip_tests PRIVATE cxx_std_17)
  include(GoogleTest)
  gtest_discover_tests(athenasip_tests)
endif()
