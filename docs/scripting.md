# AthenaSIP - Scripting

**Scripting is parked.** There is no scripting in AthenaSIP today, and the Lua sources
in `src/script/` are not compiled: `CMakeLists.txt` filters them out, and the runtime
carries no Lua dependency.

This page is kept so the decision is written down where somebody looking for scripting
will find it.

## Why

Scripting was in the tree before it had a use. A routing language is a large surface -
an API, a sandbox, a lifecycle, a debugging story - and every one of those is a promise
to the people who write against it. Carrying that promise for a feature nothing in the
project needed yet was the wrong order.

## What it comes back as

Routing policy is what scripting was for, and routing policy is a plugin kind. When it
returns it registers through the same contract as `Datastore`, `EventSystem` and
`MediaEngine` - versioned, async, with its own configuration root - rather than being a
core feature with its own rules.

That also means Lua is not the only possible answer. A routing-policy plugin can be a
script host, a static table, or a service somewhere else, and the core does not have to
know which.

[`plugins.md`](plugins.md) is the contract it would use. `TODO/ACTIVE.md` has the
decision under Parked.
