use emaster_config::Config;
use emaster_core::{Core, CoreState};
use emaster_http::{dispatch, Request, Response, Service};
use emaster_lua::{HostApi, LuaError, Script};
use emaster_modbus::{ModbusClient, ModbusError, ModbusTransport};
use emaster_rabbitmq::{ConnectionState, Message, RabbitConnection};
use emaster_serial::{MemorySerialPort, SerialConfig, SerialPort};

struct MockModbus;
impl ModbusTransport for MockModbus {
    fn transact(&mut self, request: &[u8]) -> Result<Vec<u8>, ModbusError> {
        if request[7] == 3 {
            Ok(vec![0, 1, 0, 0, 0, 5, 1, 3, 2, 0x12, 0x34])
        } else {
            Err(ModbusError::Transport)
        }
    }
}

struct HealthService;
impl Service for HealthService {
    fn handle(&mut self, _: &Request) -> Response {
        Response {
            status: 200,
            body: b"ok".to_vec(),
        }
    }
}

struct Host;
impl HostApi for Host {
    fn call(&mut self, module: &str, function: &str, argument: &str) -> Result<String, LuaError> {
        Ok(format!("{module}.{function}:{argument}"))
    }
}

#[test]
fn core_services_work_through_their_boundaries() {
    let mut core = Core::new(Config::new());
    let events = core.events().subscribe();
    core.start().unwrap();
    assert_eq!(core.state(), CoreState::Running);
    assert_eq!(events.recv().unwrap().topic, "system.started");

    let mut modbus = ModbusClient {
        transport: MockModbus,
        unit_id: 1,
    };
    assert_eq!(modbus.read_holding_registers(0, 1).unwrap(), vec![0x1234]);

    let mut serial = MemorySerialPort::default();
    serial
        .open(SerialConfig {
            path: "loopback".into(),
            baud_rate: 115200,
            timeout_ms: 100,
        })
        .unwrap();
    serial.write(b"command").unwrap();
    assert_eq!(serial.transmitted(), b"command");

    let receiver = core.events().subscribe();
    let mut rabbit = RabbitConnection::default();
    assert_eq!(rabbit.state(), ConnectionState::Disconnected);
    rabbit.connect();
    assert_eq!(
        rabbit.publish(
            core.events(),
            Message {
                routing_key: "device".into(),
                body: b"online".to_vec()
            }
        ),
        Ok(2)
    );
    assert_eq!(receiver.recv().unwrap().topic, "rabbitmq.device");

    let request = Request {
        method: "GET".into(),
        path: "/health".into(),
        body: vec![],
        api_key: None,
    };
    assert_eq!(
        dispatch(&mut HealthService, &request, 1024, None).status,
        200
    );

    let mut host = Host;
    assert_eq!(
        Script::new(&mut host).invoke("modbus", "read", "0"),
        Ok("modbus.read:0".into())
    );
    core.stop().unwrap();
}
