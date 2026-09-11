use emaster_config::Config;
use emaster_event::EventBus;
use emaster_plugin::{PluginError, PluginManifest, PluginRegistry};
use emaster_runtime::RuntimeManager;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CoreState {
    Created,
    Running,
    Stopped,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CoreError {
    InvalidState,
}

pub struct Core {
    state: CoreState,
    config: Config,
    events: EventBus,
    runtimes: RuntimeManager,
    plugins: PluginRegistry,
}

impl Core {
    pub fn new(config: Config) -> Self {
        Self {
            state: CoreState::Created,
            config,
            events: EventBus::new(),
            runtimes: RuntimeManager::new(),
            plugins: PluginRegistry::default(),
        }
    }

    pub fn start(&mut self) -> Result<(), CoreError> {
        if self.state != CoreState::Created {
            return Err(CoreError::InvalidState);
        }
        self.state = CoreState::Running;
        self.events.publish(emaster_event::Event {
            topic: "system.started".into(),
            payload: "{}".into(),
        });
        Ok(())
    }

    pub fn stop(&mut self) -> Result<(), CoreError> {
        if self.state != CoreState::Running {
            return Err(CoreError::InvalidState);
        }
        self.state = CoreState::Stopped;
        self.events.publish(emaster_event::Event {
            topic: "system.stopped".into(),
            payload: "{}".into(),
        });
        Ok(())
    }

    pub fn state(&self) -> CoreState {
        self.state
    }
    pub fn config(&self) -> &Config {
        &self.config
    }
    pub fn events(&self) -> &EventBus {
        &self.events
    }
    pub fn runtimes(&self) -> &RuntimeManager {
        &self.runtimes
    }
    pub fn register_plugin(
        &mut self,
        manifest: PluginManifest,
        supported_api: u32,
    ) -> Result<(), PluginError> {
        self.plugins.register(manifest, supported_api)
    }
    pub fn plugins(&self) -> &PluginRegistry {
        &self.plugins
    }
}

#[cfg(test)]
mod tests {
    use super::{Core, CoreState};
    use emaster_config::Config;
    use emaster_plugin::PluginManifest;

    #[test]
    fn lifecycle_is_explicit_and_publishes_events() {
        let mut core = Core::new(Config::new());
        let events = core.events().subscribe();
        assert_eq!(core.state(), CoreState::Created);
        assert!(core.start().is_ok());
        assert_eq!(events.recv().expect("start event").topic, "system.started");
        assert!(core.start().is_err());
        assert!(core.stop().is_ok());
        assert_eq!(events.recv().expect("stop event").topic, "system.stopped");
        assert!(core.stop().is_err());
    }

    #[test]
    fn core_composes_runtime_and_plugin_services() {
        let mut core = Core::new(Config::new());
        assert!(core.runtimes().register("script-a"));
        assert!(core.runtimes().start("script-a"));
        assert_eq!(
            core.runtimes().state("script-a"),
            Some(emaster_runtime::RuntimeState::Running)
        );
        let manifest = PluginManifest {
            id: "example".into(),
            version: "1.0.0".into(),
            api_version: 1,
            capabilities: vec!["test".into()],
        };
        assert!(core.register_plugin(manifest, 1).is_ok());
        assert!(core.plugins().get("example").is_some());
    }
}
