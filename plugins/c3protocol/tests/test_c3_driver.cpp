#include "c3_driver.h"

#include "hsf/mock_transport.hpp"
#include "hsf/zk_controller/C3Codec.h"
#include "hsf/testing.hpp"

namespace {
std::vector<uint8_t> OkReply() {
  std::vector<uint8_t> body = {0x01, hsf::c3::kReplyOk, 0x00, 0x00};
  const uint16_t crc = hsf::c3::Crc16(body);
  return {0xAA, body[0], body[1], body[2], body[3],
          static_cast<uint8_t>(crc & 0xff), static_cast<uint8_t>(crc >> 8), 0x55};
}
}

HSF_TEST("c3 protocol: fragmented sessionless health response") {
  hsf::MockTransport mock;
  mock.SetResponder([](const hsf::MockTransport::Bytes&) { return OkReply(); }, 2);
  hsf_c3protocol::C3Driver driver;
  HSFConfigRef config{};
  HSFStatus status = driver.Initialize(config, mock.Ref());
  HSF_CHECK_EQ(status, HSF_OK);
  HSF_CHECK_EQ(driver.Start(), HSF_OK);
  HSFDriverStatus driver_status{};
  driver.Status(&driver_status);
  HSF_CHECK(driver_status.connected != 0);
  HSF_CHECK_EQ(mock.WriteCount(), static_cast<size_t>(1));
  driver.Stop();
}

HSF_TEST("c3 protocol: rejects malformed reply") {
  hsf::MockTransport mock;
  mock.SetResponder([](const hsf::MockTransport::Bytes&) { return hsf::MockTransport::Bytes{0x00}; });
  hsf_c3protocol::C3Driver driver;
  HSF_CHECK_EQ(driver.Initialize(HSFConfigRef{}, mock.Ref()), HSF_OK);
  HSF_CHECK(driver.Start() < 0);
}

HSF_TEST_MAIN();
