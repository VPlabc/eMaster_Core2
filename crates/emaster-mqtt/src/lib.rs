//! Owned, multi-connection MQTT resource management.
//!
//! The broker transport is intentionally injected. This keeps the resource
//! model independent from a particular MQTT client library and makes reconnect
//! and subscription restoration deterministic to test.

use std::collections::BTreeMap;
use std::sync::Arc;
use std::time::Duration;

#[cfg(feature = "production-transport")]
mod production;
#[cfg(feature = "production-transport")]
pub use production::RumqttTransport;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ConnectionState {
    Created,
    Connected,
    Disconnected,
    Failed,
    #[default]
    Unknown,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum QoS {
    #[default]
    AtMostOnce,
    AtLeastOnce,
    ExactlyOnce,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MqttConfig {
    pub name: String,
    pub owner: String,
    pub host: String,
    pub port: u16,
    pub client_id: String,
    pub timeout_ms: u64,
    pub max_retries: u32,
}

impl MqttConfig {
    pub fn validate(&self) -> Result<(), MqttError> {
        if self.name.trim().is_empty()
            || self.owner.trim().is_empty()
            || self.host.trim().is_empty()
            || self.client_id.trim().is_empty()
            || self.port == 0
            || self.timeout_ms == 0
        {
            return Err(MqttError::InvalidConfiguration);
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum MqttError {
    InvalidConfiguration,
    InvalidTopic,
    PayloadTooLarge,
    NotFound,
    AlreadyExists,
    NotConnected,
    Transport(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Publication {
    pub topic: String,
    pub payload: Vec<u8>,
    pub qos: QoS,
    pub retained: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Subscription {
    pub topic_filter: String,
    pub qos: QoS,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct MqttStats {
    pub published: u64,
    pub received: u64,
    pub subscriptions: u64,
    pub reconnects: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct MqttStatus {
    pub id: String,
    pub name: String,
    pub owner: String,
    pub state: ConnectionState,
    pub subscriptions: Vec<Subscription>,
    pub stats: MqttStats,
    pub last_error: Option<String>,
}

pub trait MqttTransport: Send + Sync {
    fn connect(&self, config: &MqttConfig, timeout: Duration) -> Result<(), MqttError>;
    fn disconnect(&self, config: &MqttConfig) -> Result<(), MqttError>;
    fn publish(&self, config: &MqttConfig, publication: &Publication) -> Result<(), MqttError>;
    fn subscribe(&self, config: &MqttConfig, subscription: &Subscription) -> Result<(), MqttError>;
    fn unsubscribe(&self, config: &MqttConfig, topic_filter: &str) -> Result<(), MqttError>;
}

#[derive(Debug, Default)]
pub struct UnconfiguredTransport;

impl MqttTransport for UnconfiguredTransport {
    fn connect(&self, _: &MqttConfig, _: Duration) -> Result<(), MqttError> {
        Err(MqttError::Transport("no MQTT transport configured".into()))
    }
    fn disconnect(&self, _: &MqttConfig) -> Result<(), MqttError> {
        Ok(())
    }
    fn publish(&self, _: &MqttConfig, _: &Publication) -> Result<(), MqttError> {
        Err(MqttError::Transport("no MQTT transport configured".into()))
    }
    fn subscribe(&self, _: &MqttConfig, _: &Subscription) -> Result<(), MqttError> {
        Err(MqttError::Transport("no MQTT transport configured".into()))
    }
    fn unsubscribe(&self, _: &MqttConfig, _: &str) -> Result<(), MqttError> {
        Ok(())
    }
}

struct Connection {
    config: MqttConfig,
    state: ConnectionState,
    subscriptions: BTreeMap<String, Subscription>,
    stats: MqttStats,
    last_error: Option<String>,
    transport: Arc<dyn MqttTransport>,
}

pub struct MqttManager {
    connections: BTreeMap<String, Connection>,
    transport: Arc<dyn MqttTransport>,
    max_payload: usize,
}

impl Default for MqttManager {
    fn default() -> Self {
        Self::new()
    }
}

impl MqttManager {
    pub fn new() -> Self {
        Self {
            connections: BTreeMap::new(),
            transport: Arc::new(UnconfiguredTransport),
            max_payload: 1024 * 1024,
        }
    }
    pub fn with_transport(transport: Arc<dyn MqttTransport>) -> Self {
        Self {
            connections: BTreeMap::new(),
            transport,
            max_payload: 1024 * 1024,
        }
    }
    pub fn with_max_payload(mut self, max_payload: usize) -> Self {
        self.max_payload = max_payload;
        self
    }
    pub fn create(&mut self, id: impl Into<String>, config: MqttConfig) -> Result<(), MqttError> {
        let id = id.into();
        config.validate()?;
        if self.connections.contains_key(&id) {
            return Err(MqttError::AlreadyExists);
        }
        self.connections.insert(
            id,
            Connection {
                config,
                state: ConnectionState::Created,
                subscriptions: BTreeMap::new(),
                stats: MqttStats::default(),
                last_error: None,
                transport: Arc::clone(&self.transport),
            },
        );
        Ok(())
    }
    pub fn start(&mut self, id: &str) -> Result<(), MqttError> {
        let c = self.connections.get_mut(id).ok_or(MqttError::NotFound)?;
        c.transport
            .connect(&c.config, Duration::from_millis(c.config.timeout_ms))?;
        c.state = ConnectionState::Connected;
        c.last_error = None;
        let subscriptions: Vec<_> = c.subscriptions.values().cloned().collect();
        for subscription in subscriptions {
            c.transport.subscribe(&c.config, &subscription)?;
        }
        Ok(())
    }
    pub fn stop(&mut self, id: &str) -> Result<(), MqttError> {
        let c = self.connections.get_mut(id).ok_or(MqttError::NotFound)?;
        c.transport.disconnect(&c.config)?;
        c.state = ConnectionState::Disconnected;
        Ok(())
    }
    pub fn restart(&mut self, id: &str) -> Result<(), MqttError> {
        self.stop(id)?;
        let c = self.connections.get_mut(id).ok_or(MqttError::NotFound)?;
        c.stats.reconnects += 1;
        self.start(id)
    }
    pub fn destroy(&mut self, id: &str) -> Result<(), MqttError> {
        self.connections
            .remove(id)
            .map(|_| ())
            .ok_or(MqttError::NotFound)
    }
    pub fn destroy_owner(&mut self, owner: &str) {
        self.connections.retain(|_, c| c.config.owner != owner);
    }
    pub fn status(&self, id: &str) -> Result<MqttStatus, MqttError> {
        let c = self.connections.get(id).ok_or(MqttError::NotFound)?;
        Ok(MqttStatus {
            id: id.into(),
            name: c.config.name.clone(),
            owner: c.config.owner.clone(),
            state: c.state,
            subscriptions: c.subscriptions.values().cloned().collect(),
            stats: c.stats.clone(),
            last_error: c.last_error.clone(),
        })
    }
    pub fn list(&self) -> Vec<MqttStatus> {
        self.connections
            .keys()
            .filter_map(|id| self.status(id).ok())
            .collect()
    }
    pub fn subscribe(&mut self, id: &str, subscription: Subscription) -> Result<(), MqttError> {
        validate_topic(&subscription.topic_filter)?;
        let c = self.connections.get_mut(id).ok_or(MqttError::NotFound)?;
        if c.state != ConnectionState::Connected {
            return Err(MqttError::NotConnected);
        }
        c.transport.subscribe(&c.config, &subscription)?;
        c.subscriptions
            .insert(subscription.topic_filter.clone(), subscription);
        c.stats.subscriptions = c.subscriptions.len() as u64;
        Ok(())
    }
    pub fn unsubscribe(&mut self, id: &str, topic_filter: &str) -> Result<(), MqttError> {
        validate_topic(topic_filter)?;
        let c = self.connections.get_mut(id).ok_or(MqttError::NotFound)?;
        if c.state != ConnectionState::Connected {
            return Err(MqttError::NotConnected);
        }
        c.transport.unsubscribe(&c.config, topic_filter)?;
        c.subscriptions.remove(topic_filter);
        c.stats.subscriptions = c.subscriptions.len() as u64;
        Ok(())
    }
    pub fn publish(&mut self, id: &str, publication: Publication) -> Result<(), MqttError> {
        validate_topic(&publication.topic)?;
        if publication.payload.len() > self.max_payload {
            return Err(MqttError::PayloadTooLarge);
        }
        let c = self.connections.get_mut(id).ok_or(MqttError::NotFound)?;
        if c.state != ConnectionState::Connected {
            return Err(MqttError::NotConnected);
        }
        c.transport.publish(&c.config, &publication)?;
        c.stats.published += 1;
        Ok(())
    }
}

fn validate_topic(topic: &str) -> Result<(), MqttError> {
    if topic.trim().is_empty() || topic.len() > 65535 || topic.contains('\0') {
        return Err(MqttError::InvalidTopic);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[derive(Default)]
    struct Mock {
        calls: std::sync::Mutex<Vec<String>>,
    }
    impl MqttTransport for Mock {
        fn connect(&self, c: &MqttConfig, _: Duration) -> Result<(), MqttError> {
            self.calls
                .lock()
                .unwrap()
                .push(format!("connect:{}", c.name));
            Ok(())
        }
        fn disconnect(&self, _: &MqttConfig) -> Result<(), MqttError> {
            Ok(())
        }
        fn publish(&self, _: &MqttConfig, p: &Publication) -> Result<(), MqttError> {
            self.calls
                .lock()
                .unwrap()
                .push(format!("publish:{}", p.topic));
            Ok(())
        }
        fn subscribe(&self, _: &MqttConfig, s: &Subscription) -> Result<(), MqttError> {
            self.calls
                .lock()
                .unwrap()
                .push(format!("subscribe:{}", s.topic_filter));
            Ok(())
        }
        fn unsubscribe(&self, _: &MqttConfig, t: &str) -> Result<(), MqttError> {
            self.calls.lock().unwrap().push(format!("unsubscribe:{t}"));
            Ok(())
        }
    }
    fn cfg(owner: &str) -> MqttConfig {
        MqttConfig {
            name: "broker".into(),
            owner: owner.into(),
            host: "localhost".into(),
            port: 1883,
            client_id: "emaster-test".into(),
            timeout_ms: 100,
            max_retries: 1,
        }
    }
    #[test]
    fn multiple_connections_are_isolated_and_owned() {
        let mut m = MqttManager::with_transport(Arc::new(Mock::default()));
        m.create("a", cfg("app-a")).unwrap();
        m.create("b", cfg("app-b")).unwrap();
        m.start("a").unwrap();
        m.subscribe(
            "a",
            Subscription {
                topic_filter: "factory/#".into(),
                qos: QoS::AtLeastOnce,
            },
        )
        .unwrap();
        assert_eq!(m.status("b").unwrap().subscriptions.len(), 0);
        m.destroy_owner("app-a");
        assert!(m.status("a").is_err());
        assert!(m.status("b").is_ok());
    }
    #[test]
    fn restart_restores_subscriptions_and_publish_tracks_stats() {
        let mut m = MqttManager::with_transport(Arc::new(Mock::default()));
        m.create("a", cfg("app")).unwrap();
        m.start("a").unwrap();
        m.subscribe(
            "a",
            Subscription {
                topic_filter: "device/status".into(),
                qos: QoS::AtMostOnce,
            },
        )
        .unwrap();
        m.publish(
            "a",
            Publication {
                topic: "device/status".into(),
                payload: b"ok".to_vec(),
                qos: QoS::AtMostOnce,
                retained: false,
            },
        )
        .unwrap();
        m.restart("a").unwrap();
        let s = m.status("a").unwrap();
        assert_eq!(s.stats.published, 1);
        assert_eq!(s.stats.reconnects, 1);
        assert_eq!(s.subscriptions.len(), 1);
    }
    #[test]
    fn validates_payload_topic_and_lifecycle() {
        let mut m = MqttManager::with_transport(Arc::new(Mock::default())).with_max_payload(2);
        let mut c = cfg("app");
        c.port = 0;
        assert_eq!(m.create("a", c), Err(MqttError::InvalidConfiguration));
        m.create("a", cfg("app")).unwrap();
        assert_eq!(
            m.publish(
                "a",
                Publication {
                    topic: "x".into(),
                    payload: vec![1],
                    qos: QoS::AtMostOnce,
                    retained: false
                }
            ),
            Err(MqttError::NotConnected)
        );
        m.start("a").unwrap();
        assert_eq!(
            m.publish(
                "a",
                Publication {
                    topic: "x".into(),
                    payload: vec![1, 2, 3],
                    qos: QoS::AtMostOnce,
                    retained: false
                }
            ),
            Err(MqttError::PayloadTooLarge)
        );
    }
}
