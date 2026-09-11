use crate::{Consumer, RabbitConfig, RabbitError, RabbitMessage, RabbitTransport};
use lapin::{
    options::*, types::FieldTable, BasicProperties, Channel, Connection, ConnectionProperties,
    ExchangeKind,
};
use std::collections::BTreeMap;
use std::sync::Mutex;
use std::time::Duration;

struct Session {
    _connection: Connection,
    channel: Channel,
    consumers: BTreeMap<String, lapin::Consumer>,
}

/// Synchronous facade over lapin. The runtime is owned by this adapter so no
/// Tokio internals cross the manager or Lua boundaries.
pub struct LapinTransport {
    runtime: tokio::runtime::Runtime,
    sessions: Mutex<BTreeMap<String, Session>>,
}

impl LapinTransport {
    pub fn new() -> Result<Self, RabbitError> {
        Ok(Self {
            runtime: tokio::runtime::Runtime::new()
                .map_err(|e| RabbitError::Transport(e.to_string()))?,
            sessions: Mutex::new(BTreeMap::new()),
        })
    }
}

impl RabbitTransport for LapinTransport {
    fn connect(&self, config: &RabbitConfig, _: Duration) -> Result<(), RabbitError> {
        let uri = format!(
            "amqp://{}@{}:{}/{}",
            config.username,
            config.host,
            config.port,
            config.virtual_host.trim_start_matches('/')
        );
        let (connection, channel) = self.runtime.block_on(async {
            let connection = Connection::connect(&uri, ConnectionProperties::default())
                .await
                .map_err(|e| RabbitError::Transport(e.to_string()))?;
            let channel = connection
                .create_channel()
                .await
                .map_err(|e| RabbitError::Transport(e.to_string()))?;
            Ok::<_, RabbitError>((connection, channel))
        })?;
        self.sessions
            .lock()
            .map_err(|_| RabbitError::Transport("RabbitMQ session lock poisoned".into()))?
            .insert(
                config.name.clone(),
                Session {
                    _connection: connection,
                    channel,
                    consumers: BTreeMap::new(),
                },
            );
        Ok(())
    }
    fn disconnect(&self, config: &RabbitConfig) -> Result<(), RabbitError> {
        self.sessions
            .lock()
            .map_err(|_| RabbitError::Transport("RabbitMQ session lock poisoned".into()))?
            .remove(&config.name);
        Ok(())
    }
    fn declare_exchange(&self, config: &RabbitConfig, exchange: &str) -> Result<(), RabbitError> {
        let sessions = self
            .sessions
            .lock()
            .map_err(|_| RabbitError::Transport("RabbitMQ session lock poisoned".into()))?;
        let session = sessions
            .get(&config.name)
            .ok_or(RabbitError::NotConnected)?;
        self.runtime
            .block_on(session.channel.exchange_declare(
                exchange.into(),
                ExchangeKind::Direct,
                ExchangeDeclareOptions::default(),
                FieldTable::default(),
            ))
            .map(|_| ())
            .map_err(|e| RabbitError::Transport(e.to_string()))
    }
    fn declare_queue(&self, config: &RabbitConfig, queue: &str) -> Result<(), RabbitError> {
        let sessions = self
            .sessions
            .lock()
            .map_err(|_| RabbitError::Transport("RabbitMQ session lock poisoned".into()))?;
        let session = sessions
            .get(&config.name)
            .ok_or(RabbitError::NotConnected)?;
        self.runtime
            .block_on(session.channel.queue_declare(
                queue.into(),
                QueueDeclareOptions::default(),
                FieldTable::default(),
            ))
            .map(|_| ())
            .map_err(|e| RabbitError::Transport(e.to_string()))
    }
    fn publish(&self, config: &RabbitConfig, message: &RabbitMessage) -> Result<(), RabbitError> {
        let sessions = self
            .sessions
            .lock()
            .map_err(|_| RabbitError::Transport("RabbitMQ session lock poisoned".into()))?;
        let session = sessions
            .get(&config.name)
            .ok_or(RabbitError::NotConnected)?;
        self.runtime.block_on(async {
            let confirmation = session
                .channel
                .basic_publish(
                    message.exchange.clone().into(),
                    message.routing_key.clone().into(),
                    BasicPublishOptions::default(),
                    &message.body,
                    BasicProperties::default(),
                )
                .await
                .map_err(|e| RabbitError::Transport(e.to_string()))?;
            confirmation
                .await
                .map_err(|e| RabbitError::Transport(e.to_string()))
                .map(|_| ())
        })
    }
    fn consume(&self, config: &RabbitConfig, consumer: &Consumer) -> Result<(), RabbitError> {
        let channel = {
            let sessions = self
                .sessions
                .lock()
                .map_err(|_| RabbitError::Transport("RabbitMQ session lock poisoned".into()))?;
            sessions
                .get(&config.name)
                .ok_or(RabbitError::NotConnected)?
                .channel
                .clone()
        };
        let consumer_stream = self
            .runtime
            .block_on(channel.basic_consume(
                consumer.queue.clone().into(),
                consumer.consumer_tag.clone().into(),
                BasicConsumeOptions::default(),
                FieldTable::default(),
            ))
            .map_err(|e| RabbitError::Transport(e.to_string()))?;
        let mut sessions = self
            .sessions
            .lock()
            .map_err(|_| RabbitError::Transport("RabbitMQ session lock poisoned".into()))?;
        let session = sessions
            .get_mut(&config.name)
            .ok_or(RabbitError::NotConnected)?;
        session
            .consumers
            .insert(consumer.consumer_tag.clone(), consumer_stream);
        Ok(())
    }
    fn cancel_consumer(
        &self,
        config: &RabbitConfig,
        consumer_tag: &str,
    ) -> Result<(), RabbitError> {
        let channel = {
            let sessions = self
                .sessions
                .lock()
                .map_err(|_| RabbitError::Transport("RabbitMQ session lock poisoned".into()))?;
            sessions
                .get(&config.name)
                .ok_or(RabbitError::NotConnected)?
                .channel
                .clone()
        };
        self.runtime
            .block_on(channel.basic_cancel(consumer_tag.into(), BasicCancelOptions::default()))
            .map(|_| ())
            .map_err(|e| RabbitError::Transport(e.to_string()))?;
        let mut sessions = self
            .sessions
            .lock()
            .map_err(|_| RabbitError::Transport("RabbitMQ session lock poisoned".into()))?;
        if let Some(session) = sessions.get_mut(&config.name) {
            session.consumers.remove(consumer_tag);
        }
        Ok(())
    }
    fn acknowledge(&self, config: &RabbitConfig, delivery_tag: u64) -> Result<(), RabbitError> {
        let sessions = self
            .sessions
            .lock()
            .map_err(|_| RabbitError::Transport("RabbitMQ session lock poisoned".into()))?;
        let session = sessions
            .get(&config.name)
            .ok_or(RabbitError::NotConnected)?;
        self.runtime
            .block_on(
                session
                    .channel
                    .basic_ack(delivery_tag, BasicAckOptions::default()),
            )
            .map_err(|e| RabbitError::Transport(e.to_string()))
    }
}
