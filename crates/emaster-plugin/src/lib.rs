use std::collections::BTreeMap;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PluginManifest {
    pub id: String,
    pub version: String,
    pub api_version: u32,
    pub capabilities: Vec<String>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum PluginError {
    EmptyId,
    EmptyVersion,
    UnsupportedApi { expected: u32, actual: u32 },
    AlreadyInstalled,
    NotInstalled,
    InvalidState,
    InvalidPackage,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PluginPackage {
    pub manifest: PluginManifest,
    pub files: Vec<String>,
}

impl PluginPackage {
    pub fn validate(&self, supported_api: u32) -> Result<(), PluginError> {
        self.manifest.validate(supported_api)?;
        if self.files.is_empty()
            || self.files.iter().any(|file| {
                file.trim().is_empty()
                    || file.starts_with('/')
                    || file.starts_with('\\')
                    || file.split(['/', '\\']).any(|part| part == "..")
            })
        {
            return Err(PluginError::InvalidPackage);
        }
        Ok(())
    }
}

impl PluginManifest {
    pub fn validate(&self, supported_api: u32) -> Result<(), PluginError> {
        if self.id.trim().is_empty() {
            return Err(PluginError::EmptyId);
        }
        if self.version.trim().is_empty() {
            return Err(PluginError::EmptyVersion);
        }
        if self.api_version != supported_api {
            return Err(PluginError::UnsupportedApi {
                expected: supported_api,
                actual: self.api_version,
            });
        }
        Ok(())
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PluginState {
    Installed,
    Enabled,
    Disabled,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct InstalledPlugin {
    pub manifest: PluginManifest,
    pub state: PluginState,
}

#[derive(Debug, Default)]
pub struct PluginRegistry {
    plugins: BTreeMap<String, InstalledPlugin>,
}

impl PluginRegistry {
    pub fn install(
        &mut self,
        package: PluginPackage,
        supported_api: u32,
    ) -> Result<(), PluginError> {
        package.validate(supported_api)?;
        self.register(package.manifest, supported_api)
    }

    pub fn register(
        &mut self,
        manifest: PluginManifest,
        supported_api: u32,
    ) -> Result<(), PluginError> {
        manifest.validate(supported_api)?;
        if self.plugins.contains_key(&manifest.id) {
            return Err(PluginError::AlreadyInstalled);
        }
        self.plugins.insert(
            manifest.id.clone(),
            InstalledPlugin {
                manifest,
                state: PluginState::Installed,
            },
        );
        Ok(())
    }

    pub fn get(&self, id: &str) -> Option<&PluginManifest> {
        self.plugins.get(id).map(|plugin| &plugin.manifest)
    }

    pub fn state(&self, id: &str) -> Option<PluginState> {
        self.plugins.get(id).map(|plugin| plugin.state)
    }

    pub fn enable(&mut self, id: &str) -> Result<(), PluginError> {
        let plugin = self.plugins.get_mut(id).ok_or(PluginError::NotInstalled)?;
        if plugin.state == PluginState::Enabled {
            return Err(PluginError::InvalidState);
        }
        plugin.state = PluginState::Enabled;
        Ok(())
    }

    pub fn disable(&mut self, id: &str) -> Result<(), PluginError> {
        let plugin = self.plugins.get_mut(id).ok_or(PluginError::NotInstalled)?;
        if plugin.state != PluginState::Enabled {
            return Err(PluginError::InvalidState);
        }
        plugin.state = PluginState::Disabled;
        Ok(())
    }

    pub fn update(
        &mut self,
        manifest: PluginManifest,
        supported_api: u32,
    ) -> Result<(), PluginError> {
        manifest.validate(supported_api)?;
        let plugin = self
            .plugins
            .get_mut(&manifest.id)
            .ok_or(PluginError::NotInstalled)?;
        let state = plugin.state;
        plugin.manifest = manifest;
        plugin.state = state;
        Ok(())
    }

    pub fn remove(&mut self, id: &str) -> Result<PluginManifest, PluginError> {
        let plugin = self.plugins.get(id).ok_or(PluginError::NotInstalled)?;
        if plugin.state == PluginState::Enabled {
            return Err(PluginError::InvalidState);
        }
        self.plugins
            .remove(id)
            .map(|plugin| plugin.manifest)
            .ok_or(PluginError::NotInstalled)
    }
}

#[cfg(test)]
mod tests {
    use super::{PluginError, PluginManifest, PluginPackage, PluginRegistry, PluginState};

    fn manifest(api_version: u32) -> PluginManifest {
        PluginManifest {
            id: "example".into(),
            version: "1.0.0".into(),
            api_version,
            capabilities: vec![],
        }
    }

    #[test]
    fn validation_precedes_registration() {
        let mut registry = PluginRegistry::default();
        assert_eq!(
            registry.register(manifest(2), 1),
            Err(PluginError::UnsupportedApi {
                expected: 1,
                actual: 2
            })
        );
        assert!(registry.register(manifest(1), 1).is_ok());
        assert!(registry.get("example").is_some());
        assert_eq!(registry.state("example"), Some(PluginState::Installed));
        assert_eq!(
            registry.register(manifest(1), 1),
            Err(PluginError::AlreadyInstalled)
        );
    }

    #[test]
    fn lifecycle_prevents_removing_enabled_plugins() {
        let mut registry = PluginRegistry::default();
        registry.register(manifest(1), 1).unwrap();
        registry.enable("example").unwrap();
        assert_eq!(registry.remove("example"), Err(PluginError::InvalidState));
        registry.disable("example").unwrap();
        assert!(registry.remove("example").is_ok());
        assert_eq!(registry.state("example"), None);
    }

    #[test]
    fn update_validates_api_and_preserves_state() {
        let mut registry = PluginRegistry::default();
        registry.register(manifest(1), 1).unwrap();
        registry.enable("example").unwrap();
        let mut replacement = manifest(1);
        replacement.version = "2.0.0".into();
        registry.update(replacement, 1).unwrap();
        assert_eq!(registry.state("example"), Some(PluginState::Enabled));
        assert_eq!(
            registry.get("example").map(|m| m.version.as_str()),
            Some("2.0.0")
        );
    }

    #[test]
    fn package_validation_rejects_path_traversal() {
        let mut registry = PluginRegistry::default();
        let package = PluginPackage {
            manifest: manifest(1),
            files: vec!["plugin/main.dll".into(), "../secret".into()],
        };
        assert_eq!(
            registry.install(package, 1),
            Err(PluginError::InvalidPackage)
        );
    }
}
