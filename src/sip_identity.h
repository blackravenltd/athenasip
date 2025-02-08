//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//
#pragma once

#include <string>
#include <optional>

namespace athenasip {

class SipIdentity {
public:
    // Constructors
    SipIdentity();
    explicit SipIdentity(const std::string& identity);

    // Accessors
    std::optional<std::string> display_name() const;
    std::string scheme() const;
    std::optional<std::string> user() const;
    std::optional<std::string> password() const;
    std::string host() const;
    std::optional<uint16_t> port() const;
    std::optional<std::string> parameters() const;
    std::optional<std::string> headers() const;

    std::string to_string() const;

    // Mutators
    void set_display_name(const std::string& name);
    void set_scheme(const std::string& scheme);
    void set_user(const std::string& user);
    void set_password(const std::string& password);
    void set_host(const std::string& host);
    void set_port(uint16_t port);
    void set_parameters(const std::string& parameters);
    void set_headers(const std::string& headers);
    void clear_display_name();

private:
    void parse(const std::string& identity);

    // SIP URI components
    std::optional<std::string> _display_name;
    std::string _scheme;
    std::string _user;
    std::optional<std::string> _password;
    std::string _host;
    std::optional<uint16_t> _port;
    std::string _parameters;
    std::string _headers;
};

}