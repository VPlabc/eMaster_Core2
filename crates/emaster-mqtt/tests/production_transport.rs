#![cfg(feature = "production-transport")]

use emaster_mqtt::{MqttConfig, MqttTransport, RumqttTransport};
use std::time::Duration;

#[test]
#[ignore = "requires a reachable MQTT broker"]
fn connects_to_configured_mqtt_broker() {
    let host = std::env::var("EMASTER_MQTT_HOST").expect("set EMASTER_MQTT_HOST");
    let transport = RumqttTransport::new();
    let config = MqttConfig {
        name: "integration".into(),
        owner: "tests".into(),
        host,
        port: 1883,
        client_id: "emaster-integration".into(),
        timeout_ms: 5000,
        max_retries: 1,
    };
    transport.connect(&config, Duration::from_secs(5)).unwrap();
    transport.disconnect(&config).unwrap();
}
