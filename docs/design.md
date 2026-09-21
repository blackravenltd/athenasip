# AthenaSIP - Design

## Transport Abstractions

AthenaSIP separates transport handling into three clear layers: **Server**, **Connection**, and **Channel**. Each plays a distinct role in managing the flow of SIP messages without coupling to the specifics of the underlying transport protocol.

---

### Node

A `Node` is an instance of AthenaSIP running on one physical host. Typically, this is a cloud instance or container.

### Server

A `Server` is responsible for listening on a specific transport protocol and network interface. It handles incoming traffic or connection requests and creates `Connection` instances accordingly. For example, a `UDPServer` monitors packets from remote endpoints, while a `TCPServer` accepts new streams. Each server operates independently but provides a unified interface to the SIP system.

Servers are local to nodes and not shared between them.

* The Server abstract class: [`src/servers/server.h`](../src/servers/server.h)

---

### Connection

A `Connection` represents a transport-level communication path between AthenaSIP and a remote peer. It abstracts the underlying transport (e.g., socket streams or datagram endpoints), providing a uniform view of data I/O. For connection-oriented protocols like TCP, it wraps a socket; for connectionless protocols like UDP, it represents a specific remote address. `Connection` is intentionally kept at the byte-buffer level and has no knowledge of SIP or message semantics.

Connections are local to servers and not shared between them.

A connection belongs to the thread its server's `io_context` runs on, and a socket is not safe for two threads at once. Everything that touches the stream - starting a read, starting a write, closing it - runs on `Connection::executor()`, and the Core strand hands the work over rather than doing it itself.

* The Connection abstract class: [`src/servers/connection.h`](../src/servers/connection.h)

---

### Channel

A `Channel` is the first level to operate with SIP awareness. It builds upon `Connection` and is responsible for converting byte streams into complete SIP messages and vice versa. A channel manages message framing, parsing, and serialization. It forms the boundary between the transport layer and the SIP core. Importantly, it is not a SIP session or dialog, but simply a message-level conduit.

A Channel is composed of exactly one connection, and is known by one name: `transport://host:port`. That name is the key the channel registry files it under, the key `Core::channel_find` answers to, and the flow id a registration binding records (RFC 5626), so that a node holding a binding can find the connection again without re-resolving a Contact that, for a browser or a NAT'd client, resolves to nothing reachable.

A channel writes one message at a time. Two writes in flight on one socket interleave their bytes, so anything sent while a write is outstanding waits behind it.

* The Channel class: [`src/channel.h`](../src/channel.h)

---

### Design Intent

These abstractions ensure AthenaSIP remains transport-agnostic at its core. Nodes form the cluster, Servers manage listening and setup, Connections manage raw communication, and Channels manage protocol framing. This separation allows AthenaSIP to support multiple protocols cleanly and makes it easy to extend or replace components as needed.

Above the transport are the transaction layer and the transaction users, which
[`architecture.md`](architecture.md) describes.
