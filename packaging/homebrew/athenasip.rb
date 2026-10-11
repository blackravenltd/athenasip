#
# AthenaSIP - Secure, Minimal, Cloud-Native SIP Server
#
# Copyright (C) 2026 Tom Cully <mail@tomcully.com>
# Licensed under the GNU GPLv3 – see <https://www.gnu.org/licenses/gpl-3.0.html>
#
# The Homebrew formula. It lives in the tap repository blackravenltd/homebrew-athenasip as
# Formula/athenasip.rb; this copy is the source of it. docs/releasing.md says how to update
# the url and sha256 for a release.

class Athenasip < Formula
  desc "Secure, minimal, cloud-native SIP server"
  homepage "https://github.com/blackravenltd/athenasip"
  url "https://github.com/blackravenltd/athenasip/archive/refs/tags/VERSION.tar.gz"
  sha256 "SHA256"
  license "GPL-3.0-only"
  head "https://github.com/blackravenltd/athenasip.git", branch: "main"

  depends_on "cmake" => :build
  depends_on "pkgconf" => :build
  depends_on "boost"
  depends_on "libnghttp2"
  depends_on "lua@5.4"
  depends_on "openssl@3"
  depends_on "yaml-cpp"

  def install
    system "cmake", "-S", ".", "-B", "build", *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  def caveats
    <<~EOS
      The configuration is #{etc}/athenasip/config.yaml, the annotated example. Check it, make
      the first administrator, and start the node:

        athenasip --config #{etc}/athenasip/config.yaml --check
        athenasip --config #{etc}/athenasip/config.yaml --add-user admin
        brew services start athenasip

      With the example's memory:// datastore, --add-user carries on as the node; set a
      persistent datastore such as redis:// first, or start it that way instead of as a service.
    EOS
  end

  service do
    run [opt_bin/"athenasip", "--config", etc/"athenasip/config.yaml"]
    keep_alive true
    log_path var/"log/athenasip.log"
    error_log_path var/"log/athenasip.log"
  end

  test do
    assert_equal version.to_s, shell_output("#{bin}/athenasip --version").strip
    assert_match "builtin://", shell_output("#{bin}/athenasip --config #{etc}/athenasip/config.yaml --print-config")
  end
end
