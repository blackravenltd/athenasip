cmake_minimum_required(VERSION 3.23...4.2)

project(athenasip VERSION 0.1.0 LANGUAGES CXX)

option(ATHENA_BUILD_TESTING "Build test targets" OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

function(athena_add_prefix prefix)
    if(prefix AND IS_DIRECTORY "${prefix}")
        list(PREPEND CMAKE_PREFIX_PATH "${prefix}")
        set(CMAKE_PREFIX_PATH "${CMAKE_PREFIX_PATH}" PARENT_SCOPE)

        if(IS_DIRECTORY "${prefix}/lib/pkgconfig")
            list(PREPEND ATHENA_PKG_CONFIG_PATHS "${prefix}/lib/pkgconfig")
            set(ATHENA_PKG_CONFIG_PATHS "${ATHENA_PKG_CONFIG_PATHS}" PARENT_SCOPE)
        endif()

        if(IS_DIRECTORY "${prefix}/share/pkgconfig")
            list(PREPEND ATHENA_PKG_CONFIG_PATHS "${prefix}/share/pkgconfig")
            set(ATHENA_PKG_CONFIG_PATHS "${ATHENA_PKG_CONFIG_PATHS}" PARENT_SCOPE)
        endif()
    endif()
endfunction()

if(APPLE)
    athena_add_prefix("/usr/local")
    athena_add_prefix("/opt/homebrew")

    find_program(ATHENA_BREW brew)

    if(ATHENA_BREW)
        foreach(ATHENA_FORMULA
            boost
            openssl@3
            mysql-connector-c++
            libpq
            libpqxx
            yaml-cpp
            tinyxml2
            sqlite
            lua@5.4
            googletest
        )
            execute_process(
                COMMAND "${ATHENA_BREW}" --prefix "${ATHENA_FORMULA}"
                RESULT_VARIABLE ATHENA_BREW_RESULT
                OUTPUT_VARIABLE ATHENA_BREW_PREFIX
                ERROR_QUIET
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )

            if(ATHENA_BREW_RESULT EQUAL 0)
                athena_add_prefix("${ATHENA_BREW_PREFIX}")
            endif()
        endforeach()
    endif()

    if(CMAKE_PREFIX_PATH)
        list(REMOVE_DUPLICATES CMAKE_PREFIX_PATH)
    endif()

    if(ATHENA_PKG_CONFIG_PATHS)
        list(REMOVE_DUPLICATES ATHENA_PKG_CONFIG_PATHS)
        list(JOIN ATHENA_PKG_CONFIG_PATHS ":" ATHENA_PKG_CONFIG_PATH)

        if(DEFINED ENV{PKG_CONFIG_PATH} AND NOT "$ENV{PKG_CONFIG_PATH}" STREQUAL "")
            set(ENV{PKG_CONFIG_PATH} "${ATHENA_PKG_CONFIG_PATH}:$ENV{PKG_CONFIG_PATH}")
        else()
            set(ENV{PKG_CONFIG_PATH} "${ATHENA_PKG_CONFIG_PATH}")
        endif()
    endif()
endif()

find_package(Boost 1.91.0 EXACT CONFIG REQUIRED COMPONENTS thread json charconv)
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)

find_package(mysql-concpp CONFIG QUIET)

if(TARGET mysql::concpp)
    set(ATHENA_MYSQL_TARGET mysql::concpp)
else()
    find_path(ATHENA_MYSQL_INCLUDE_DIR NAMES mysqlx/xdevapi.h PATH_SUFFIXES include REQUIRED)
    find_library(ATHENA_MYSQL_LIBRARY NAMES mysqlcppconnx libmysqlcppconnx PATH_SUFFIXES lib lib64 REQUIRED)

    add_library(Athena::mysql_concpp INTERFACE IMPORTED)

    set_target_properties(Athena::mysql_concpp PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${ATHENA_MYSQL_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${ATHENA_MYSQL_LIBRARY}"
    )

    set(ATHENA_MYSQL_TARGET Athena::mysql_concpp)
endif()

find_package(PostgreSQL REQUIRED)

if(TARGET PostgreSQL::PostgreSQL)
    set(ATHENA_POSTGRESQL_TARGET PostgreSQL::PostgreSQL)
else()
    if(NOT PostgreSQL_INCLUDE_DIRS OR NOT PostgreSQL_LIBRARIES)
        message(FATAL_ERROR "PostgreSQL was found, but no usable target or variables were provided")
    endif()

    add_library(Athena::PostgreSQL INTERFACE IMPORTED)

    set_target_properties(Athena::PostgreSQL PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${PostgreSQL_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES "${PostgreSQL_LIBRARIES}"
    )

    if(PostgreSQL_LIBRARY_DIRS)
        set_target_properties(Athena::PostgreSQL PROPERTIES
            INTERFACE_LINK_DIRECTORIES "${PostgreSQL_LIBRARY_DIRS}"
        )
    endif()

    set(ATHENA_POSTGRESQL_TARGET Athena::PostgreSQL)
endif()

find_package(libpqxx CONFIG QUIET)

if(TARGET libpqxx::pqxx)
    set(ATHENA_PQXX_TARGET libpqxx::pqxx)
elseif(TARGET pqxx::pqxx)
    set(ATHENA_PQXX_TARGET pqxx::pqxx)
elseif(TARGET pqxx)
    set(ATHENA_PQXX_TARGET pqxx)
else()
    find_path(ATHENA_PQXX_INCLUDE_DIR NAMES pqxx/pqxx PATH_SUFFIXES include REQUIRED)
    find_library(ATHENA_PQXX_LIBRARY NAMES pqxx libpqxx PATH_SUFFIXES lib lib64 REQUIRED)

    add_library(Athena::pqxx INTERFACE IMPORTED)

    set_target_properties(Athena::pqxx PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${ATHENA_PQXX_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${ATHENA_PQXX_LIBRARY};${ATHENA_POSTGRESQL_TARGET}"
    )

    set(ATHENA_PQXX_TARGET Athena::pqxx)
endif()

find_package(yaml-cpp CONFIG REQUIRED)

if(TARGET yaml-cpp::yaml-cpp)
    set(ATHENA_YAML_TARGET yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
    set(ATHENA_YAML_TARGET yaml-cpp)
else()
    message(FATAL_ERROR "yaml-cpp was found, but no usable target was provided")
endif()

find_package(tinyxml2 CONFIG QUIET)

if(NOT TARGET tinyxml2::tinyxml2 AND NOT TARGET tinyxml2)
    set(tinyxml2_BUILD_TESTING OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        tinyxml2
        GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
        GIT_TAG 11.0.0
    )

    FetchContent_MakeAvailable(tinyxml2)
endif()

if(TARGET tinyxml2::tinyxml2)
    set(ATHENA_TINYXML2_TARGET tinyxml2::tinyxml2)
elseif(TARGET tinyxml2)
    set(ATHENA_TINYXML2_TARGET tinyxml2)
else()
    message(FATAL_ERROR "tinyxml2 was not found")
endif()

find_package(SQLite3 REQUIRED)

if(TARGET SQLite3::SQLite3)
    set(ATHENA_SQLITE_TARGET SQLite3::SQLite3)
elseif(TARGET SQLite::SQLite3)
    set(ATHENA_SQLITE_TARGET SQLite::SQLite3)
else()
    if(NOT SQLite3_INCLUDE_DIRS OR NOT SQLite3_LIBRARIES)
        message(FATAL_ERROR "SQLite3 was found, but no usable target or variables were provided")
    endif()

    add_library(Athena::SQLite3 INTERFACE IMPORTED)

    set_target_properties(Athena::SQLite3 PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${SQLite3_INCLUDE_DIRS}"
        INTERFACE_LINK_LIBRARIES "${SQLite3_LIBRARIES}"
    )

    set(ATHENA_SQLITE_TARGET Athena::SQLite3)
endif()

find_package(Lua REQUIRED)

if(Lua_FOUND AND NOT TARGET Lua::Lua)
    add_library(Lua::Lua INTERFACE IMPORTED)

    set_target_properties(Lua::Lua PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${LUA_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${LUA_LIBRARIES}"
    )
endif()

if(NOT TARGET Lua::Lua)
    message(FATAL_ERROR "Lua was found, but no usable target or variables were provided")
endif()

file(GLOB_RECURSE ATHENA_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

list(REMOVE_ITEM ATHENA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
)

add_library(athena_core STATIC ${ATHENA_SOURCES})

target_compile_features(athena_core PUBLIC cxx_std_20)

target_include_directories(athena_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_include_directories(athena_core
        PUBLIC
            "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    )
endif()

target_link_libraries(athena_core
    PUBLIC
        Boost::thread
        Boost::json
        Boost::charconv
        OpenSSL::SSL
        OpenSSL::Crypto
        "${ATHENA_MYSQL_TARGET}"
        "${ATHENA_PQXX_TARGET}"
        "${ATHENA_YAML_TARGET}"
        "${ATHENA_TINYXML2_TARGET}"
        "${ATHENA_SQLITE_TARGET}"
        Lua::Lua
)

add_executable(athenasip "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

target_link_libraries(athenasip PRIVATE athena_core)
target_compile_features(athenasip PRIVATE cxx_std_20)

if(MSVC AND TARGET OpenSSL::applink)
    target_link_libraries(athenasip PRIVATE OpenSSL::applink)
endif()

if(ATHENA_BUILD_TESTING)
    include(CTest)
    enable_testing()

    find_package(GTest REQUIRED)

    file(GLOB_RECURSE ATHENA_TEST_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.cpp"
    )

    add_executable(athenasip_tests ${ATHENA_TEST_SOURCES})

    target_link_libraries(athenasip_tests PRIVATE athena_core)

    if(TARGET GTest::gtest_main)
        target_link_libraries(athenasip_tests PRIVATE GTest::gtest_main)
    elseif(TARGET GTest::Main)
        target_link_libraries(athenasip_tests PRIVATE GTest::Main)
    else()
        message(FATAL_ERROR "GTest was found, but no usable main target was provided")
    endif()

    target_compile_features(athenasip_tests PRIVATE cxx_std_20)

    if(MSVC AND TARGET OpenSSL::applink)
        target_link_libraries(athenasip_tests PRIVATE OpenSSL::applink)
    endif()

    include(GoogleTest)
    gtest_discover_tests(athenasip_tests)
endif()