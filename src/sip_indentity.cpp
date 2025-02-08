//
// AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
//
// Copyright (C) 2024 Tom Cully <mail@tomcully.com>
// Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
//

#include "sip_identity.h"
#include <regex>
#include <sstream>

namespace athenasip {

// Default constructor
SipIdentity::SipIdentity() : _scheme("sip"), _user(""), _host(""), _port(std::nullopt), _password(std::nullopt) {}

// Constructor with parsing
SipIdentity::SipIdentity(const std::string& identity) {
    parse(identity);
}

// Accessors
std::optional<std::string> SipIdentity::display_name() const {
    return _display_name;
}

std::string SipIdentity::scheme() const {
    return _scheme;
}

std::optional<std::string> SipIdentity::user() const {
    return _user.empty() ? std::nullopt : std::optional<std::string>(user_);
}

std::optional<std::string> SipIdentity::password() const {
    return _password;
}

std::string SipIdentity::host() const {
    return _host;
}

std::optional<int> SipIdentity::port() const {
    return _port;
}

std::optional<std::string> SipIdentity::parameters() const {
    return _parameters.empty() ? std::nullopt : std::optional<std::string>(parameters_);
}

std::optional<std::string> SipIdentity::headers() const {
    return _headers.empty() ? std::nullopt : std::optional<std::string>(headers_);
}

std::string SipIdentity::to_string() const {
    std::ostringstream oss;
    if (_display_name) {
        oss << *_display_name << " ";
    }
    oss << "<" << _scheme << ":";

    if (!user_.empty()) {
        oss << _user;
        if (password_) {
            oss << ":" << *password_;
        }
        oss << "@";
    }

    oss << _host;
    
    if (port_) {
        oss << ":" << *port_;
    }

    if (!parameters_.empty()) {
        oss << ";" << _parameters;
    }

    if (!headers_.empty()) {
        oss << "?" << _headers;
    }

    oss << ">";
    return oss.str();
}

// Mutators
void SipIdentity::set_display_name(const std::string& name) {
    _display_name = name;
}

void SipIdentity::set_scheme(const std::string& scheme) {
    _scheme = scheme;
}

void SipIdentity::set_user(const std::string& user) {
    _user = user;
}

void SipIdentity::set_password(const std::string& password) {
    _password = password;
}

void SipIdentity::set_host(const std::string& host) {
    _host = host;
}

void SipIdentity::set_port(int port) {
    _port = port;
}

void SipIdentity::set_parameters(const std::string& parameters) {
    _parameters = parameters;
}

void SipIdentity::set_headers(const std::string& headers) {
    _headers = headers;
}

void SipIdentity::clear_display_name() {
    _display_name.reset();
}

// Private parsing function
void SipIdentity::parse(const std::string& identity) {
    static const std::regex sip_regex(R"((?:\"?([^\"]+)\"?\s*)?<([a-zA-Z]+):(?:([^:@]+)(?::([^@]+))?@)?([^:;?]+)(?::(\d+))?(;[^?]*)?(\?.*)?>)");
    std::smatch match;

    if (std::regex_match(identity, match, sip_regex)) {
        _display_name = match[1].matched ? std::optional<std::string>(match[1].str()) : std::nullopt;
        _scheme = match[2].str();
        _user = match[3].matched ? match[3].str() : "";
        _password = match[4].matched ? std::optional<std::string>(match[4].str()) : std::nullopt;
        _host = match[5].str();
        _port = match[6].matched ? std::optional<int>(std::stoi(match[6].str())) : std::nullopt;
        _parameters = match[7].matched ? match[7].str().substr(1) : "";  // Remove leading ";"
        _headers = match[8].matched ? match[8].str().substr(1) : "";  // Remove leading "?"
    } else {
        _display_name.reset();
        _scheme = "sip";
        _user.clear();
        _password.reset();
        _host = identity;
        _port.reset();
        _parameters.clear();
        _headers.clear();
    }
}

}
