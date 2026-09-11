# Flow Definition v1

The Visual Flow editor and future QtNode runtime share this UI-independent
intermediate representation. Runtime adapters must consume this document and
must not depend on DOM IDs or editor layout.

```json
{"version":1,"name":"Locker Access","metadata":{"author":"","updated_at":""},"nodes":[{"id":"node-1","type":"TRIGGER","parameters":{}}],"connections":[{"from":"node-1","to":"node-2","port":"default"}]}
```

Node IDs are stable within a flow. `type` is a runtime category and
`parameters` is an opaque JSON object interpreted by the selected runtime.
Connections refer only to node IDs and optional ports. New fields are additive;
consumers must ignore fields they do not understand so the same definition can
be loaded by the Web runtime and a future QtNode adapter.
