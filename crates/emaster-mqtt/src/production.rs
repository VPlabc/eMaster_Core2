use crate::{MqttConfig, MqttError, MqttTransport, Publication, Subscription};
use rumqttc::{Client, Connection, MqttOptions, QoS as RumqttQoS};
use std::collections::BTreeMap;
use std::sync::{mpsc, Mutex};
use std::time::Duration;

struct Session {
    client: Client,
    _worker: std::thread::JoinHandle<()>,
}

/// `rumqttc` adapter for the manager's synchronous transport boundary.
/// The event loop is kept alive on a dedicated worker; callers only see the
/// manager's transport-neutral API.
#[derive(Default)]
pub struct RumqttTransport {
    sessions: Mutex<BTreeMap<String, Session>>,
}

impl RumqttTransport {
    pub fn new() -> Self {
        Self::default()
    }
}

impl MqttTransport for RumqttTransport {
    fn connect(&self, config: &MqttConfig, timeout: Duration) -> Result<(), MqttError> {
        let mut options = MqttOptions::new(&config.client_id, &config.host, config.port);
        options.set_keep_alive(Duration::from_secs(30));
        let (client, connection) = Client::new(options, 32);
        let (ready_sender, ready_receiver) = mpsc::channel();
        let worker = spawn_event_loop(connection, ready_sender);
        match ready_receiver.recv_timeout(timeout) {
            Ok(Ok(())) => {}
            Ok(Err(error)) => return Err(MqttError::Transport(error)),
            Err(mpsc::RecvTimeoutError::Timeout) => {
                return Err(MqttError::Transport("MQTT connection timed out".into()))
            }
            Err(mpsc::RecvTimeoutError::Disconnected) => {
                return Err(MqttError::Transport("MQTT event loop stopped".into()))
            }
        }
        self.sessions
            .lock()
            .map_err(|_| MqttError::Transport("MQTT session lock poisoned".into()))?
            .insert(
                config.name.clone(),
                Session {
                    client,
                    _worker: worker,
                },
            );
        Ok(())
    }
    fn disconnect(&self, config: &MqttConfig) -> Result<(), MqttError> {
        self.sessions
            .lock()
            .map_err(|_| MqttError::Transport("MQTT session lock poisoned".into()))?
            .remove(&config.name);
        Ok(())
    }
    fn publish(&self, config: &MqttConfig, publication: &Publication) -> Result<(), MqttError> {
        let sessions = self
            .sessions
            .lock()
            .map_err(|_| MqttError::Transport("MQTT session lock poisoned".into()))?;
        let session = sessions.get(&config.name).ok_or(MqttError::NotConnected)?;
        session
            .client
            .try_publish(
                &publication.topic,
                to_qos(publication.qos),
                publication.retained,
                publication.payload.clone(),
            )
            .map_err(|e| MqttError::Transport(e.to_string()))
    }
    fn subscribe(&self, config: &MqttConfig, subscription: &Subscription) -> Result<(), MqttError> {
        let sessions = self
            .sessions
            .lock()
            .map_err(|_| MqttError::Transport("MQTT session lock poisoned".into()))?;
        let session = sessions.get(&config.name).ok_or(MqttError::NotConnected)?;
        session
            .client
            .subscribe(&subscription.topic_filter, to_qos(subscription.qos))
            .map_err(|e| MqttError::Transport(e.to_string()))
    }
    fn unsubscribe(&self, config: &MqttConfig, topic_filter: &str) -> Result<(), MqttError> {
        let sessions = self
            .sessions
            .lock()
            .map_err(|_| MqttError::Transport("MQTT session lock poisoned".into()))?;
        let session = sessions.get(&config.name).ok_or(MqttError::NotConnected)?;
        session
            .client
            .unsubscribe(topic_filter)
            .map_err(|e| MqttError::Transport(e.to_string()))
    }
}

fn to_qos(qos: crate::QoS) -> RumqttQoS {
    match qos {
        crate::QoS::AtMostOnce => RumqttQoS::AtMostOnce,
        crate::QoS::AtLeastOnce => RumqttQoS::AtLeastOnce,
        crate::QoS::ExactlyOnce => RumqttQoS::ExactlyOnce,
    }
}

fn spawn_event_loop(
    mut connection: Connection,
    ready_sender: mpsc::Sender<Result<(), String>>,
) -> std::thread::JoinHandle<()> {
    std::thread::spawn(move || {
        let mut ready = false;
        for event in connection.iter() {
            match event {
                Ok(_) if !ready => {
                    ready = true;
                    let _ = ready_sender.send(Ok(()));
                }
                Err(error) => {
                    if !ready {
                        let _ = ready_sender.send(Err(error.to_string()));
                    }
                    break;
                }
                Ok(_) => {}
            }
        }
    })
}
