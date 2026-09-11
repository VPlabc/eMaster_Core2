use std::collections::{BTreeMap, BTreeSet};
use std::sync::{mpsc, Mutex};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RuntimeState {
    Registered,
    Running,
    Stopped,
    Failed,
}

#[derive(Debug, Default)]
pub struct RuntimeManager {
    runtimes: Mutex<BTreeMap<String, RuntimeState>>,
}

impl RuntimeManager {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn register(&self, id: impl Into<String>) -> bool {
        if let Ok(mut runtimes) = self.runtimes.lock() {
            return runtimes
                .insert(id.into(), RuntimeState::Registered)
                .is_none();
        }
        false
    }

    pub fn start(&self, id: &str) -> bool {
        self.transition(id, RuntimeState::Registered, RuntimeState::Running)
    }

    pub fn stop(&self, id: &str) -> bool {
        self.transition(id, RuntimeState::Running, RuntimeState::Stopped)
    }

    pub fn state(&self, id: &str) -> Option<RuntimeState> {
        self.runtimes.lock().ok()?.get(id).copied()
    }

    fn transition(&self, id: &str, from: RuntimeState, to: RuntimeState) -> bool {
        let Ok(mut runtimes) = self.runtimes.lock() else {
            return false;
        };
        if runtimes.get(id) != Some(&from) {
            return false;
        }
        runtimes.insert(id.to_owned(), to);
        true
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ApplicationState {
    Registered,
    Running,
    Stopped,
    Failed,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ApplicationStatus {
    pub id: String,
    pub version: String,
    pub state: ApplicationState,
    pub variables: BTreeMap<String, String>,
    pub configurations: BTreeSet<String>,
    pub connections: BTreeSet<String>,
    pub resources: usize,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ApplicationEvent {
    Started(String),
    Stopped(String),
    Failed(String),
    Removed(String),
}

pub trait Cleanup: Send {
    fn cleanup(&mut self);
}

struct Application {
    version: String,
    state: ApplicationState,
    variables: BTreeMap<String, String>,
    configurations: BTreeSet<String>,
    connections: BTreeSet<String>,
    resources: Vec<Box<dyn Cleanup>>,
}

#[derive(Default)]
pub struct ApplicationManager {
    applications: Mutex<BTreeMap<String, Application>>,
    subscribers: Mutex<Vec<mpsc::Sender<ApplicationEvent>>>,
}

impl ApplicationManager {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn register(&self, id: impl Into<String>, version: impl Into<String>) -> bool {
        let id = id.into();
        let Ok(mut apps) = self.applications.lock() else {
            return false;
        };
        if apps.contains_key(&id) {
            return false;
        }
        apps.insert(
            id,
            Application {
                version: version.into(),
                state: ApplicationState::Registered,
                variables: BTreeMap::new(),
                configurations: BTreeSet::new(),
                connections: BTreeSet::new(),
                resources: Vec::new(),
            },
        );
        true
    }
    pub fn start(&self, id: &str) -> bool {
        self.transition(
            id,
            ApplicationState::Registered,
            ApplicationState::Running,
            Some(ApplicationEvent::Started(id.into())),
        )
    }
    pub fn fail(&self, id: &str) -> bool {
        self.transition(
            id,
            ApplicationState::Running,
            ApplicationState::Failed,
            Some(ApplicationEvent::Failed(id.into())),
        )
    }
    pub fn stop(&self, id: &str) -> bool {
        let changed = self.transition(
            id,
            ApplicationState::Running,
            ApplicationState::Stopped,
            Some(ApplicationEvent::Stopped(id.into())),
        );
        if changed {
            self.cleanup(id);
        }
        changed
    }
    pub fn remove(&self, id: &str) -> bool {
        let Ok(mut apps) = self.applications.lock() else {
            return false;
        };
        let Some(mut app) = apps.remove(id) else {
            return false;
        };
        cleanup_application(&mut app);
        self.emit(ApplicationEvent::Removed(id.into()));
        true
    }
    pub fn state(&self, id: &str) -> Option<ApplicationState> {
        self.applications.lock().ok()?.get(id).map(|app| app.state)
    }
    pub fn status(&self, id: &str) -> Option<ApplicationStatus> {
        let apps = self.applications.lock().ok()?;
        let app = apps.get(id)?;
        Some(ApplicationStatus {
            id: id.into(),
            version: app.version.clone(),
            state: app.state,
            variables: app.variables.clone(),
            configurations: app.configurations.clone(),
            connections: app.connections.clone(),
            resources: app.resources.len(),
        })
    }
    pub fn list(&self) -> Vec<ApplicationStatus> {
        let Ok(apps) = self.applications.lock() else {
            return Vec::new();
        };
        apps.iter()
            .map(|(id, app)| ApplicationStatus {
                id: id.clone(),
                version: app.version.clone(),
                state: app.state,
                variables: app.variables.clone(),
                configurations: app.configurations.clone(),
                connections: app.connections.clone(),
                resources: app.resources.len(),
            })
            .collect()
    }
    pub fn set_variable(&self, id: &str, key: impl Into<String>, value: impl Into<String>) -> bool {
        let Ok(mut apps) = self.applications.lock() else {
            return false;
        };
        let Some(app) = apps.get_mut(id) else {
            return false;
        };
        app.variables.insert(key.into(), value.into());
        true
    }
    pub fn own_configuration(&self, id: &str, configuration_id: impl Into<String>) -> bool {
        let Ok(mut apps) = self.applications.lock() else {
            return false;
        };
        let Some(app) = apps.get_mut(id) else {
            return false;
        };
        app.configurations.insert(configuration_id.into());
        true
    }
    pub fn own_connection(&self, id: &str, connection_id: impl Into<String>) -> bool {
        let Ok(mut apps) = self.applications.lock() else {
            return false;
        };
        let Some(app) = apps.get_mut(id) else {
            return false;
        };
        app.connections.insert(connection_id.into());
        true
    }
    pub fn add_resource(&self, id: &str, resource: Box<dyn Cleanup>) -> bool {
        let Ok(mut apps) = self.applications.lock() else {
            return false;
        };
        let Some(app) = apps.get_mut(id) else {
            return false;
        };
        app.resources.push(resource);
        true
    }
    pub fn subscribe(&self) -> mpsc::Receiver<ApplicationEvent> {
        let (sender, receiver) = mpsc::channel();
        if let Ok(mut subscribers) = self.subscribers.lock() {
            subscribers.push(sender);
        }
        receiver
    }
    fn transition(
        &self,
        id: &str,
        from: ApplicationState,
        to: ApplicationState,
        event: Option<ApplicationEvent>,
    ) -> bool {
        let Ok(mut apps) = self.applications.lock() else {
            return false;
        };
        let Some(app) = apps.get_mut(id) else {
            return false;
        };
        if app.state != from {
            return false;
        }
        app.state = to;
        drop(apps);
        if let Some(event) = event {
            self.emit(event);
        }
        true
    }
    fn cleanup(&self, id: &str) {
        if let Ok(mut apps) = self.applications.lock() {
            if let Some(app) = apps.get_mut(id) {
                for resource in &mut app.resources {
                    resource.cleanup();
                }
                app.resources.clear();
                app.connections.clear();
            }
        }
    }
    fn emit(&self, event: ApplicationEvent) {
        if let Ok(mut subscribers) = self.subscribers.lock() {
            subscribers.retain(|sender| sender.send(event.clone()).is_ok());
        }
    }
}

fn cleanup_application(app: &mut Application) {
    for resource in &mut app.resources {
        resource.cleanup();
    }
    app.resources.clear();
    app.connections.clear();
}

#[cfg(test)]
mod tests {
    use super::{RuntimeManager, RuntimeState};

    #[test]
    fn lifecycle_rejects_invalid_transitions() {
        let manager = RuntimeManager::new();
        assert!(manager.register("script-a"));
        assert!(!manager.register("script-a"));
        assert!(!manager.stop("script-a"));
        assert!(manager.start("script-a"));
        assert_eq!(manager.state("script-a"), Some(RuntimeState::Running));
        assert!(manager.stop("script-a"));
    }
}

#[cfg(test)]
mod application_tests {
    use super::*;
    use std::sync::Arc;
    struct Marker(Arc<Mutex<u32>>);
    impl Cleanup for Marker {
        fn cleanup(&mut self) {
            *self.0.lock().unwrap() += 1;
        }
    }

    #[test]
    fn applications_are_isolated_and_cleanup_on_stop() {
        let manager = ApplicationManager::new();
        let cleaned = Arc::new(Mutex::new(0));
        assert!(manager.register("a", "1.0"));
        assert!(manager.register("b", "1.0"));
        assert!(manager.start("a"));
        assert!(manager.set_variable("a", "state", "ready"));
        assert!(manager.own_configuration("a", "server"));
        assert!(manager.own_connection("a", "rest-1"));
        assert!(manager.add_resource("a", Box::new(Marker(Arc::clone(&cleaned)))));
        assert!(manager.stop("a"));
        assert_eq!(*cleaned.lock().unwrap(), 1);
        assert_eq!(manager.status("b").unwrap().resources, 0);
        assert_eq!(manager.status("a").unwrap().connections.len(), 0);
    }

    #[test]
    fn failures_and_removal_emit_events_and_cleanup() {
        let manager = ApplicationManager::new();
        let events = manager.subscribe();
        let cleaned = Arc::new(Mutex::new(0));
        manager.register("app", "2");
        manager.start("app");
        manager.add_resource("app", Box::new(Marker(Arc::clone(&cleaned))));
        assert!(manager.fail("app"));
        assert_eq!(manager.state("app"), Some(ApplicationState::Failed));
        assert!(manager.remove("app"));
        assert_eq!(*cleaned.lock().unwrap(), 1);
        assert!(matches!(
            events.try_iter().last(),
            Some(ApplicationEvent::Removed(_))
        ));
    }
}
