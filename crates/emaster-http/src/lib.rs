//! Validated REST connectivity primitives with explicit ownership and lifecycle.
use std::collections::BTreeMap;
use std::io::{Read, Write};
use std::net::TcpStream;
use std::sync::Arc;
use std::time::Duration;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum HttpError {
    PayloadTooLarge,
    Unauthenticated,
    Forbidden,
    Malformed,
    InvalidUrl,
    NotConnected,
    AlreadyExists,
    NotFound,
    Timeout,
    Transport(String),
    HttpStatus(u16),
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Request {
    pub method: String,
    pub path: String,
    pub body: Vec<u8>,
    pub api_key: Option<String>,
}
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct Response {
    pub status: u16,
    pub body: Vec<u8>,
}
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct HttpResponse {
    pub status: u16,
    pub headers: BTreeMap<String, String>,
    pub body: Vec<u8>,
}
pub trait Service {
    fn handle(&mut self, request: &Request) -> Response;
}
pub fn dispatch<S: Service>(
    service: &mut S,
    request: &Request,
    max_payload: usize,
    expected_key: Option<&str>,
) -> Response {
    match validate_request(request, max_payload, expected_key) {
        Ok(()) => service.handle(request),
        Err(HttpError::PayloadTooLarge) => text_response(413, "payload too large"),
        Err(HttpError::Unauthenticated) => text_response(401, "unauthenticated"),
        Err(HttpError::Forbidden) => text_response(403, "forbidden"),
        Err(HttpError::Malformed) => text_response(400, "malformed request"),
        Err(_) => text_response(400, "invalid request"),
    }
}
fn text_response(status: u16, body: &str) -> Response {
    Response {
        status,
        body: body.as_bytes().to_vec(),
    }
}
pub fn validate_request(
    request: &Request,
    max_payload: usize,
    expected_key: Option<&str>,
) -> Result<(), HttpError> {
    if request.body.len() > max_payload {
        return Err(HttpError::PayloadTooLarge);
    }
    if request.method.trim().is_empty() || !request.path.starts_with('/') {
        return Err(HttpError::Malformed);
    }
    if let Some(key) = expected_key {
        if request.api_key.as_deref() != Some(key) {
            return Err(HttpError::Unauthenticated);
        }
    }
    Ok(())
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ConnectionState {
    Created,
    Connected,
    Disconnected,
    Failed,
    #[default]
    Unknown,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Authentication {
    None,
    ApiKey(String),
    Bearer(String),
    Basic { username: String, password: String },
    Headers(BTreeMap<String, String>),
}
impl Default for Authentication {
    fn default() -> Self {
        Self::None
    }
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConnectionConfig {
    pub name: String,
    pub url: String,
    pub owner: String,
    pub timeout_ms: u64,
    pub max_retries: u32,
    pub authentication: Authentication,
}
impl ConnectionConfig {
    pub fn validate(&self) -> Result<(), HttpError> {
        let scheme = self.url.split_once("://").map(|v| v.0);
        if !matches!(scheme, Some("http") | Some("https"))
            || self
                .url
                .split_once("://")
                .map(|v| v.1)
                .unwrap_or("")
                .trim()
                .is_empty()
        {
            return Err(HttpError::InvalidUrl);
        }
        if self.name.trim().is_empty() || self.owner.trim().is_empty() || self.timeout_ms == 0 {
            return Err(HttpError::Malformed);
        }
        Ok(())
    }
}
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ConnectionStats {
    pub requests: u64,
    pub successes: u64,
    pub failures: u64,
    pub retries: u64,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConnectionStatus {
    pub id: String,
    pub name: String,
    pub owner: String,
    pub state: ConnectionState,
    pub stats: ConnectionStats,
    pub last_error: Option<String>,
}
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HttpRequest {
    pub method: String,
    pub path: String,
    pub query: BTreeMap<String, String>,
    pub headers: BTreeMap<String, String>,
    pub body: Vec<u8>,
}
impl HttpRequest {
    pub fn new(method: impl Into<String>, path: impl Into<String>) -> Self {
        Self {
            method: method.into(),
            path: path.into(),
            query: BTreeMap::new(),
            headers: BTreeMap::new(),
            body: Vec::new(),
        }
    }
}
pub trait HttpTransport: Send + Sync {
    fn send(
        &self,
        base_url: &str,
        request: &HttpRequest,
        timeout: Duration,
    ) -> Result<HttpResponse, HttpError>;
}
#[derive(Debug, Default)]
pub struct UnconfiguredTransport;
impl HttpTransport for UnconfiguredTransport {
    fn send(&self, _: &str, _: &HttpRequest, _: Duration) -> Result<HttpResponse, HttpError> {
        Err(HttpError::Transport("no HTTP transport configured".into()))
    }
}

/// Small synchronous HTTP/1.1 transport for environments where an external
/// HTTP client is not available. It intentionally supports `http://` only;
/// HTTPS must be supplied through an injected TLS-capable transport.
#[derive(Debug, Default)]
pub struct TcpHttpTransport;

impl HttpTransport for TcpHttpTransport {
    fn send(
        &self,
        base_url: &str,
        request: &HttpRequest,
        timeout: Duration,
    ) -> Result<HttpResponse, HttpError> {
        let (host, port, base_path) = parse_http_url(base_url)?;
        let address = format!("{host}:{port}");
        let mut stream = TcpStream::connect_timeout(
            &address.parse().map_err(|_| HttpError::InvalidUrl)?,
            timeout,
        )
        .map_err(|e| {
            if e.kind() == std::io::ErrorKind::TimedOut {
                HttpError::Timeout
            } else {
                HttpError::Transport(e.to_string())
            }
        })?;
        stream
            .set_read_timeout(Some(timeout))
            .map_err(|e| HttpError::Transport(e.to_string()))?;
        stream
            .set_write_timeout(Some(timeout))
            .map_err(|e| HttpError::Transport(e.to_string()))?;
        let path = join_path(&base_path, &request.path);
        let query = request
            .query
            .iter()
            .map(|(key, value)| format!("{}={}", encode_component(key), encode_component(value)))
            .collect::<Vec<_>>()
            .join("&");
        let target = if query.is_empty() {
            path
        } else {
            format!("{path}?{query}")
        };
        let mut wire = format!(
            "{} {} HTTP/1.1\r\nHost: {host}\r\nConnection: close\r\n",
            request.method, target
        );
        for (key, value) in &request.headers {
            wire.push_str(&format!("{key}: {value}\r\n"));
        }
        wire.push_str(&format!("Content-Length: {}\r\n\r\n", request.body.len()));
        stream
            .write_all(wire.as_bytes())
            .and_then(|_| stream.write_all(&request.body))
            .map_err(|e| HttpError::Transport(e.to_string()))?;
        let mut bytes = Vec::new();
        stream.read_to_end(&mut bytes).map_err(|e| {
            if e.kind() == std::io::ErrorKind::TimedOut {
                HttpError::Timeout
            } else {
                HttpError::Transport(e.to_string())
            }
        })?;
        parse_response(&bytes)
    }
}

fn parse_http_url(url: &str) -> Result<(String, u16, String), HttpError> {
    let rest = url.strip_prefix("http://").ok_or(HttpError::InvalidUrl)?;
    let (authority, path) = rest.split_once('/').map_or((rest, "/"), |parts| parts);
    if authority.is_empty() || authority.contains('@') {
        return Err(HttpError::InvalidUrl);
    }
    let (host, port) = authority
        .rsplit_once(':')
        .map_or((authority, 80), |(host, port)| {
            (
                host,
                port.parse().map_err(|_| HttpError::InvalidUrl).unwrap_or(0),
            )
        });
    if host.is_empty() || port == 0 {
        return Err(HttpError::InvalidUrl);
    }
    Ok((
        host.to_owned(),
        port,
        format!("/{path}", path = path.trim_start_matches('/')),
    ))
}
fn join_path(base: &str, path: &str) -> String {
    format!(
        "{}/{}",
        base.trim_end_matches('/'),
        path.trim_start_matches('/')
    )
}
fn encode_component(value: &str) -> String {
    value
        .bytes()
        .map(|byte| {
            if byte.is_ascii_alphanumeric() || b"-_.~".contains(&byte) {
                (byte as char).to_string()
            } else {
                format!("%{byte:02X}")
            }
        })
        .collect()
}
fn parse_response(bytes: &[u8]) -> Result<HttpResponse, HttpError> {
    let marker = b"\r\n\r\n";
    let split = bytes
        .windows(marker.len())
        .position(|window| window == marker)
        .ok_or_else(|| HttpError::Transport("malformed HTTP response".into()))?;
    let header_text = std::str::from_utf8(&bytes[..split])
        .map_err(|_| HttpError::Transport("invalid HTTP headers".into()))?;
    let mut lines = header_text.lines();
    let status = lines
        .next()
        .and_then(|line| line.split_whitespace().nth(1))
        .and_then(|value| value.parse().ok())
        .ok_or_else(|| HttpError::Transport("missing HTTP status".into()))?;
    let mut headers = BTreeMap::new();
    for line in lines {
        if let Some((key, value)) = line.split_once(':') {
            headers.insert(key.trim().to_ascii_lowercase(), value.trim().into());
        }
    }
    Ok(HttpResponse {
        status,
        headers,
        body: bytes[split + marker.len()..].to_vec(),
    })
}
struct Connection {
    config: ConnectionConfig,
    state: ConnectionState,
    stats: ConnectionStats,
    last_error: Option<String>,
    transport: Arc<dyn HttpTransport>,
}
pub struct HttpManager {
    connections: BTreeMap<String, Connection>,
    transport: Arc<dyn HttpTransport>,
}
impl Default for HttpManager {
    fn default() -> Self {
        Self::new()
    }
}
impl HttpManager {
    pub fn new() -> Self {
        Self {
            connections: BTreeMap::new(),
            transport: Arc::new(UnconfiguredTransport),
        }
    }
    pub fn with_transport(transport: Arc<dyn HttpTransport>) -> Self {
        Self {
            connections: BTreeMap::new(),
            transport,
        }
    }
    pub fn create(
        &mut self,
        id: impl Into<String>,
        config: ConnectionConfig,
    ) -> Result<(), HttpError> {
        let id = id.into();
        config.validate()?;
        if self.connections.contains_key(&id) {
            return Err(HttpError::AlreadyExists);
        }
        self.connections.insert(
            id,
            Connection {
                config,
                state: ConnectionState::Created,
                stats: ConnectionStats::default(),
                last_error: None,
                transport: Arc::clone(&self.transport),
            },
        );
        Ok(())
    }
    pub fn start(&mut self, id: &str) -> Result<(), HttpError> {
        let c = self.connections.get_mut(id).ok_or(HttpError::NotFound)?;
        c.state = ConnectionState::Connected;
        c.last_error = None;
        Ok(())
    }
    pub fn stop(&mut self, id: &str) -> Result<(), HttpError> {
        let c = self.connections.get_mut(id).ok_or(HttpError::NotFound)?;
        c.state = ConnectionState::Disconnected;
        Ok(())
    }
    pub fn restart(&mut self, id: &str) -> Result<(), HttpError> {
        self.stop(id)?;
        self.start(id)
    }
    pub fn destroy(&mut self, id: &str) -> Result<(), HttpError> {
        self.connections
            .remove(id)
            .map(|_| ())
            .ok_or(HttpError::NotFound)
    }
    pub fn status(&self, id: &str) -> Result<ConnectionStatus, HttpError> {
        let c = self.connections.get(id).ok_or(HttpError::NotFound)?;
        Ok(ConnectionStatus {
            id: id.into(),
            name: c.config.name.clone(),
            owner: c.config.owner.clone(),
            state: c.state,
            stats: c.stats.clone(),
            last_error: c.last_error.clone(),
        })
    }
    pub fn list(&self) -> Vec<ConnectionStatus> {
        self.connections
            .keys()
            .filter_map(|id| self.status(id).ok())
            .collect()
    }
    pub fn destroy_owner(&mut self, owner: &str) {
        self.connections.retain(|_, c| c.config.owner != owner);
    }
    pub fn request(
        &mut self,
        id: &str,
        mut request: HttpRequest,
    ) -> Result<HttpResponse, HttpError> {
        let c = self.connections.get_mut(id).ok_or(HttpError::NotFound)?;
        if c.state != ConnectionState::Connected {
            return Err(HttpError::NotConnected);
        }
        if !request.path.starts_with('/') || request.method.trim().is_empty() {
            return Err(HttpError::Malformed);
        }
        apply_auth(&c.config.authentication, &mut request.headers);
        let attempts = c.config.max_retries.saturating_add(1);
        c.stats.requests += 1;
        let mut last_failure = None;
        for attempt in 0..attempts {
            match c.transport.send(
                &c.config.url,
                &request,
                Duration::from_millis(c.config.timeout_ms),
            ) {
                Ok(reply) if reply.status < 500 || attempt + 1 == attempts => {
                    if reply.status < 500 {
                        c.stats.successes += 1;
                    } else {
                        c.stats.failures += 1;
                    }
                    c.last_error = None;
                    return Ok(reply);
                }
                Ok(reply) => {
                    let error = HttpError::HttpStatus(reply.status);
                    c.last_error = Some(format!("{error:?}"));
                    last_failure = Some(error);
                    if attempt + 1 < attempts {
                        c.stats.retries += 1;
                    }
                }
                Err(error) => {
                    c.last_error = Some(format!("{error:?}"));
                    last_failure = Some(error);
                    if attempt + 1 < attempts {
                        c.stats.retries += 1;
                    }
                }
            }
        }
        c.stats.failures += 1;
        c.state = ConnectionState::Failed;
        Err(last_failure
            .unwrap_or_else(|| HttpError::Transport("request retries exhausted".into())))
    }
}
fn apply_auth(auth: &Authentication, headers: &mut BTreeMap<String, String>) {
    match auth {
        Authentication::None => {}
        Authentication::ApiKey(v) => {
            headers.insert("x-api-key".into(), v.clone());
        }
        Authentication::Bearer(v) => {
            headers.insert("authorization".into(), format!("Bearer {v}"));
        }
        Authentication::Basic { username, password } => {
            headers.insert(
                "authorization".into(),
                format!("Basic {username}:{password}"),
            );
        }
        Authentication::Headers(values) => headers.extend(values.clone()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    struct Mock {
        calls: std::sync::Mutex<u32>,
    }
    impl HttpTransport for Mock {
        fn send(&self, _: &str, r: &HttpRequest, _: Duration) -> Result<HttpResponse, HttpError> {
            let mut n = self.calls.lock().unwrap();
            *n += 1;
            if *n == 1 {
                return Ok(HttpResponse {
                    status: 503,
                    body: b"retry".to_vec(),
                    ..HttpResponse::default()
                });
            }
            assert_eq!(r.headers.get("authorization"), Some(&"Bearer token".into()));
            Ok(HttpResponse {
                status: 200,
                body: b"ok".to_vec(),
                ..HttpResponse::default()
            })
        }
    }
    fn cfg() -> ConnectionConfig {
        ConnectionConfig {
            name: "main".into(),
            url: "https://example.test/api".into(),
            owner: "app-a".into(),
            timeout_ms: 100,
            max_retries: 1,
            authentication: Authentication::Bearer("token".into()),
        }
    }
    #[test]
    fn lifecycle_and_owner_cleanup() {
        let mut m = HttpManager::with_transport(Arc::new(Mock {
            calls: std::sync::Mutex::new(0),
        }));
        m.create("a", cfg()).unwrap();
        assert_eq!(m.status("a").unwrap().state, ConnectionState::Created);
        m.start("a").unwrap();
        m.stop("a").unwrap();
        m.restart("a").unwrap();
        m.destroy_owner("app-a");
        assert!(m.status("a").is_err());
    }
    #[test]
    fn request_applies_auth_and_retries() {
        let mut m = HttpManager::with_transport(Arc::new(Mock {
            calls: std::sync::Mutex::new(0),
        }));
        m.create("a", cfg()).unwrap();
        m.start("a").unwrap();
        assert_eq!(
            m.request("a", HttpRequest::new("GET", "/status"))
                .unwrap()
                .status,
            200
        );
        assert_eq!(m.status("a").unwrap().stats.retries, 1);
    }
    #[test]
    fn invalid_and_disconnected_are_rejected() {
        let mut m = HttpManager::new();
        let mut c = cfg();
        c.url = "ftp://host".into();
        assert_eq!(m.create("a", c), Err(HttpError::InvalidUrl));
        m.create("a", cfg()).unwrap();
        assert_eq!(
            m.request("a", HttpRequest::new("GET", "/x")),
            Err(HttpError::NotConnected)
        );
    }

    #[test]
    fn tcp_transport_sends_request_and_parses_response() {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let address = listener.local_addr().unwrap();
        let server = std::thread::spawn(move || {
            let (mut stream, _) = listener.accept().unwrap();
            let mut request = [0u8; 512];
            let _ = stream.read(&mut request).unwrap();
            stream
                .write_all(b"HTTP/1.1 201 Created\r\nX-Test: yes\r\nContent-Length: 2\r\n\r\nok")
                .unwrap();
        });
        let transport = TcpHttpTransport;
        let reply = transport
            .send(
                &format!("http://{address}"),
                &HttpRequest::new("GET", "/status"),
                Duration::from_secs(2),
            )
            .unwrap();
        assert_eq!(reply.status, 201);
        assert_eq!(reply.headers.get("x-test"), Some(&"yes".into()));
        assert_eq!(reply.body, b"ok");
        server.join().unwrap();
    }
}
