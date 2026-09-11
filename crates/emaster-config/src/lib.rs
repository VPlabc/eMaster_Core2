use std::collections::BTreeMap;
use std::fs;
use std::path::Path;
use std::sync::mpsc;

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Config {
    values: BTreeMap<String, String>,
}

#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    String(String),
    Number(f64),
    Integer(i64),
    Boolean(bool),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FieldType {
    String,
    Password,
    Number,
    Integer,
    Boolean,
    Url,
    Port,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Field {
    pub id: String,
    pub field_type: FieldType,
    pub required: bool,
    pub default: Option<Value>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Schema {
    pub application: String,
    pub version: String,
    pub fields: Vec<Field>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ConfigError {
    InvalidSchema,
    UnknownField(String),
    MissingField(String),
    WrongType(String),
    InvalidValue(String),
    Io(String),
    Parse(String),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum ConfigEvent {
    Created { application: String },
    Updated { application: String },
    Reloaded { application: String },
    ValidationFailed { application: String },
}

impl Schema {
    pub fn validate(&self) -> Result<(), ConfigError> {
        if self.application.trim().is_empty()
            || self.version.trim().is_empty()
            || self.fields.is_empty()
        {
            return Err(ConfigError::InvalidSchema);
        }
        let mut ids = BTreeMap::new();
        for field in &self.fields {
            if field.id.trim().is_empty() || ids.insert(&field.id, ()).is_some() {
                return Err(ConfigError::InvalidSchema);
            }
            if let Some(value) = &field.default {
                validate_type(field, value)?;
            }
        }
        Ok(())
    }
    pub fn defaults(&self) -> BTreeMap<String, Value> {
        self.fields
            .iter()
            .filter_map(|f| f.default.clone().map(|v| (f.id.clone(), v)))
            .collect()
    }
    pub fn validate_values(&self, values: &BTreeMap<String, Value>) -> Result<(), ConfigError> {
        self.validate()?;
        for key in values.keys() {
            if !self.fields.iter().any(|field| field.id == *key) {
                return Err(ConfigError::UnknownField(key.clone()));
            }
        }
        for field in &self.fields {
            match values.get(&field.id) {
                Some(value) => validate_type(field, value)?,
                None if field.required && field.default.is_none() => {
                    return Err(ConfigError::MissingField(field.id.clone()))
                }
                _ => {}
            }
        }
        Ok(())
    }
}

fn validate_type(field: &Field, value: &Value) -> Result<(), ConfigError> {
    let valid = match field.field_type {
        FieldType::String | FieldType::Password => matches!(value, Value::String(_)),
        FieldType::Number => matches!(value, Value::Number(_)),
        FieldType::Integer | FieldType::Port => matches!(value, Value::Integer(_)),
        FieldType::Boolean => matches!(value, Value::Boolean(_)),
        FieldType::Url => {
            matches!(value, Value::String(v) if v.starts_with("http://") || v.starts_with("https://"))
        }
    };
    if !valid {
        return Err(ConfigError::WrongType(field.id.clone()));
    }
    if field.field_type == FieldType::Port {
        if let Value::Integer(port) = value {
            if !(1..=65535).contains(port) {
                return Err(ConfigError::InvalidValue(field.id.clone()));
            }
        }
    }
    Ok(())
}

pub struct ApplicationConfigManager {
    schemas: BTreeMap<String, Schema>,
    values: BTreeMap<String, BTreeMap<String, Value>>,
    subscribers: Vec<mpsc::Sender<ConfigEvent>>,
}

impl Default for ApplicationConfigManager {
    fn default() -> Self {
        Self {
            schemas: BTreeMap::new(),
            values: BTreeMap::new(),
            subscribers: Vec::new(),
        }
    }
}
impl ApplicationConfigManager {
    pub fn declare(&mut self, schema: Schema) -> Result<(), ConfigError> {
        schema.validate()?;
        let app = schema.application.clone();
        self.values
            .entry(app.clone())
            .or_insert_with(|| schema.defaults());
        self.schemas.insert(app, schema);
        Ok(())
    }
    pub fn schema(&self, application: &str) -> Option<&Schema> {
        self.schemas.get(application)
    }
    pub fn get(&self, application: &str) -> Option<&BTreeMap<String, Value>> {
        self.values.get(application)
    }
    pub fn set(
        &mut self,
        application: &str,
        values: BTreeMap<String, Value>,
    ) -> Result<(), ConfigError> {
        let schema = self
            .schemas
            .get(application)
            .ok_or_else(|| ConfigError::UnknownField(application.into()))?;
        let validation = schema.validate_values(&values);
        if validation.is_err() {
            self.emit(ConfigEvent::ValidationFailed {
                application: application.into(),
            });
            return validation;
        }
        self.values.insert(application.into(), values);
        self.emit(ConfigEvent::Updated {
            application: application.into(),
        });
        Ok(())
    }
    pub fn subscribe(&mut self) -> mpsc::Receiver<ConfigEvent> {
        let (sender, receiver) = mpsc::channel();
        self.subscribers.push(sender);
        receiver
    }
    fn emit(&mut self, event: ConfigEvent) {
        self.subscribers
            .retain(|sender| sender.send(event.clone()).is_ok());
    }
    pub fn save_json(
        &mut self,
        application: &str,
        path: impl AsRef<Path>,
    ) -> Result<(), ConfigError> {
        let values = self
            .values
            .get(application)
            .ok_or_else(|| ConfigError::UnknownField(application.into()))?;
        fs::write(path, encode_json(values)).map_err(|e| ConfigError::Io(e.to_string()))?;
        self.emit(ConfigEvent::Created {
            application: application.into(),
        });
        Ok(())
    }
    pub fn reload_json(
        &mut self,
        application: &str,
        path: impl AsRef<Path>,
    ) -> Result<(), ConfigError> {
        let schema = self
            .schemas
            .get(application)
            .ok_or_else(|| ConfigError::UnknownField(application.into()))?
            .clone();
        let values =
            decode_json(&fs::read_to_string(path).map_err(|e| ConfigError::Io(e.to_string()))?)?;
        schema.validate_values(&values).map_err(|e| {
            self.emit(ConfigEvent::ValidationFailed {
                application: application.into(),
            });
            e
        })?;
        self.values.insert(application.into(), values);
        self.emit(ConfigEvent::Reloaded {
            application: application.into(),
        });
        Ok(())
    }
}

fn encode_json(values: &BTreeMap<String, Value>) -> String {
    let entries = values
        .iter()
        .map(|(key, value)| format!("\"{}\":{}", escape(key), encode_value(value)))
        .collect::<Vec<_>>()
        .join(",");
    format!("{{{entries}}}")
}
fn encode_value(value: &Value) -> String {
    match value {
        Value::String(v) => format!("\"{}\"", escape(v)),
        Value::Number(v) => v.to_string(),
        Value::Integer(v) => v.to_string(),
        Value::Boolean(v) => v.to_string(),
    }
}
fn escape(value: &str) -> String {
    value
        .replace('\\', "\\\\")
        .replace('"', "\\\"")
        .replace('\n', "\\n")
}
fn decode_json(input: &str) -> Result<BTreeMap<String, Value>, ConfigError> {
    let trimmed = input.trim();
    if !trimmed.starts_with('{') || !trimmed.ends_with('}') {
        return Err(ConfigError::Parse("expected JSON object".into()));
    }
    let mut result = BTreeMap::new();
    let body = &trimmed[1..trimmed.len() - 1];
    if body.trim().is_empty() {
        return Ok(result);
    }
    for item in body.split(',') {
        let (key, raw) = item
            .split_once(':')
            .ok_or_else(|| ConfigError::Parse("expected key/value".into()))?;
        let key = key.trim().trim_matches('"').to_string();
        let raw = raw.trim();
        let value = if raw.starts_with('"') && raw.ends_with('"') {
            Value::String(
                raw[1..raw.len() - 1]
                    .replace("\\\"", "\"")
                    .replace("\\n", "\n")
                    .replace("\\\\", "\\"),
            )
        } else if raw == "true" || raw == "false" {
            Value::Boolean(raw == "true")
        } else if raw.contains('.') {
            Value::Number(raw.parse().map_err(|_| ConfigError::Parse(raw.into()))?)
        } else {
            Value::Integer(raw.parse().map_err(|_| ConfigError::Parse(raw.into()))?)
        };
        result.insert(key, value);
    }
    Ok(result)
}

impl Config {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn set(&mut self, key: impl Into<String>, value: impl Into<String>) {
        self.values.insert(key.into(), value.into());
    }

    pub fn get(&self, key: &str) -> Option<&str> {
        self.values.get(key).map(String::as_str)
    }

    pub fn contains(&self, key: &str) -> bool {
        self.values.contains_key(key)
    }
}

#[cfg(test)]
mod tests {
    use super::Config;

    #[test]
    fn values_are_owned_and_retrievable() {
        let mut config = Config::new();
        config.set("web.port", "8080");
        assert_eq!(config.get("web.port"), Some("8080"));
        assert!(config.contains("web.port"));
        assert_eq!(config.get("missing"), None);
    }
}

#[cfg(test)]
mod dynamic_tests {
    use super::*;

    fn schema() -> Schema {
        Schema {
            application: "locker".into(),
            version: "1".into(),
            fields: vec![
                Field {
                    id: "url".into(),
                    field_type: FieldType::Url,
                    required: true,
                    default: None,
                },
                Field {
                    id: "port".into(),
                    field_type: FieldType::Port,
                    required: false,
                    default: Some(Value::Integer(8080)),
                },
                Field {
                    id: "enabled".into(),
                    field_type: FieldType::Boolean,
                    required: false,
                    default: Some(Value::Boolean(true)),
                },
            ],
        }
    }

    #[test]
    fn schema_defaults_validate_and_isolate_applications() {
        let mut manager = ApplicationConfigManager::default();
        manager.declare(schema()).unwrap();
        assert_eq!(
            manager.get("locker").unwrap().get("port"),
            Some(&Value::Integer(8080))
        );
        let mut values = BTreeMap::new();
        values.insert("url".into(), Value::String("https://server".into()));
        manager.set("locker", values).unwrap();
        assert!(manager.get("other").is_none());
        let mut invalid = BTreeMap::new();
        invalid.insert("url".into(), Value::String("not-a-url".into()));
        assert!(matches!(
            manager.set("locker", invalid),
            Err(ConfigError::WrongType(_))
        ));
    }

    #[test]
    fn json_round_trip_and_events_work() {
        let mut manager = ApplicationConfigManager::default();
        let receiver = manager.subscribe();
        manager.declare(schema()).unwrap();
        let mut values = BTreeMap::new();
        values.insert("url".into(), Value::String("https://server".into()));
        manager.set("locker", values).unwrap();
        let path =
            std::env::temp_dir().join(format!("emaster-config-phase-{}.json", std::process::id()));
        manager.save_json("locker", &path).unwrap();
        manager.reload_json("locker", &path).unwrap();
        assert_eq!(
            manager.get("locker").unwrap().get("url"),
            Some(&Value::String("https://server".into()))
        );
        assert!(matches!(
            receiver.try_iter().last(),
            Some(ConfigEvent::Reloaded { .. })
        ));
        let _ = std::fs::remove_file(path);
    }
}
