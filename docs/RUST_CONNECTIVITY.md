# Rust connectivity foundation

The first connectivity migration phase introduces `emaster-http::HttpManager`.
It is deliberately independent from the production C++ gateway until
compatibility tests are available.

Each REST resource has an explicit ID, owner application, configuration,
connection state, statistics, and last error. The manager supports:

- `create`, `start`, `stop`, `restart`, and `destroy`
- status and deterministic connection listing
- `destroy_owner` for application unload cleanup
- HTTP methods, query parameters, headers, and request bodies through
  `HttpRequest`
- API-key, bearer, basic, and custom-header authentication
- bounded retry of server failures

`HttpTransport` is an injected boundary. This keeps lifecycle and validation
tests deterministic and allows a production HTTP implementation to be added
without exposing transport internals to Lua or the C ABI. The default
`UnconfiguredTransport` fails closed; it does not silently perform network I/O.

The existing `Request`, `Response`, `Service`, and `dispatch` types remain
compatible with the current Rust gateway tests. Rich outbound headers are
available through `HttpResponse` for the new manager API.
