# AthenaSIP - Scripting

AthenaSIP has no scripting. The Lua sources in `src/script/` are excluded from the build
by `CMakeLists.txt`, and the binary carries no Lua dependency.

Routing policy, which is what scripting was for, is intended to be a plugin kind
registered through the same contract as `Datastore`, `EventSystem` and `MediaEngine`. A
routing-policy plugin could be a script host, a static table or an external service.
[plugins.md](plugins.md) describes the contract.
