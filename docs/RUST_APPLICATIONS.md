# Application lifecycle and isolation

`emaster-runtime::ApplicationManager` gives each Lua/plugin application an
independent identity, version, state, variables, configuration ownership,
connection ownership, and cleanup-resource list.

Applications transition through registered, running, stopped, and failed
states. Every registered `Cleanup` resource is released when an application
stops or is removed. Connection ownership is cleared during stop, while
configuration ownership remains represented by the application until removal.
Lifecycle events are available through a standard channel for host/UI
integration.

The existing `RuntimeManager` API remains unchanged for compatibility with
current runtime callers.
