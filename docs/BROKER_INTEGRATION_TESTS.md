# Broker integration tests

Production MQTT and RabbitMQ adapter tests are feature-gated and ignored by
default because they require external brokers. Run them when environments are
available with:

```text
EMASTER_MQTT_HOST=broker cargo test -p emaster-mqtt --features production-transport --test production_transport -- --ignored
EMASTER_AMQP_HOST=broker cargo test -p emaster-rabbitmq --features production-transport --test production_transport -- --ignored
```

The normal workspace test suite remains deterministic and does not open broker
connections.

CI runs these tests with Mosquitto and RabbitMQ service containers through
`.github/workflows/broker-integration.yml`.
