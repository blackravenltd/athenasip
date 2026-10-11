#!/bin/sh
#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# Sourced by run.sh and cluster.sh for --lua: each node's configuration again with
# policy.url lua:// and no scripts of its own, so it runs the standard scripts, and a
# compose overlay mounting it in place of the original. The scenarios are unchanged: a
# scenario that passes on builtin:// and fails here is the scripts disagreeing.

LUA_DIR="test/e2e/generated-lua"

lua_begin() {
  rm -rf "${LUA_DIR}"
  mkdir -p "${LUA_DIR}"
  printf 'services:\n' >"${LUA_DIR}/compose.yml"
}

# lua_node <service> <its configuration directory>
lua_node() {
  mkdir -p "${LUA_DIR}/$1"
  cp "$2/config.yaml" "${LUA_DIR}/$1/config.yaml"
  printf '\npolicy:\n  url: "lua://"\n' >>"${LUA_DIR}/$1/config.yaml"
  printf '  %s:\n    volumes:\n      - ./%s/%s:/root/.athenasip:ro\n' "$1" "${LUA_DIR}" "$1" >>"${LUA_DIR}/compose.yml"
}
