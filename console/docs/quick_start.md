# AthenaSIP Admin - Quick Start

```
npm install
npm run dev
```

Open the address Vite prints and sign in. No node is needed: the console runs against an
in-memory fake that enforces the node's rules, seeded with realms, subscribers, registrations
and calls.

| User | Password | Roles |
| --- | --- | --- |
| `admin` | `admin` | All |
| `ops` | `ops` | View cluster status |
| `helpdesk` | `helpdesk` | Manage subscribers |
| `newhire` | `newhire` | None |

## Against a real node

```
VITE_ATHENASIP_LIVE=true npm run dev
```

The dev server proxies `/api` to `http://127.0.0.1:8080`; set `ATHENASIP_API` to point
elsewhere. Sign in as a user of that node (see [configuration.md](configuration.md)).

## The screens

| Screen | What it shows |
| --- | --- |
| Overview | The node's health, version and transports, and every node in the cluster. |
| Phone | A browser phone, registered as a subscriber. See [softphone.md](softphone.md). |
| SIP | Realms and their behaviour, subscribers, registrations and probed clients. |
| Calls | Live calls with per-leg packet rates, and a flag for one-way audio. |
| Media | The media engine, each realm's media policy, and phones that refused an offer. |
| Security | Which listeners are encrypted and which are not. |
| Users | Who can sign in, and their roles. |
| Diagnostics | The softphone as the end-to-end run sees it, with the full negotiation. |
