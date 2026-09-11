use std::sync::{mpsc, Arc, Mutex};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Event {
    pub topic: String,
    pub payload: String,
}

#[derive(Clone, Default)]
pub struct EventBus {
    subscribers: Arc<Mutex<Vec<mpsc::Sender<Event>>>>,
}

impl EventBus {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn subscribe(&self) -> mpsc::Receiver<Event> {
        let (sender, receiver) = mpsc::channel();
        if let Ok(mut subscribers) = self.subscribers.lock() {
            subscribers.push(sender);
        }
        receiver
    }

    pub fn publish(&self, event: Event) -> usize {
        let mut delivered = 0;
        if let Ok(mut subscribers) = self.subscribers.lock() {
            subscribers.retain(|subscriber| {
                if subscriber.send(event.clone()).is_ok() {
                    delivered += 1;
                    true
                } else {
                    false
                }
            });
        }
        delivered
    }
}

#[cfg(test)]
mod tests {
    use super::{Event, EventBus};

    #[test]
    fn publish_fans_out_and_removes_closed_subscribers() {
        let bus = EventBus::new();
        let first = bus.subscribe();
        let second = bus.subscribe();
        assert_eq!(
            bus.publish(Event {
                topic: "x".into(),
                payload: "1".into()
            }),
            2
        );
        assert_eq!(first.recv().expect("subscriber is alive").payload, "1");
        assert_eq!(second.recv().expect("subscriber is alive").topic, "x");
        drop(second);
        assert_eq!(
            bus.publish(Event {
                topic: "x".into(),
                payload: "2".into()
            }),
            1
        );
    }
}
