# AthenaSIP - Scripting

AthenaSIP has no scripting yet. The Lua sources in `src/script/` are excluded from the
build by `CMakeLists.txt`, and the binary carries no Lua dependency.

Routing and authorisation are to become Lua scripts, with the SIP mechanics staying
native. The design is in two parts: [what the node provides](design/scripting-1-the-node.md)
and [the engine and the scripts](design/scripting-2-the-engine.md). This page becomes
the reference when it lands.
