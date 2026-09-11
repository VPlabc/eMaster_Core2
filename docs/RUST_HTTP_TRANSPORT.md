# Rust HTTP transport

`emaster-http::TcpHttpTransport` is the first production transport behind the
REST manager. It supports HTTP/1.1 requests over TCP, query parameters,
headers, request bodies, response headers/bodies, and bounded connect/read/
write timeouts.

It accepts `http://` URLs only and returns an explicit error for HTTPS until a
TLS-capable dependency is selected. Applications can continue using an
injected `HttpTransport` implementation for HTTPS or alternate HTTP clients.
