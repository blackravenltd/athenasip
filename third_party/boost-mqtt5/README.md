# Boost.MQTT5, vendored

The `mqtt://` event system's MQTT client: Boost.MQTT5, header-only, copied unchanged from the
Boost 1.89.0 release (`boost_1_89_0.tar.gz`, SHA-256
`9de758db755e8330a01d995b0a24d09798048400ac25c03fc5ea9be364b13c93`, from
`https://archives.boost.io/release/1.89.0/source/`): `boost/mqtt5/` and `boost/mqtt5.hpp`, and
the licence, `LICENSE_1_0.txt` (the Boost Software License 1.0).

AthenaSIP builds on Debian 13's Boost 1.83, as the rest of the stack does (decided 2026-11-23),
and Boost.MQTT5 first shipped in 1.88. It builds against 1.83's Asio as it is; nothing here is
changed. The build searches these headers before the system's.

A bump is a deliberate commit: take the headers from the new release's tarball, read its release
notes against `src/events/mqtt_event_system.cpp`, and record the version and checksum here.
