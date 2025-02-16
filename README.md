![AthenaSIP Logo](docs/athenasip_small_white.png)

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

### Debain (RedHat)

*TODO*

### Linux (RedHat)

*TODO*

### MacOSX

*TODO*

### Build From Source

#### Install Dependencies

**Debain**
```sh
sudo apt install -y build-essential cmake libboost-all-dev libssl-dev
```

**MacOS**
```sh
brew install cmake boost openssl
```

#### Compile

```sh
mkdir build && cd build
cmake ..
make
```

## License

AthenaSIP is liscensed under [GPLv3](https://www.gnu.org/licenses/gpl-3.0.en.html). Please see the [LICENSE](LICENSE) file.



