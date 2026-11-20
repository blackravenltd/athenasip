# AthenaSIP Admin - Architecture

## Layout

```
src/
  main.tsx      Mounts the app and chooses the AdminApi: HTTP, or the in-memory fake.
  App.tsx       The shell: top bar, section bar, routes, and the phone it holds.
  app/          routes.ts (every path, named once) and navigation.ts.
  api/          AdminApi, its HTTP implementation, the fake, types and errors.
  auth/         The session, roles, and the sign-in screens.
  screens/      One file per screen. Each takes its AdminApi as a prop.
  realms/       A realm's behaviour settings, as form fields and as words.
  phone/        The Phone section: host, screen, dial pad, ringing, devices, settings.
  softphone/    The JsSIP controller, the harness page and its contract.
  components/   SectionNav, Modal, Status, VolumeMeter.
  hooks/        useRefreshableAsync, useSubmit, usePagination.
  styles/       tokens.css (palette), base.css (shell), admin.css (surfaces).
e2e/            Playwright specs, run against a real node.
```

## The API

`AdminApi` (`src/api/AdminApi.ts`) is everything the console can ask of a node. Its contract
is the server's `docs/api/openapi.yaml`.

- `HttpAdminApi` is the only code that knows a URL, a verb or a status code.
  `contract.test.ts` checks every request it makes against the OpenAPI document in a sibling
  repository this directory is in, and skips when there is none.
- `FakeAdminApi` holds the same records in memory and enforces the node's rules. Development
  and every screen test run against it, so a test never passes against a fixture the app
  does not use.

Components never call `fetch`. A realm is addressed by name and a subscriber by user, never
by id: the server's ids are 64-bit and would arrive rounded.

## Authentication

The console is a client of the API as curl is. `POST /auth/login` returns a bearer token,
which `Session` (`src/auth/Session.ts`) holds in memory and drops at the node's expiry.

- A 401 on any request ends the session.
- A 403 re-reads `GET /session`, so a role removed mid-session leaves the navigation at once.
- A 429 or 503 never signs anyone out.

Navigation items and routes name the roles that admit them (`src/app/navigation.ts`,
`App.tsx`). A user with no roles sees a No permissions page, and can still use the phone.

## Screens

A list is read-only: nothing in a row is an input. Every change opens a `FormModal` or
`ConfirmModal` (`src/components/Modal.tsx`), which own focus, Escape and the busy state, and
runs through `useSubmit`.

`useRefreshableAsync` keeps the last value on screen during a refresh and shows a spinner
only on first load. Every request is aborted on unmount, and an abort is never an error.

## The phone and the softphone

`Softphone` (`src/softphone/Softphone.ts`) wraps JsSIP with no React in it and publishes a
snapshot after every change. It has two faces:

- **The Phone section** (`src/phone/`). `PhoneHost` is loaded on first use and stays mounted
  until sign-out, so a call outlives a change of screen.
- **The harness** (`SoftphoneScreen`), in the console under Diagnostics and alone on
  `softphone.html`, which the end-to-end runs drive.

JsSIP is injected through `SipStack`, so tests use a user agent that opens no socket. See
[softphone.md](softphone.md).

## Styling

Hand-written CSS, dark only. The accent colour marks position: the active item, the focused
control, the primary action. Green, amber and red are reserved for state, and nothing else
may use them.
