![AthenaSIP Logo](docs/logos/athenasip_small_white.png)

# AthenaSIP Admin

*v0.4.2*

**Project Status: ALPHA - DO NOT USE**

The administration console for [AthenaSIP](https://github.com/blackravenltd/athenasip), with
a browser phone. It is a static React bundle that the SIP server serves beside its admin API.

It needs an AthenaSIP node at 0.9.0 or later, and is tested against 0.12.0. Routing scripts,
trunks, the attributes scripts read and presence are not managed here yet: use the server's admin API and its
[scripting guide](https://github.com/blackravenltd/athenasip/blob/main/docs/scripting.md). See
[what the console does not manage](docs/configuration.md#what-the-console-does-not-manage).

## Features

* **Provisioning** - realms, subscribers, users and roles.
* **Live view** - registrations, calls, the media engine and the cluster.
* **A browser phone** - audio and video calls, dial pad, hold, mute, history and directory.
* **Low dependency** - React, React Router and JsSIP, with hand-written CSS.
* **Same origin** - the node serves the bundle and the API, so nothing is cross-origin.

## Quick start

```
npm install
npm run dev
```

This runs against an in-memory fake of the API. Sign in as `admin`, password `admin`.

```
npm test          # unit tests (Vitest)
npm run typecheck # tsc, both projects
npm run build     # typecheck, then a bundle in build/
npm run test:e2e  # browsers call through a running node; see docs/softphone.md
npm run test:athenasip  # everything unattended, as the server's suite runs it
```

## Documentation

* [Quick start](docs/quick_start.md)
* [Installation](docs/installation.md)
* [Configuration](docs/configuration.md)
* [Architecture](docs/architecture.md)
* [The phone, the harness page and the end-to-end runs](docs/softphone.md)
* [Goals](docs/goals.md)

Current work is in [TODO/ACTIVE.md](TODO/ACTIVE.md).

## License

[GPLv3](https://www.gnu.org/licenses/gpl-3.0.en.html). See [LICENSE](LICENSE).
