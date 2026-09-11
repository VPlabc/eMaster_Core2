# eMaster developer API

This document describes the application-facing Rust boundary. Applications
must use these host APIs rather than retaining native client objects or writing
configuration files directly.

## Application lifecycle

`emaster_lua::ApplicationRuntime` composes the application manager with the
connectivity and configuration managers:

```text
register(id, version)
start(id)
    create owned resources
stop(id) / fail(id)
    connectivity resources are destroyed
remove(id)
    active configuration ownership is released
```

Application IDs are the ownership boundary. A resource created for one ID
cannot be accessed by another ID. Persisted JSON configuration is retained
when an application is removed so an operator can recover or explicitly purge
it according to product policy.

## Connectivity

`ConnectivityRuntime` exposes opaque numeric handles for REST, MQTT, and
RabbitMQ. The supported lifecycle is:

```text
create -> start -> stop/restart -> destroy
```

Every connection configuration must contain the same `owner` as the
application creating it. Native transport objects, Tokio runtimes, worker
threads, and C++ implementation details never cross this boundary.

The managers provide independent resources with status/list operations,
timeouts, retry settings, statistics, and owner cleanup. Production adapters
are enabled with the `emaster-lua/production-transports` feature; tests should
inject deterministic transports instead.

## Dynamic configuration

Applications declare an `emaster_config::Schema` containing a version and
typed fields. Supported field types include string, password, number, integer,
boolean, URL, and port. Required fields and defaults are validated before a
value is stored.

Use `ConfigRuntime` for declaration, read/update, JSON save/reload, and change
events. The web UI consumes the same schema through the application-config API
and renders controls dynamically. Do not add application-specific cards to
`web/config.html`.

## Error and security rules

- Treat every handle and owner check as mandatory; do not bypass the host API.
- Handle connectivity failures in the application and allow other applications
  to continue running.
- Keep credentials in configuration or the secure deployment data directory,
  never in Lua source, release archives, or target profiles.
- Validate URLs, topics, queue names, ports, payload sizes, and configuration
  types before invoking a transport.
- Do not use `unwrap`, `expect`, or `panic` for external input or lifecycle
  operations.

## Release workflow

Build a target-specific release, run its tests, then package and verify it:

```text
build -> test -> package -> verify-package -> publish
```

Packages must contain a manifest and checksum and must exclude databases,
credentials, source, tests, and build artifacts. OTA installation stages a
versioned release, verifies its checksum/signature, activates it through the
managed layout, and keeps a rollback release available.
