# AthenaSIP - Scripting

AthenaSIP is programmable via the [Lua](https://www.lua.org/) language:

* Authentication
* Routing
* Call Forwarding
* Media Control

## Configuration


## Files

## AthenaSIP Global Functions and Tables

### include

```lua
include("/path/to/script")
```

The `include` function loads the file supplied in the argument.

### print

```lua
print("Any String")
```

The `print` function outputs to the log at INFO level. This function is an alias for `log.info()` (see below).

### log

```lua
log.debug("A debug log message")
log.info("A info log message")
log.warn("A warn log message")
log.error("A error log message")
```

The `log` table exposes the logger:

* `log.debug(string)` - Log the supplied string at DEBUG level
* `log.info(string)` - Log the supplied string at INFO level
* `log.warn(string)` - Log the supplied string at WARN level
* `log.error(string)` - Log the supplied string at ERROR level
