# Rust RabbitMQ foundation

`emaster-rabbitmq::RabbitManager` adds the third connectivity migration phase
while preserving the original event-bus-oriented `RabbitConnection` API.

Managed connections support:

- independent configuration and application ownership
- create, start, stop, restart, destroy, status, and list
- exchange and queue declaration
- multiple consumers with cancellation
- routed publication and explicit acknowledgement
- restoration of exchanges, queues, and consumers after restart
- validation of connection settings, names, routing keys, and message bodies

`RabbitTransport` is the boundary for a production AMQP client. The default
transport fails closed and performs no network I/O. C++ RabbitMQ behavior stays
active as the compatibility reference until an integration transport and
behavioral tests are available.
