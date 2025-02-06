# AthenaSIP

**Project Status: ALPHA - DO NOT USE**

AthenaSIP is an open-source SIP implementation designed with the following key principles:

* Security & Privacy – TLS-only, with no support for insecure TCP/UDP.
* Minimalism – Focused on core SIP functionality without unnecessary features.
* Ease of Use – Works out of the box for common use cases with minimal configuration.
* Cloud-Native Design – Built for deployment in cloud environments.
* Scalability – Maintains minimal internal state to support efficient scaling.

## Installation

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

## Snakeoil Certificate

To ease testing and demonstration, a self-signed certificate is included at `tls/snakeoil.crt|key`.

**CAVEAT: Do Not Use This Certificate In Production.**

## License

AthenaSIP is liscensed under [GPLv3](https://www.gnu.org/licenses/gpl-3.0.en.html). Please see the [LICENSE](LICENSE) file.



