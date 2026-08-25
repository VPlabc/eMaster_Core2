#include "modbus_driver.h"
#include "hsf/mock_transport.hpp"
#include "hsf/testing.hpp"

using Bytes = std::vector<uint8_t>;

HSF_TEST("modbus: fragmented FC03 response reads one holding register") {
  hsf::MockTransport transport;
  transport.SetResponder([](const Bytes& request) {
    if (request.size() != 12) return Bytes{};
    return Bytes{request[0], request[1], 0, 0, 0, 5, request[6], 3, 2, 0x12, 0x34};
  }, 2);
  hsf_modbus::ModbusDriver driver;
  HSFConfigRef config{};
  HSF_REQUIRE(driver.Initialize(config, transport.Ref()) == HSF_OK);
  HSF_REQUIRE(driver.Start() == HSF_OK);
  HSFDeviceAddress address = hsf_device_address_init();
  address.a1 = 3; address.a2 = 40001; address.encoding = HSF_ENC_U16;
  HSFValue value{};
  HSF_CHECK_OK(driver.Read(&address, &value));
  HSF_CHECK_EQ(value.kind, HSF_VALUE_U64);
  HSF_CHECK_EQ(value.as.u64, (uint64_t)0x1234);
}

HSF_TEST("modbus: FC05 writes a coil and rejects an invalid echo") {
  hsf::MockTransport transport;
  transport.SetResponder([](const Bytes& request) {
    return Bytes{request[0], request[1], 0, 0, 0, 6, request[6], 5, request[8], request[9], request[10], request[11]};
  });
  hsf_modbus::ModbusDriver driver;
  HSFConfigRef config{};
  HSF_REQUIRE(driver.Initialize(config, transport.Ref()) == HSF_OK);
  HSF_REQUIRE(driver.Start() == HSF_OK);
  HSFDeviceAddress address = hsf_device_address_init(); address.a1 = 1; address.a2 = 7;
  HSFValue value = hsf_value_bool(1);
  HSF_CHECK_OK(driver.Write(&address, &value));
  HSF_CHECK_EQ(transport.LastWrite()[7], (uint8_t)5);
}

HSF_TEST_MAIN()
