#![cfg(feature = "production-transport")]

use emaster_rabbitmq::{LapinTransport, RabbitConfig, RabbitTransport};
use std::time::Duration;

#[test]
#[ignore = "requires a reachable RabbitMQ broker"]
fn connects_to_configured_rabbitmq_broker() {
    let host = std::env::var("EMASTER_AMQP_HOST").expect("set EMASTER_AMQP_HOST");
    let transport = LapinTransport::new().unwrap();
    let config = RabbitConfig {
        name: "integration".into(),
        owner: "tests".into(),
        host,
        port: 5672,
        username: "guest".into(),
        virtual_host: "/".into(),
        timeout_ms: 5000,
        max_retries: 1,
    };
    transport.connect(&config, Duration::from_secs(5)).unwrap();
    transport.disconnect(&config).unwrap();
}
