# AthenaSIP - Quick Start

Pick the guide for what you want to do. Each starts from nothing and ends with a call
going through.

| Guide | What you end up with | Needs |
|---|---|---|
| [Try it in Docker](quick-start/docker.md) | A full stack in one command: Redis, Mosquitto, rtpengine and coturn, an administrator and two subscribers | Docker |
| [One node by hand](quick-start/one-node.md) | One process with nothing external, provisioned step by step over the admin API | A build, or Docker |
| [A server on Linux](quick-start/linux-server.md) | A node run by systemd as its own user, data in Redis, TLS and HTTPS | A Debian or Ubuntu host |
| [Connecting phones](quick-start/phones.md) | Desk phones and softphones registered and calling, on the node's network and off it | A running node |
| [Calling from a browser](quick-start/browser.md) | Browsers calling each other and phones, through rtpengine, secure WebSocket and TURN | A running node, a certificate |
| [Running a cluster](clustering.md) | Two nodes sharing subscribers, where a client whose node dies registers again through the other | Two hosts, Redis, an MQTT broker |

If you are new to SIP, [How a call works](how-a-call-works.md) follows one call through a
node and the [Glossary](glossary.md) has the words.
