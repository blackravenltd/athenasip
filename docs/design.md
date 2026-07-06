# AthenaSIP - Design

## Transport Abstractions

AthenaSIP separates transport handling into three clear layers: **Server**, **Connection**, and **Channel**. Each plays a distinct role in managing the flow of SIP messages without coupling to the specifics of the underlying transport protocol.

---

### Node

A `Node` is an instance of AthenaSIP running on one physical host. Typically, this is a cloud instance or container.

### Server

A `Server` is responsible for listening on a specific transport protocol and network interface. It handles incoming traffic or connection requests and creates `Connection` instances accordingly. For example, a `UDPServer` monitors packets from remote endpoints, while a `TCPServer` accepts new streams. Each server operates independently but provides a unified interface to the SIP system.

Servers are local to nodes and not shared between them.

* The Server abstract class [server.h](src/servers/server.h)

---

### Connection

A `Connection` represents a transport-level communication path between AthenaSIP and a remote peer. It abstracts the underlying transport (e.g., socket streams or datagram endpoints), providing a uniform view of data I/O. For connection-oriented protocols like TCP, it wraps a socket; for connectionless protocols like UDP, it represents a specific remote address. `Connection` is intentionally kept at the byte-buffer level and has no knowledge of SIP or message semantics.

Connections are local to servers and not shared between them.

* The Connection abstract class [server.h](src/servers/connection.h)

---

### Channel

A `Channel` is the first level to operate with SIP awareness. It builds upon `Connection` and is responsible for converting byte streams into complete SIP messages and vice versa. A channel manages message framing, parsing, and serialization. It forms the boundary between the transport layer and the SIP core. Importantly, it is not a SIP session or dialog, but simply a message-level conduit.

A Channel is composed of exactly one connection.

* The Channel class [channel.h](src/channel.h)

---

### Design Intent

These abstractions ensure AthenaSIP remains transport-agnostic at its core. Nodes form the cluster, Servers manage listening and setup, Connections manage raw communication, and Channels manage protocol framing. This separation allows AthenaSIP to support multiple protocols cleanly and makes it easy to extend or replace components as needed.
