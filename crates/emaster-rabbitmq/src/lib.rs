use emaster_event::{Event, EventBus};
use std::collections::BTreeMap;
use std::sync::Arc;
use std::time::Duration;

#[cfg(feature = "production-transport")]
mod production;
#[cfg(feature = "production-transport")]
pub use production::LapinTransport;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Message {
    pub routing_key: String,
    pub body: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum RabbitError {
    EmptyRoutingKey,
    EmptyBody,
    NotConnected,
    InvalidConfiguration,
    InvalidName,
    AlreadyExists,
    NotFound,
    Transport(String),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum ConnectionState {
    #[default]
    Disconnected,
    Connected,
}

#[derive(Debug, Default)]
pub struct RabbitConnection {
    state: ConnectionState,
}

impl RabbitConnection {
    pub fn connect(&mut self) {
        self.state = ConnectionState::Connected;
    }
    pub fn disconnect(&mut self) {
        self.state = ConnectionState::Disconnected;
    }
    pub fn reconnect(&mut self) {
        self.disconnect();
        self.connect();
    }
    pub fn state(&self) -> ConnectionState {
        self.state
    }
    pub fn publish(&self, bus: &EventBus, message: Message) -> Result<usize, RabbitError> {
        if self.state != ConnectionState::Connected {
            return Err(RabbitError::NotConnected);
        }
        publish(bus, message)
    }
}

pub fn publish(bus: &EventBus, message: Message) -> Result<usize, RabbitError> {
    if message.routing_key.trim().is_empty() {
        return Err(RabbitError::EmptyRoutingKey);
    }
    if message.body.is_empty() {
        return Err(RabbitError::EmptyBody);
    }
    let payload = String::from_utf8_lossy(&message.body).into_owned();
    Ok(bus.publish(Event {
        topic: format!("rabbitmq.{}", message.routing_key),
        payload,
    }))
}

/// Configuration for one independently owned RabbitMQ connection.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RabbitConfig {
    pub name: String,
    pub owner: String,
    pub host: String,
    pub port: u16,
    pub username: String,
    pub virtual_host: String,
    pub timeout_ms: u64,
    pub max_retries: u32,
}

impl RabbitConfig {
    pub fn validate(&self) -> Result<(), RabbitError> {
        if self.name.trim().is_empty()
            || self.owner.trim().is_empty()
            || self.host.trim().is_empty()
            || self.username.trim().is_empty()
            || self.virtual_host.trim().is_empty()
            || self.port == 0
            || self.timeout_ms == 0
        {
            return Err(RabbitError::InvalidConfiguration);
        }
        Ok(())
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RabbitMessage {
    pub exchange: String,
    pub routing_key: String,
    pub body: Vec<u8>,
    pub delivery_tag: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Consumer {
    pub queue: String,
    pub consumer_tag: String,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct RabbitStats {
    pub published: u64,
    pub acknowledged: u64,
    pub consumed: u64,
    pub reconnects: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RabbitStatus {
    pub id: String,
    pub name: String,
    pub owner: String,
    pub state: ConnectionState,
    pub exchanges: Vec<String>,
    pub queues: Vec<String>,
    pub consumers: Vec<Consumer>,
    pub stats: RabbitStats,
    pub last_error: Option<String>,
}

pub trait RabbitTransport: Send + Sync {
    fn connect(&self, config: &RabbitConfig, timeout: Duration) -> Result<(), RabbitError>;
    fn disconnect(&self, config: &RabbitConfig) -> Result<(), RabbitError>;
    fn declare_exchange(&self, config: &RabbitConfig, exchange: &str) -> Result<(), RabbitError>;
    fn declare_queue(&self, config: &RabbitConfig, queue: &str) -> Result<(), RabbitError>;
    fn publish(&self, config: &RabbitConfig, message: &RabbitMessage) -> Result<(), RabbitError>;
    fn consume(&self, config: &RabbitConfig, consumer: &Consumer) -> Result<(), RabbitError>;
    fn cancel_consumer(&self, config: &RabbitConfig, consumer_tag: &str)
        -> Result<(), RabbitError>;
    fn acknowledge(&self, config: &RabbitConfig, delivery_tag: u64) -> Result<(), RabbitError>;
}

#[derive(Debug, Default)]
pub struct UnconfiguredTransport;
impl RabbitTransport for UnconfiguredTransport {
    fn connect(&self, _: &RabbitConfig, _: Duration) -> Result<(), RabbitError> {
        Err(RabbitError::Transport(
            "no RabbitMQ transport configured".into(),
        ))
    }
    fn disconnect(&self, _: &RabbitConfig) -> Result<(), RabbitError> {
        Ok(())
    }
    fn declare_exchange(&self, _: &RabbitConfig, _: &str) -> Result<(), RabbitError> {
        Ok(())
    }
    fn declare_queue(&self, _: &RabbitConfig, _: &str) -> Result<(), RabbitError> {
        Ok(())
    }
    fn publish(&self, _: &RabbitConfig, _: &RabbitMessage) -> Result<(), RabbitError> {
        Err(RabbitError::Transport(
            "no RabbitMQ transport configured".into(),
        ))
    }
    fn consume(&self, _: &RabbitConfig, _: &Consumer) -> Result<(), RabbitError> {
        Err(RabbitError::Transport(
            "no RabbitMQ transport configured".into(),
        ))
    }
    fn cancel_consumer(&self, _: &RabbitConfig, _: &str) -> Result<(), RabbitError> {
        Ok(())
    }
    fn acknowledge(&self, _: &RabbitConfig, _: u64) -> Result<(), RabbitError> {
        Err(RabbitError::Transport(
            "no RabbitMQ transport configured".into(),
        ))
    }
}

struct ManagedConnection {
    config: RabbitConfig,
    state: ConnectionState,
    exchanges: Vec<String>,
    queues: Vec<String>,
    consumers: BTreeMap<String, Consumer>,
    stats: RabbitStats,
    last_error: Option<String>,
    transport: Arc<dyn RabbitTransport>,
}

pub struct RabbitManager {
    connections: BTreeMap<String, ManagedConnection>,
    transport: Arc<dyn RabbitTransport>,
}

impl Default for RabbitManager {
    fn default() -> Self {
        Self::new()
    }
}
impl RabbitManager {
    pub fn new() -> Self {
        Self {
            connections: BTreeMap::new(),
            transport: Arc::new(UnconfiguredTransport),
        }
    }
    pub fn with_transport(transport: Arc<dyn RabbitTransport>) -> Self {
        Self {
            connections: BTreeMap::new(),
            transport,
        }
    }
    pub fn create(
        &mut self,
        id: impl Into<String>,
        config: RabbitConfig,
    ) -> Result<(), RabbitError> {
        let id = id.into();
        config.validate()?;
        if self.connections.contains_key(&id) {
            return Err(RabbitError::AlreadyExists);
        }
        self.connections.insert(
            id,
            ManagedConnection {
                config,
                state: ConnectionState::Disconnected,
                exchanges: Vec::new(),
                queues: Vec::new(),
                consumers: BTreeMap::new(),
                stats: RabbitStats::default(),
                last_error: None,
                transport: Arc::clone(&self.transport),
            },
        );
        Ok(())
    }
    pub fn start(&mut self, id: &str) -> Result<(), RabbitError> {
        let c = self.connections.get_mut(id).ok_or(RabbitError::NotFound)?;
        c.transport
            .connect(&c.config, Duration::from_millis(c.config.timeout_ms))?;
        c.state = ConnectionState::Connected;
        c.last_error = None;
        for exchange in c.exchanges.clone() {
            c.transport.declare_exchange(&c.config, &exchange)?;
        }
        for queue in c.queues.clone() {
            c.transport.declare_queue(&c.config, &queue)?;
        }
        for consumer in c.consumers.values() {
            c.transport.consume(&c.config, consumer)?;
        }
        Ok(())
    }
    pub fn stop(&mut self, id: &str) -> Result<(), RabbitError> {
        let c = self.connections.get_mut(id).ok_or(RabbitError::NotFound)?;
        c.transport.disconnect(&c.config)?;
        c.state = ConnectionState::Disconnected;
        Ok(())
    }
    pub fn restart(&mut self, id: &str) -> Result<(), RabbitError> {
        self.stop(id)?;
        let c = self.connections.get_mut(id).ok_or(RabbitError::NotFound)?;
        c.stats.reconnects += 1;
        self.start(id)
    }
    pub fn destroy(&mut self, id: &str) -> Result<(), RabbitError> {
        self.connections
            .remove(id)
            .map(|_| ())
            .ok_or(RabbitError::NotFound)
    }
    pub fn destroy_owner(&mut self, owner: &str) {
        self.connections.retain(|_, c| c.config.owner != owner);
    }
    pub fn status(&self, id: &str) -> Result<RabbitStatus, RabbitError> {
        let c = self.connections.get(id).ok_or(RabbitError::NotFound)?;
        Ok(RabbitStatus {
            id: id.into(),
            name: c.config.name.clone(),
            owner: c.config.owner.clone(),
            state: c.state,
            exchanges: c.exchanges.clone(),
            queues: c.queues.clone(),
            consumers: c.consumers.values().cloned().collect(),
            stats: c.stats.clone(),
            last_error: c.last_error.clone(),
        })
    }
    pub fn list(&self) -> Vec<RabbitStatus> {
        self.connections
            .keys()
            .filter_map(|id| self.status(id).ok())
            .collect()
    }
    pub fn declare_exchange(
        &mut self,
        id: &str,
        exchange: impl Into<String>,
    ) -> Result<(), RabbitError> {
        let exchange = valid_name(exchange.into())?;
        let c = self.connections.get_mut(id).ok_or(RabbitError::NotFound)?;
        if c.state != ConnectionState::Connected {
            return Err(RabbitError::NotConnected);
        }
        c.transport.declare_exchange(&c.config, &exchange)?;
        if !c.exchanges.contains(&exchange) {
            c.exchanges.push(exchange);
        }
        Ok(())
    }
    pub fn declare_queue(&mut self, id: &str, queue: impl Into<String>) -> Result<(), RabbitError> {
        let queue = valid_name(queue.into())?;
        let c = self.connections.get_mut(id).ok_or(RabbitError::NotFound)?;
        if c.state != ConnectionState::Connected {
            return Err(RabbitError::NotConnected);
        }
        c.transport.declare_queue(&c.config, &queue)?;
        if !c.queues.contains(&queue) {
            c.queues.push(queue);
        }
        Ok(())
    }
    pub fn consume(&mut self, id: &str, consumer: Consumer) -> Result<(), RabbitError> {
        let consumer = validate_consumer(consumer)?;
        let c = self.connections.get_mut(id).ok_or(RabbitError::NotFound)?;
        if c.state != ConnectionState::Connected {
            return Err(RabbitError::NotConnected);
        }
        c.transport.consume(&c.config, &consumer)?;
        c.consumers.insert(consumer.consumer_tag.clone(), consumer);
        Ok(())
    }
    pub fn cancel_consumer(&mut self, id: &str, tag: &str) -> Result<(), RabbitError> {
        let c = self.connections.get_mut(id).ok_or(RabbitError::NotFound)?;
        if c.state != ConnectionState::Connected {
            return Err(RabbitError::NotConnected);
        }
        c.transport.cancel_consumer(&c.config, tag)?;
        c.consumers.remove(tag);
        Ok(())
    }
    pub fn publish(&mut self, id: &str, message: RabbitMessage) -> Result<(), RabbitError> {
        if message.exchange.trim().is_empty() || message.routing_key.trim().is_empty() {
            return Err(RabbitError::EmptyRoutingKey);
        }
        if message.body.is_empty() {
            return Err(RabbitError::EmptyBody);
        }
        let c = self.connections.get_mut(id).ok_or(RabbitError::NotFound)?;
        if c.state != ConnectionState::Connected {
            return Err(RabbitError::NotConnected);
        }
        c.transport.publish(&c.config, &message)?;
        c.stats.published += 1;
        Ok(())
    }
    pub fn acknowledge(&mut self, id: &str, delivery_tag: u64) -> Result<(), RabbitError> {
        if delivery_tag == 0 {
            return Err(RabbitError::InvalidName);
        }
        let c = self.connections.get_mut(id).ok_or(RabbitError::NotFound)?;
        if c.state != ConnectionState::Connected {
            return Err(RabbitError::NotConnected);
        }
        c.transport.acknowledge(&c.config, delivery_tag)?;
        c.stats.acknowledged += 1;
        Ok(())
    }
}
fn valid_name(name: String) -> Result<String, RabbitError> {
    if name.trim().is_empty() || name.len() > 255 || name.contains('\0') {
        Err(RabbitError::InvalidName)
    } else {
        Ok(name)
    }
}
fn validate_consumer(consumer: Consumer) -> Result<Consumer, RabbitError> {
    valid_name(consumer.queue.clone())?;
    valid_name(consumer.consumer_tag.clone())?;
    Ok(consumer)
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn validated_messages_enter_the_event_bus() {
        let bus = EventBus::new();
        let receiver = bus.subscribe();
        assert_eq!(
            publish(
                &bus,
                Message {
                    routing_key: "device".into(),
                    body: b"ok".to_vec()
                }
            )
            .unwrap(),
            1
        );
        assert_eq!(receiver.recv().unwrap().topic, "rabbitmq.device");
        assert_eq!(
            publish(
                &bus,
                Message {
                    routing_key: "".into(),
                    body: vec![1]
                }
            ),
            Err(RabbitError::EmptyRoutingKey)
        );
    }

    #[test]
    fn connection_requires_explicit_connect_and_supports_reconnect() {
        let bus = EventBus::new();
        let mut connection = RabbitConnection::default();
        assert_eq!(
            connection.publish(
                &bus,
                Message {
                    routing_key: "x".into(),
                    body: vec![1]
                }
            ),
            Err(RabbitError::NotConnected)
        );
        connection.connect();
        assert_eq!(connection.state(), ConnectionState::Connected);
        assert_eq!(
            connection.publish(
                &bus,
                Message {
                    routing_key: "x".into(),
                    body: vec![1]
                }
            ),
            Ok(0)
        );
        connection.reconnect();
        assert_eq!(connection.state(), ConnectionState::Connected);
    }
}

#[cfg(test)]
mod manager_tests {
    use super::*;

    #[derive(Default)]
    struct Mock {
        calls: std::sync::Mutex<Vec<String>>,
    }
    impl RabbitTransport for Mock {
        fn connect(&self, _: &RabbitConfig, _: Duration) -> Result<(), RabbitError> {
            self.calls.lock().unwrap().push("connect".into());
            Ok(())
        }
        fn disconnect(&self, _: &RabbitConfig) -> Result<(), RabbitError> {
            Ok(())
        }
        fn declare_exchange(&self, _: &RabbitConfig, value: &str) -> Result<(), RabbitError> {
            self.calls.lock().unwrap().push(format!("exchange:{value}"));
            Ok(())
        }
        fn declare_queue(&self, _: &RabbitConfig, value: &str) -> Result<(), RabbitError> {
            self.calls.lock().unwrap().push(format!("queue:{value}"));
            Ok(())
        }
        fn publish(&self, _: &RabbitConfig, value: &RabbitMessage) -> Result<(), RabbitError> {
            self.calls
                .lock()
                .unwrap()
                .push(format!("publish:{}", value.routing_key));
            Ok(())
        }
        fn consume(&self, _: &RabbitConfig, value: &Consumer) -> Result<(), RabbitError> {
            self.calls
                .lock()
                .unwrap()
                .push(format!("consume:{}", value.consumer_tag));
            Ok(())
        }
        fn cancel_consumer(&self, _: &RabbitConfig, _: &str) -> Result<(), RabbitError> {
            Ok(())
        }
        fn acknowledge(&self, _: &RabbitConfig, _: u64) -> Result<(), RabbitError> {
            Ok(())
        }
    }
    fn config(owner: &str) -> RabbitConfig {
        RabbitConfig {
            name: "bus".into(),
            owner: owner.into(),
            host: "localhost".into(),
            port: 5672,
            username: "guest".into(),
            virtual_host: "/".into(),
            timeout_ms: 100,
            max_retries: 1,
        }
    }
    #[test]
    fn manager_restores_declared_resources_and_consumers() {
        let mut manager = RabbitManager::with_transport(Arc::new(Mock::default()));
        manager.create("one", config("app-a")).unwrap();
        manager.start("one").unwrap();
        manager.declare_exchange("one", "events").unwrap();
        manager.declare_queue("one", "events.q").unwrap();
        manager
            .consume(
                "one",
                Consumer {
                    queue: "events.q".into(),
                    consumer_tag: "worker".into(),
                },
            )
            .unwrap();
        manager.restart("one").unwrap();
        let status = manager.status("one").unwrap();
        assert_eq!(status.exchanges, vec!["events"]);
        assert_eq!(status.queues, vec!["events.q"]);
        assert_eq!(status.consumers.len(), 1);
        assert_eq!(status.stats.reconnects, 1);
    }
    #[test]
    fn manager_tracks_publish_ack_and_owner_isolation() {
        let mut manager = RabbitManager::with_transport(Arc::new(Mock::default()));
        manager.create("a", config("app-a")).unwrap();
        manager.create("b", config("app-b")).unwrap();
        manager.start("a").unwrap();
        manager
            .publish(
                "a",
                RabbitMessage {
                    exchange: "events".into(),
                    routing_key: "device.status".into(),
                    body: vec![1],
                    delivery_tag: 0,
                },
            )
            .unwrap();
        manager.acknowledge("a", 1).unwrap();
        assert_eq!(manager.status("a").unwrap().stats.published, 1);
        assert_eq!(manager.status("a").unwrap().stats.acknowledged, 1);
        manager.destroy_owner("app-a");
        assert!(manager.status("a").is_err());
        assert!(manager.status("b").is_ok());
    }
}
