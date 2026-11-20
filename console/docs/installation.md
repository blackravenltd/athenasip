# AthenaSIP Admin - Installation

## Requirements

Node 20 or newer, to build. Nothing at run time: the result is static files.

An [AthenaSIP](https://github.com/blackravenltd/athenasip) node at 0.9.0 or later to serve
it. The phone reads its connection from `/api/v1/subscriber/{realm}/config`, which earlier
nodes do not have.

## Build

```
npm install
VITE_ATHENASIP_LIVE=true npm run build
```

The bundle lands in `build/`. Without `VITE_ATHENASIP_LIVE=true` it talks to the in-memory
fake, which is wrong for a deployment.

## Deploy

AthenaSIP serves the bundle from the listener that answers `/api/v1`. Point the server's
`config.yaml` at a directory and copy `build/` into it:

```yaml
http:
  address: 0.0.0.0
  port: 8080
  api:
    enable: true
  files:
    enable: true
    path: "../admin/"
```

The node needs no restart when the files change, and by default answers a client-side route
such as `/phone` with `index.html` (`http.files.spa`). Serving the bundle from anywhere else
makes the API cross-origin, which is not supported.

The server's Docker setup serves a build of this repository with `docker/up.sh --console`,
from `console/build` or `ATHENA_CONSOLE_DIR`.

## HTTPS

Browsers give a microphone or camera only to a secure context. For the phone to work, serve
the console over https (or reach it through `localhost`) and enable the node's `wss`
listener. The browser must trust the node's certificate for both.

## The first user

```
athenasip --add-user NAME            # on the node's host
athenasip --reset-password NAME      # for a lost password
```
