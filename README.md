![AthenaSIP Logo](docs/logos/athenasip_small_white.png)

# AthenaSIP

**Project Status: ALPHA - DO NOT USE**

AthenaSIP is a **modern, cloud-native SIP server** designed for security, scalability, and ease of use. It provides **out-of-the-box** support for SIP signaling, secure media relay, and event-driven processing, with a flexible architecture that adapts to both **standalone** and **distributed** deployments.

## Key Features
* **Secure by Default** – TLS-only SIP, with SRTP and DTLS-SRTP enforced.
* **Standalone or Pluggable** – Ready-to-use with built-in components but fully extensible for scaling using industry-standard OSS databases, event systems and media proxies.  
* **Cloud-Native & Scalable** – Stateless design enables seamless scaling and clustering.
* **Minimal Configuration** – Designed for rapid deployment with automatic defaults.
* **API-Driven** – Exposes a RESTful API for management, monitoring, and automation, works with [AthenaSIP Admin](https://github.com/blackravenltd/athenasip-admin) - a React based administration client. 
* **WebRTC-Ready** *(Future)* – Planned support for SIP over WebSockets, STUN/TURN/ICE.


## Documentation

* [Installation](docs/installation.md)
* [Quick Start](docs/quick_start.md)
* [Configuration](docs/configuration.md)
* [Architecture](docs/architecture.md)

For more information, please see the [docs](docs/) directory.

## Project Goals

### Security & Privacy First
AthenaSIP is built with **strong security defaults**—SIP communication is **TLS-only by default**, with **SRTP and DTLS-SRTP enforced** for media. Plaintext SIP (UDP/TCP) and RTP must be explicitly enabled. The server also ensures **strict cipher policies** and modern cryptographic standards.

### Self-Contained Yet Extensible
AthenaSIP includes everything needed for a complete SIP solution **out of the box**—an integrated **database, eventing system, and RTP relay**—allowing immediate use without complex setup. However, all major components are **pluggable**, allowing users to swap in external datastores, event systems, and media relays as needed.

### Cloud-Native & Soft-Clusterable
Designed for **stateless** operation, AthenaSIP can scale horizontally, making it suitable for **containerized and distributed environments** like Kubernetes. It allows **soft clustering**, meaning it can dynamically distribute SIP signaling and media handling across multiple nodes.

### Minimal Configuration, Maximum Usability
AthenaSIP is designed to **work out of the box** with sensible defaults. This makes it easy to evaluate, deploy, and integrate into existing infrastructures **without deep SIP expertise**.

### API-First & Automation-Ready
AthenaSIP exposes a **RESTful JSON API** for managing SIP routing, user authentication, monitoring, and call handling. This enables easy integration with external **admin interfaces, analytics platforms, and automation tools**.

### Future-Proof & WebRTC-Ready *(Planned)*
Future releases will introduce **SIP over WebSockets (RFC 7118), STUN/TURN/ICE support**, and advanced WebRTC capabilities—allowing seamless interoperability between **SIP-based systems and browser-based clients**.

## License

AthenaSIP is licensed under [GPLv3](https://www.gnu.org/licenses/gpl-3.0.en.html). Please see the [LICENSE](LICENSE) file.
