# Connectivity C ABI

`include/gateway/connectivity.h` defines the additive version-1 C ABI for
opaque connectivity resource handles. It exposes only resource IDs, lifecycle
state, ownership, and error codes; C++ manager internals are hidden.

Handles are owned by the caller and released with
`emaster_connectivity_destroy_connection` or
`emaster_connectivity_destroy_owner`. Input strings are borrowed for each
call. The implementation serializes access with a mutex, catches exceptions at
every exported boundary, and uses a bounded copy API for error text.

The ABI currently models REST, MQTT, and RabbitMQ resources. It is an
ownership/lifecycle bridge only; transport operations remain in the Rust and
C++ connectivity managers until compatibility integration is completed.
