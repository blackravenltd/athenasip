![AthenaSIP Logo](docs/logos/athenasip_small_white.png)

# AthenaSIP

**Project Status: ALPHA - DO NOT USE**

AthenaSIP is an open-source SIP ([RFC3261](https://datatracker.ietf.org/doc/html/rfc3261)) implementation designed around 
the following principles:

* Security & Privacy – TLS-only, with no support for insecure TCP/UDP.
* Minimalism – Focused on core SIP functionality without unnecessary features.
* Ease of Use – Works out of the box for common use cases with minimal configuration.
* Cloud-Native Design – Built for deployment in cloud environments.
* Scalability – Maintains minimal internal state to support efficient scaling.

## Quick Start

Follow the [Quick Start Instructions](docs/quick_start.md) to quickly evaluate AthenaSIP on your local network. 

## Installation

AthenaSIP is not yet available in pre-built packages.

### Build From Source

#### Install Dependencies

**Debain (Ubuntu)**

```sh
sudo apt-get update
sudo apt-get install -y libboost-system-dev libboost-thread-dev libssl-dev pkg-config libpqxx-dev libmysqlcppconn-dev libyaml-cpp-dev libtinyxml2-dev
```

**Linux (RedHat - Fedora)**

```sh
sudo dnf install boost-devel openssl-devel pkg-config libpqxx-devel mysql-connector-c++-devel yaml-cpp-devel tinyxml2-devel
```

**Linux (RedHat - CentOS/RHEL)**

```sh
sudo yum install boost-devel openssl-devel pkgconfig libpqxx-devel mysql-connector-c++-devel yaml-cpp-devel tinyxml2-devel
```

**MacOSX**

```sh
brew install boost openssl pkg-config libpqxx mysql-connector-c++
```

#### Compile

```sh
mkdir build && cd build
cmake ..
make
```

## License

AthenaSIP is licensed under [GPLv3](https://www.gnu.org/licenses/gpl-3.0.en.html). Please see the [LICENSE](LICENSE) file.



