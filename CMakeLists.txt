cmake_minimum_required(VERSION 3.23...4.2)

project(athenasip VERSION 0.8.0 LANGUAGES CXX)

option(ATHENA_BUILD_TESTING "Build test targets" OFF)
set(ATHENA_SANITIZE "none" CACHE STRING "Sanitizer to build with: none, address, thread")
set_property(CACHE ATHENA_SANITIZE PROPERTY STRINGS none address thread)

# Single-config generators leave CMAKE_BUILD_TYPE empty, which means no optimisation
# and no debug info. Packaging passes Release explicitly.
get_property(ATHENA_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)

if(NOT ATHENA_MULTI_CONFIG AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(ATHENA_SANITIZE STREQUAL "address")
    set(ATHENA_SANITIZE_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
elseif(ATHENA_SANITIZE STREQUAL "thread")
    set(ATHENA_SANITIZE_FLAGS -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT ATHENA_SANITIZE STREQUAL "none")
    message(FATAL_ERROR "ATHENA_SANITIZE must be one of: none, address, thread")
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/build_version.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/build_version.h"
    @ONLY
)

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
            yaml-cpp
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

find_package(Boost 1.87.0 CONFIG REQUIRED COMPONENTS thread json charconv)
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)

find_package(yaml-cpp CONFIG REQUIRED)

if(TARGET yaml-cpp::yaml-cpp)
    set(ATHENA_YAML_TARGET yaml-cpp::yaml-cpp)
elseif(TARGET yaml-cpp)
    set(ATHENA_YAML_TARGET yaml-cpp)
else()
    message(FATAL_ERROR "yaml-cpp was found, but no usable target was provided")
endif()

file(GLOB_RECURSE ATHENA_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp"
)

list(REMOVE_ITEM ATHENA_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp"
)

# Lua scripting is parked: the sources stay in the tree but are not built and the
# runtime carries no Lua dependency until there is a real use for it.
list(FILTER ATHENA_SOURCES EXCLUDE REGEX "/src/script/")

# RTPProxyClient speaks the rtpproxy text protocol and nothing constructs it: rtpengine
# and its ng protocol are the flagship engine, and the MediaEngine contract is how a
# driver reaches the server now. It stays in the tree as the starting point for an
# rtpproxy driver and out of the build until someone writes one.
list(FILTER ATHENA_SOURCES EXCLUDE REGEX "/src/rtp/rtp_proxy_client")

add_library(athena_core STATIC ${ATHENA_SOURCES})

target_compile_features(athena_core PUBLIC cxx_std_20)

target_include_directories(athena_core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>"
)

if(ATHENA_SANITIZE_FLAGS)
    target_compile_options(athena_core PUBLIC ${ATHENA_SANITIZE_FLAGS})
    target_link_options(athena_core PUBLIC ${ATHENA_SANITIZE_FLAGS})
endif()

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
        "${ATHENA_YAML_TARGET}"
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

    # The TLS listener tests need real certificates, and the working directory a test
    # runs in is the build tree rather than the source one.
    target_compile_definitions(athenasip_tests PRIVATE ATHENA_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")

    if(MSVC AND TARGET OpenSSL::applink)
        target_link_libraries(athenasip_tests PRIVATE OpenSSL::applink)
    endif()

    include(GoogleTest)
    gtest_discover_tests(athenasip_tests DISCOVERY_TIMEOUT 60)
endif()
# --- Install ------------------------------------------------------------------------
#
# What a package puts on a host: the binary, a unit, and a configuration file that is
# never overwritten once it exists. Principle 3 says a node should be easy to install,
# and "easy" includes "upgrading does not silently replace what you configured".

include(GNUInstallDirs)

# Where a node's live configuration lives. /etc for a system prefix, and under the
# prefix for anything else, so a --prefix=$HOME/opt install is self-contained.
if(NOT DEFINED ATHENA_CONFIG_INSTALL_DIR)
    if(CMAKE_INSTALL_PREFIX STREQUAL "/usr" OR CMAKE_INSTALL_PREFIX STREQUAL "/usr/local")
        set(ATHENA_CONFIG_INSTALL_DIR "/etc/athenasip")
    else()
        set(ATHENA_CONFIG_INSTALL_DIR "${CMAKE_INSTALL_PREFIX}/etc/athenasip")
    endif()
endif()

install(TARGETS athenasip RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The example is installed as a reference under the docs, always, so a host carries the
# annotated original whatever its live configuration has become.
install(FILES config/config.example.yaml
        DESTINATION ${CMAKE_INSTALL_DOCDIR}
        RENAME config.example.yaml)

# And as the live configuration only where there is none. An install that clobbered it
# would take the node down on upgrade, which is the worst possible moment.
#
# DESTDIR is honoured by hand here, because install(CODE) writes the paths it is given
# and nothing prepends it for us. Without this a packager staging a build would have
# the real /etc/athenasip written by a build that was never meant to touch the host.
install(CODE "
    set(ATHENA_CONFIG_DIR \"${ATHENA_CONFIG_INSTALL_DIR}\")

    if(NOT \"\$ENV{DESTDIR}\" STREQUAL \"\")
        set(ATHENA_CONFIG_DIR \"\$ENV{DESTDIR}/\${ATHENA_CONFIG_DIR}\")
    endif()

    if(EXISTS \"\${ATHENA_CONFIG_DIR}/config.yaml\")
        message(STATUS \"Keeping existing \${ATHENA_CONFIG_DIR}/config.yaml\")
    else()
        message(STATUS \"Installing \${ATHENA_CONFIG_DIR}/config.yaml\")
        file(MAKE_DIRECTORY \"\${ATHENA_CONFIG_DIR}\")
        file(COPY_FILE \"${CMAKE_CURRENT_SOURCE_DIR}/config/config.example.yaml\"
                       \"\${ATHENA_CONFIG_DIR}/config.yaml\")
    endif()
")

# The unit, where systemd looks for one that came from a package rather than from an
# administrator. ATHENA_SYSTEMD_UNIT_DIR moves it for a distribution that disagrees.
if(NOT DEFINED ATHENA_SYSTEMD_UNIT_DIR)
    set(ATHENA_SYSTEMD_UNIT_DIR "${CMAKE_INSTALL_PREFIX}/lib/systemd/system")

    if(CMAKE_INSTALL_PREFIX STREQUAL "/usr" OR CMAKE_INSTALL_PREFIX STREQUAL "/usr/local")
        set(ATHENA_SYSTEMD_UNIT_DIR "/lib/systemd/system")
    endif()
endif()

if(UNIX AND NOT APPLE)
    # Generated rather than copied, so ExecStart names where the binary actually went
    # and the unit cannot drift from the install prefix.
    configure_file(packaging/athenasip.service.in
                   ${CMAKE_CURRENT_BINARY_DIR}/athenasip.service @ONLY)

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/athenasip.service
            DESTINATION ${ATHENA_SYSTEMD_UNIT_DIR})
endif()
