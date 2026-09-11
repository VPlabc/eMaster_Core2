use std::collections::BTreeMap;
use std::path::Path;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum LuaError {
    EmptyScript,
    ForbiddenResource,
    Runtime,
    InvalidHandle,
    AlreadyExists,
    Connectivity(String),
    Configuration(String),
}

pub trait HostApi {
    fn call(&mut self, module: &str, function: &str, argument: &str) -> Result<String, LuaError>;
}

pub struct Script<'a, H: HostApi> {
    host: &'a mut H,
    max_argument_len: usize,
}

impl<'a, H: HostApi> Script<'a, H> {
    pub fn new(host: &'a mut H) -> Self {
        Self {
            host,
            max_argument_len: 4096,
        }
    }
    pub fn with_argument_limit(mut self, max: usize) -> Self {
        self.max_argument_len = max;
        self
    }
    pub fn invoke(
        &mut self,
        module: &str,
        function: &str,
        argument: &str,
    ) -> Result<String, LuaError> {
        if module.trim().is_empty() || function.trim().is_empty() {
            return Err(LuaError::EmptyScript);
        }
        if argument.len() > self.max_argument_len {
            return Err(LuaError::Runtime);
        }
        if module == "os" || module == "io" {
            return Err(LuaError::ForbiddenResource);
        }
        self.host.call(module, function, argument)
    }
}

/// Resource type exposed to Lua without exposing native client objects.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ConnectivityKind {
    Rest,
    Mqtt,
    RabbitMq,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HandleState {
    Created,
    Started,
    Stopped,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConnectivityHandle {
    pub id: u64,
    pub owner: String,
    pub name: String,
    pub kind: ConnectivityKind,
    pub state: HandleState,
}

#[derive(Default)]
pub struct ConnectivityRegistry {
    next_id: u64,
    handles: Vec<ConnectivityHandle>,
}

impl ConnectivityRegistry {
    pub fn new() -> Self {
        Self {
            next_id: 1,
            handles: Vec::new(),
        }
    }
    pub fn create(
        &mut self,
        owner: impl Into<String>,
        kind: ConnectivityKind,
        name: impl Into<String>,
    ) -> Result<u64, LuaError> {
        let owner = owner.into();
        let name = name.into();
        if owner.trim().is_empty() || name.trim().is_empty() {
            return Err(LuaError::Runtime);
        }
        if self
            .handles
            .iter()
            .any(|handle| handle.owner == owner && handle.name == name)
        {
            return Err(LuaError::AlreadyExists);
        }
        let id = self.next_id;
        self.next_id = self.next_id.checked_add(1).ok_or(LuaError::Runtime)?;
        self.handles.push(ConnectivityHandle {
            id,
            owner,
            name,
            kind,
            state: HandleState::Created,
        });
        Ok(id)
    }
    pub fn start(&mut self, owner: &str, id: u64) -> Result<(), LuaError> {
        let handle = self.authorized_mut(owner, id)?;
        handle.state = HandleState::Started;
        Ok(())
    }
    pub fn stop(&mut self, owner: &str, id: u64) -> Result<(), LuaError> {
        let handle = self.authorized_mut(owner, id)?;
        handle.state = HandleState::Stopped;
        Ok(())
    }
    pub fn destroy(&mut self, owner: &str, id: u64) -> Result<(), LuaError> {
        let index = self
            .handles
            .iter()
            .position(|handle| handle.id == id && handle.owner == owner)
            .ok_or(LuaError::InvalidHandle)?;
        self.handles.remove(index);
        Ok(())
    }
    pub fn get(&self, owner: &str, id: u64) -> Result<&ConnectivityHandle, LuaError> {
        self.handles
            .iter()
            .find(|handle| handle.id == id && handle.owner == owner)
            .ok_or(LuaError::InvalidHandle)
    }
    pub fn list(&self, owner: &str) -> Vec<ConnectivityHandle> {
        self.handles
            .iter()
            .filter(|handle| handle.owner == owner)
            .cloned()
            .collect()
    }
    pub fn destroy_owner(&mut self, owner: &str) {
        self.handles.retain(|handle| handle.owner != owner);
    }
    fn authorized_mut(
        &mut self,
        owner: &str,
        id: u64,
    ) -> Result<&mut ConnectivityHandle, LuaError> {
        self.handles
            .iter_mut()
            .find(|handle| handle.id == id && handle.owner == owner)
            .ok_or(LuaError::InvalidHandle)
    }
}

/// Host-owned bridge between opaque Lua handles and the native connectivity
/// managers. Lua never receives the manager objects or transport internals.
pub struct ConnectivityRuntime {
    pub registry: ConnectivityRegistry,
    pub rest: emaster_http::HttpManager,
    pub mqtt: emaster_mqtt::MqttManager,
    pub rabbitmq: emaster_rabbitmq::RabbitManager,
}

/// Application-facing configuration API. Storage and validation remain owned
/// by `emaster-config`; Lua only supplies schemas and values.
pub struct ConfigRuntime {
    pub manager: emaster_config::ApplicationConfigManager,
    owners: BTreeMap<String, String>,
}

/// Coordinates application lifecycle with all application-owned services.
/// Persistent configuration intentionally survives `remove`; callers may
/// delete it through the configuration store when the product's retention
/// policy requires that behavior.
pub struct ApplicationRuntime {
    pub applications: emaster_runtime::ApplicationManager,
    pub connectivity: ConnectivityRuntime,
    pub configuration: ConfigRuntime,
}

impl Default for ApplicationRuntime {
    fn default() -> Self {
        Self::new()
    }
}

impl ApplicationRuntime {
    pub fn new() -> Self {
        Self {
            applications: emaster_runtime::ApplicationManager::new(),
            connectivity: ConnectivityRuntime::new(),
            configuration: ConfigRuntime::new(),
        }
    }

    pub fn register(&self, id: impl Into<String>, version: impl Into<String>) -> bool {
        self.applications.register(id, version)
    }

    pub fn start(&self, id: &str) -> bool {
        self.applications.start(id)
    }

    pub fn stop(&mut self, id: &str) -> bool {
        let stopped = self.applications.stop(id);
        if stopped {
            self.connectivity.destroy_owner(id);
        }
        stopped
    }

    pub fn fail(&mut self, id: &str) -> bool {
        let failed = self.applications.fail(id);
        if failed {
            self.connectivity.destroy_owner(id);
        }
        failed
    }

    pub fn remove(&mut self, id: &str) -> bool {
        let removed = self.applications.remove(id);
        if removed {
            self.connectivity.destroy_owner(id);
            self.configuration.remove_owner(id);
        }
        removed
    }
}

impl Default for ConfigRuntime {
    fn default() -> Self {
        Self {
            manager: emaster_config::ApplicationConfigManager::default(),
            owners: BTreeMap::new(),
        }
    }
}
impl ConfigRuntime {
    pub fn new() -> Self {
        Self::default()
    }
    pub fn subscribe(&mut self) -> std::sync::mpsc::Receiver<emaster_config::ConfigEvent> {
        self.manager.subscribe()
    }
    pub fn declare(&mut self, owner: &str, schema: emaster_config::Schema) -> Result<(), LuaError> {
        if owner.trim().is_empty() || owner != schema.application {
            return Err(LuaError::Configuration(
                "schema application does not match owner".into(),
            ));
        }
        self.manager
            .declare(schema)
            .map_err(|e| LuaError::Configuration(format!("{e:?}")))?;
        self.owners.insert(owner.into(), owner.into());
        Ok(())
    }
    pub fn get(&self, owner: &str) -> Result<&BTreeMap<String, emaster_config::Value>, LuaError> {
        if !self.owners.contains_key(owner) {
            return Err(LuaError::Configuration(
                "application schema not declared".into(),
            ));
        }
        self.manager
            .get(owner)
            .ok_or_else(|| LuaError::Configuration("configuration not found".into()))
    }
    pub fn set(
        &mut self,
        owner: &str,
        values: BTreeMap<String, emaster_config::Value>,
    ) -> Result<(), LuaError> {
        self.get(owner).map(|_| ())?;
        self.manager
            .set(owner, values)
            .map_err(|e| LuaError::Configuration(format!("{e:?}")))
    }
    pub fn save_json(&mut self, owner: &str, path: impl AsRef<Path>) -> Result<(), LuaError> {
        self.get(owner).map(|_| ())?;
        self.manager
            .save_json(owner, path)
            .map_err(|e| LuaError::Configuration(format!("{e:?}")))
    }
    pub fn reload_json(&mut self, owner: &str, path: impl AsRef<Path>) -> Result<(), LuaError> {
        self.get(owner).map(|_| ())?;
        self.manager
            .reload_json(owner, path)
            .map_err(|e| LuaError::Configuration(format!("{e:?}")))
    }
    pub fn remove_owner(&mut self, owner: &str) {
        self.owners.remove(owner);
    }
}

impl Default for ConnectivityRuntime {
    fn default() -> Self {
        Self::new()
    }
}

impl ConnectivityRuntime {
    pub fn new() -> Self {
        Self {
            registry: ConnectivityRegistry::new(),
            rest: emaster_http::HttpManager::new(),
            mqtt: emaster_mqtt::MqttManager::new(),
            rabbitmq: emaster_rabbitmq::RabbitManager::new(),
        }
    }
    #[cfg(feature = "production-transports")]
    pub fn with_production_transports() -> Result<Self, LuaError> {
        let rabbitmq = emaster_rabbitmq::LapinTransport::new()
            .map_err(|error| LuaError::Connectivity(format!("{error:?}")))?;
        Ok(Self {
            registry: ConnectivityRegistry::new(),
            rest: emaster_http::HttpManager::with_transport(std::sync::Arc::new(
                emaster_http::TcpHttpTransport,
            )),
            mqtt: emaster_mqtt::MqttManager::with_transport(std::sync::Arc::new(
                emaster_mqtt::RumqttTransport::new(),
            )),
            rabbitmq: emaster_rabbitmq::RabbitManager::with_transport(std::sync::Arc::new(
                rabbitmq,
            )),
        })
    }
    pub fn create_rest(
        &mut self,
        owner: &str,
        config: emaster_http::ConnectionConfig,
    ) -> Result<u64, LuaError> {
        if config.owner != owner {
            return Err(LuaError::Connectivity("connection owner mismatch".into()));
        }
        let handle = self
            .registry
            .create(owner, ConnectivityKind::Rest, &config.name)?;
        if let Err(error) = self.rest.create(handle.to_string(), config) {
            let _ = self.registry.destroy(owner, handle);
            return Err(LuaError::Connectivity(format!("{error:?}")));
        }
        Ok(handle)
    }
    pub fn create_mqtt(
        &mut self,
        owner: &str,
        config: emaster_mqtt::MqttConfig,
    ) -> Result<u64, LuaError> {
        if config.owner != owner {
            return Err(LuaError::Connectivity("connection owner mismatch".into()));
        }
        let handle = self
            .registry
            .create(owner, ConnectivityKind::Mqtt, &config.name)?;
        if let Err(error) = self.mqtt.create(handle.to_string(), config) {
            let _ = self.registry.destroy(owner, handle);
            return Err(LuaError::Connectivity(format!("{error:?}")));
        }
        Ok(handle)
    }
    pub fn create_rabbitmq(
        &mut self,
        owner: &str,
        config: emaster_rabbitmq::RabbitConfig,
    ) -> Result<u64, LuaError> {
        if config.owner != owner {
            return Err(LuaError::Connectivity("connection owner mismatch".into()));
        }
        let handle = self
            .registry
            .create(owner, ConnectivityKind::RabbitMq, &config.name)?;
        if let Err(error) = self.rabbitmq.create(handle.to_string(), config) {
            let _ = self.registry.destroy(owner, handle);
            return Err(LuaError::Connectivity(format!("{error:?}")));
        }
        Ok(handle)
    }
    pub fn start(&mut self, owner: &str, handle: u64) -> Result<(), LuaError> {
        let resource = self.registry.get(owner, handle)?.clone();
        let id = handle.to_string();
        let result = match resource.kind {
            ConnectivityKind::Rest => self.rest.start(&id).map_err(|e| format!("{e:?}")),
            ConnectivityKind::Mqtt => self.mqtt.start(&id).map_err(|e| format!("{e:?}")),
            ConnectivityKind::RabbitMq => self.rabbitmq.start(&id).map_err(|e| format!("{e:?}")),
        };
        result
            .map_err(LuaError::Connectivity)
            .and_then(|_| self.registry.start(owner, handle))
    }
    pub fn stop(&mut self, owner: &str, handle: u64) -> Result<(), LuaError> {
        let resource = self.registry.get(owner, handle)?.clone();
        let id = handle.to_string();
        let result = match resource.kind {
            ConnectivityKind::Rest => self.rest.stop(&id).map_err(|e| format!("{e:?}")),
            ConnectivityKind::Mqtt => self.mqtt.stop(&id).map_err(|e| format!("{e:?}")),
            ConnectivityKind::RabbitMq => self.rabbitmq.stop(&id).map_err(|e| format!("{e:?}")),
        };
        result
            .map_err(LuaError::Connectivity)
            .and_then(|_| self.registry.stop(owner, handle))
    }
    pub fn destroy(&mut self, owner: &str, handle: u64) -> Result<(), LuaError> {
        let resource = self.registry.get(owner, handle)?.clone();
        let id = handle.to_string();
        let result = match resource.kind {
            ConnectivityKind::Rest => self.rest.destroy(&id).map_err(|e| format!("{e:?}")),
            ConnectivityKind::Mqtt => self.mqtt.destroy(&id).map_err(|e| format!("{e:?}")),
            ConnectivityKind::RabbitMq => self.rabbitmq.destroy(&id).map_err(|e| format!("{e:?}")),
        };
        result
            .map_err(LuaError::Connectivity)
            .and_then(|_| self.registry.destroy(owner, handle))
    }
    pub fn destroy_owner(&mut self, owner: &str) {
        self.rest.destroy_owner(owner);
        self.mqtt.destroy_owner(owner);
        self.rabbitmq.destroy_owner(owner);
        self.registry.destroy_owner(owner);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    struct Mock;
    impl HostApi for Mock {
        fn call(
            &mut self,
            module: &str,
            function: &str,
            argument: &str,
        ) -> Result<String, LuaError> {
            Ok(format!("{module}.{function}:{argument}"))
        }
    }
    #[test]
    fn controlled_api_blocks_os_access() {
        let mut mock = Mock;
        let mut script = Script::new(&mut mock);
        assert_eq!(
            script.invoke("modbus", "read", "1"),
            Ok("modbus.read:1".into())
        );
        assert_eq!(
            script.invoke("io", "open", "x"),
            Err(LuaError::ForbiddenResource)
        );
    }

    #[test]
    fn argument_limit_is_enforced_before_host_call() {
        let mut mock = Mock;
        let mut script = Script::new(&mut mock).with_argument_limit(2);
        assert_eq!(
            script.invoke("modbus", "write", "123"),
            Err(LuaError::Runtime)
        );
    }
}

#[cfg(test)]
mod connectivity_tests {
    use super::*;
    #[test]
    fn handles_are_opaque_and_owner_scoped() {
        let mut registry = ConnectivityRegistry::new();
        let rest = registry
            .create("app-a", ConnectivityKind::Rest, "main")
            .unwrap();
        let mqtt = registry
            .create("app-b", ConnectivityKind::Mqtt, "main")
            .unwrap();
        registry.start("app-a", rest).unwrap();
        assert_eq!(
            registry.get("app-a", rest).unwrap().state,
            HandleState::Started
        );
        assert_eq!(registry.get("app-b", rest), Err(LuaError::InvalidHandle));
        assert_eq!(registry.list("app-a").len(), 1);
        registry.destroy_owner("app-a");
        assert!(registry.get("app-a", rest).is_err());
        assert!(registry.get("app-b", mqtt).is_ok());
    }
    #[test]
    fn duplicate_names_and_invalid_lifecycle_are_rejected() {
        let mut registry = ConnectivityRegistry::new();
        assert_eq!(
            registry.create("", ConnectivityKind::RabbitMq, "bus"),
            Err(LuaError::Runtime)
        );
        registry
            .create("app", ConnectivityKind::RabbitMq, "bus")
            .unwrap();
        assert_eq!(
            registry.create("app", ConnectivityKind::RabbitMq, "bus"),
            Err(LuaError::AlreadyExists)
        );
        assert_eq!(registry.stop("other", 1), Err(LuaError::InvalidHandle));
    }

    #[test]
    fn runtime_binds_lua_handles_to_native_rest_manager() {
        let mut runtime = ConnectivityRuntime::new();
        let handle = runtime
            .create_rest(
                "app",
                emaster_http::ConnectionConfig {
                    name: "api".into(),
                    url: "https://example.test".into(),
                    owner: "app".into(),
                    timeout_ms: 100,
                    max_retries: 0,
                    authentication: emaster_http::Authentication::None,
                },
            )
            .unwrap();
        runtime.start("app", handle).unwrap();
        assert_eq!(
            runtime.rest.status(&handle.to_string()).unwrap().state,
            emaster_http::ConnectionState::Connected
        );
        runtime.destroy_owner("app");
        assert!(runtime.registry.get("app", handle).is_err());
        assert!(runtime.rest.status(&handle.to_string()).is_err());
    }

    #[test]
    fn config_runtime_requires_owner_matching_schema() {
        let mut runtime = ConfigRuntime::new();
        let schema = emaster_config::Schema {
            application: "app".into(),
            version: "1".into(),
            fields: vec![emaster_config::Field {
                id: "enabled".into(),
                field_type: emaster_config::FieldType::Boolean,
                required: true,
                default: None,
            }],
        };
        assert!(runtime.declare("other", schema.clone()).is_err());
        runtime.declare("app", schema).unwrap();
        let mut values = BTreeMap::new();
        values.insert("enabled".into(), emaster_config::Value::Boolean(true));
        runtime.set("app", values).unwrap();
        assert!(runtime.get("other").is_err());
        runtime.remove_owner("app");
        assert!(runtime.get("app").is_err());
    }

    #[test]
    fn application_runtime_cleans_connectivity_without_deleting_config() {
        let mut runtime = ApplicationRuntime::new();
        assert!(runtime.register("app", "1"));
        assert!(runtime.start("app"));
        let handle = runtime
            .connectivity
            .create_rest(
                "app",
                emaster_http::ConnectionConfig {
                    name: "api".into(),
                    url: "https://example.test".into(),
                    owner: "app".into(),
                    timeout_ms: 100,
                    max_retries: 0,
                    authentication: emaster_http::Authentication::None,
                },
            )
            .unwrap();
        assert!(runtime.stop("app"));
        assert!(runtime.connectivity.registry.get("app", handle).is_err());
        assert_eq!(
            runtime.applications.state("app"),
            Some(emaster_runtime::ApplicationState::Stopped)
        );
    }

    #[test]
    fn connection_owner_must_match_lua_application() {
        let mut runtime = ConnectivityRuntime::new();
        let result = runtime.create_mqtt(
            "app-a",
            emaster_mqtt::MqttConfig {
                name: "broker".into(),
                owner: "app-b".into(),
                host: "localhost".into(),
                port: 1883,
                client_id: "client".into(),
                timeout_ms: 100,
                max_retries: 0,
            },
        );
        assert!(matches!(result, Err(LuaError::Connectivity(_))));
    }
}
