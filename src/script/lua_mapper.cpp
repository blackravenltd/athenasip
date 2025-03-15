#include <lua.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "headers/header.h"
#include "headers/string_header.h"
#include "sip_header.h"
#include "sip_message.h"

namespace athenasip {

// Forward declarations of the Lua C functions for header get/set.
static int header_get(lua_State* L);
static int header_set(lua_State* L);

// Push a SIPMessage as a Lua table onto the Lua stack.
void push_sip_message(lua_State* L, const SIPMessage& msg) {
  // Create a new table for the SIPMessage.
  lua_newtable(L);

  // Set top-level fields.
  lua_pushstring(L, "callID");
  lua_pushstring(L, msg.callID.c_str());
  lua_settable(L, -3);

  lua_pushstring(L, "body");
  lua_pushstring(L, msg.body.c_str());
  lua_settable(L, -3);

  lua_pushstring(L, "body_length");
  lua_pushinteger(L, msg.body_length);
  lua_settable(L, -3);

  lua_pushstring(L, "branch");
  lua_pushstring(L, msg.branch.c_str());
  lua_settable(L, -3);

  lua_pushstring(L, "source_port");
  lua_pushinteger(L, msg.source_port);
  lua_settable(L, -3);

  lua_pushstring(L, "authenticated");
  lua_pushboolean(L, msg.authenticated);
  lua_settable(L, -3);

  // --- Convert SIPHeader into a nested Lua table ---
  // Create a new table for the header.
  if (msg.header) {
    lua_pushstring(L, "header");
    lua_newtable(L);  // header table

    // Copy simple fields from SIPHeader.
    lua_pushstring(L, "request_method");
    lua_pushstring(L, msg.header->request_method.c_str());
    lua_settable(L, -3);

    lua_pushstring(L, "sip_version");
    lua_pushstring(L, msg.header->sip_version.c_str());
    lua_settable(L, -3);

    // Create a subtable to hold all header fields.
    lua_pushstring(L, "_fields");
    lua_newtable(L);
    // Iterate over headers_map and insert each header.
    for (const auto& kv : msg.header->headers_map) {
      const std::string& key = kv.first;
      const auto& vec = kv.second;
      // If only one header value, store it as a string.
      if (vec.size() == 1) {
        std::string val = vec.front()->to_string();
        lua_pushstring(L, val.c_str());
        lua_setfield(L, -2, key.c_str());
      } else {
        // If more than one, store as an array.
        lua_newtable(L);
        int idx = 1;
        for (const auto& hdr : vec) {
          lua_pushinteger(L, idx++);
          lua_pushstring(L, hdr->to_string().c_str());
          lua_settable(L, -3);
        }
        lua_setfield(L, -2, key.c_str());
      }
    }
    // Set the _fields subtable.
    lua_settable(L, -3);  // header["_fields"] = (the table we just built)

    // Register header helper functions:
    // get: function(self, headerName)
    lua_pushcfunction(L, header_get);
    lua_setfield(L, -2, "get");

    // set: function(self, headerName, value)
    lua_pushcfunction(L, header_set);
    lua_setfield(L, -2, "set");

    // Set the header table into the SIPMessage table.
    lua_settable(L, -3);
  }
}

// Lua C function: header_get(self, key)
// Retrieves the header value(s) from the _fields subtable.
static int header_get(lua_State* L) {
  // Argument 1: header table; Argument 2: key
  const char* key = luaL_checkstring(L, 2);
  // Get the _fields table.
  lua_getfield(L, 1, "_fields");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_pushnil(L);
    return 1;
  }
  lua_getfield(L, -1, key);
  // Return whatever is found (or nil if not set).
  return 1;
}

// Lua C function: header_set(self, key, value)
// Sets a header value in the _fields subtable.
static int header_set(lua_State* L) {
  // Argument 1: header table; Argument 2: key; Argument 3: value
  const char* key = luaL_checkstring(L, 2);
  // Get the _fields table.
  lua_getfield(L, 1, "_fields");
  if (!lua_istable(L, -1)) {
    // If _fields doesn't exist, create one.
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setfield(L, 1, "_fields");
  }
  // Now _fields is at the top of the stack.
  // Set _fields[key] = value.
  lua_pushvalue(L, 3);
  lua_setfield(L, -2, key);
  lua_pop(L, 1);  // pop _fields
  return 0;
}

// Convert a Lua table (assumed to be at the top of the stack) into a SIPMessage instance.
SIPMessage table_to_sip_message(lua_State* L) {
  SIPMessage msg;

  // Extract top-level fields.
  lua_getfield(L, -1, "callID");
  if (lua_isstring(L, -1)) msg.callID = lua_tostring(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, -1, "body");
  if (lua_isstring(L, -1)) msg.body = lua_tostring(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, -1, "body_length");
  if (lua_isnumber(L, -1)) msg.body_length = static_cast<unsigned int>(lua_tointeger(L, -1));
  lua_pop(L, 1);

  lua_getfield(L, -1, "branch");
  if (lua_isstring(L, -1)) msg.branch = lua_tostring(L, -1);
  lua_pop(L, 1);

  lua_getfield(L, -1, "source_port");
  if (lua_isnumber(L, -1)) msg.source_port = static_cast<uint16_t>(lua_tointeger(L, -1));
  lua_pop(L, 1);

  lua_getfield(L, -1, "authenticated");
  if (lua_isboolean(L, -1)) msg.authenticated = lua_toboolean(L, -1);
  lua_pop(L, 1);

  // --- Convert the header table back into SIPHeader ---
  lua_getfield(L, -1, "header");  // push header table
  if (lua_istable(L, -1)) {
    auto header = std::make_shared<SIPHeader>();

    lua_getfield(L, -1, "request_method");
    if (lua_isstring(L, -1)) header->request_method = lua_tostring(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, -1, "sip_version");
    if (lua_isstring(L, -1)) header->sip_version = lua_tostring(L, -1);
    lua_pop(L, 1);

    // Retrieve the _fields subtable.
    lua_getfield(L, -1, "_fields");
    if (lua_istable(L, -1)) {
      // Iterate over _fields.
      lua_pushnil(L);
      while (lua_next(L, -2) != 0) {
        // Now key is at -2 and value at -1.
        if (lua_isstring(L, -2)) {
          std::string key = lua_tostring(L, -2);
          // If value is a table, iterate numeric indices.
          if (lua_istable(L, -1)) {
            std::vector<std::shared_ptr<headers::Header>> hdrs;
            int n = lua_rawlen(L, -1);
            for (int i = 1; i <= n; ++i) {
              lua_rawgeti(L, -1, i);
              if (lua_isstring(L, -1)) {
                std::string val = lua_tostring(L, -1);
                auto hdr = headers::Header::create(key, val);
                hdrs.push_back(hdr);
              }
              lua_pop(L, 1);
            }
            if (!hdrs.empty()) {
              header->headers_map[key] = hdrs;
              for (const auto& hdr : hdrs) header->headers.push_back({key, hdr});
            }
          } else if (lua_isstring(L, -1)) {
            std::string val = lua_tostring(L, -1);
            auto hdr = headers::Header::create(key, val);
            header->headers_map[key] = std::vector<std::shared_ptr<headers::Header>>{hdr};
            header->headers.push_back({key, hdr});
          }
        }
        lua_pop(L, 1);  // pop value, keep key for next iteration.
      }
    }
    lua_pop(L, 1);  // pop _fields table.
    msg.header = header;
  }
  lua_pop(L, 1);  // pop header table

  return msg;
}

}  // namespace athenasip
