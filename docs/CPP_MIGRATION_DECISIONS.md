# C++ Migration Decisions

The C++ gateway remains the behavioral reference while Rust contracts are
introduced. Each native component is evaluated independently:

| Component | Decision | Reason |
| --- | --- | --- |
| C3 codec/client | Keep C++; expose C ABI | Stable protocol behavior and existing socket/vendor integration |
| ZK/vendor SDK wrappers | Keep C++ behind C ABI | Vendor SDK ABI and platform availability require isolation |
| Modbus business layer | Migrate toward Rust | Protocol boundary is portable and now has Rust tests |
| Serial OS adapters | Keep platform-specific implementations behind Rust trait | Windows/Linux APIs differ; Core must stay portable |
| REST/RabbitMQ orchestration | Migrate toward Rust | Service validation and event boundaries are platform-neutral |
| Lua host API | Migrate boundary toward Rust; embed VM per target | Controlled capabilities belong to Core, VM remains an integration dependency |

No stable C++ driver is removed by this migration step. A component can be
retired only after parity tests compare the Rust path with the C++ reference,
including errors, state transitions, reconnect behavior, and edge cases.
