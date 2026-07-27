#include "mini_ics/protocol.hpp"
#include "test_harness.hpp"

using namespace mini_ics;
using namespace mini_ics::test;

MINI_ICS_TEST(HeaderRoundTrip) {
    Header h = MakeHeader(MessageType::kMmioWriteReq, /*txn_id=*/42, /*address=*/0x1000,
                           /*byte_enable=*/0xF, StatusCode::kSuccess, /*sim_timestamp=*/12345,
                           /*payload_len=*/4);
    auto bytes = SerializeHeader(h);
    MINI_ICS_CHECK_EQ(bytes.size(), kHeaderSize);

    CodecError err = CodecError::kNone;
    auto parsed = DeserializeHeader(bytes.data(), bytes.size(), &err);
    MINI_ICS_CHECK(parsed.has_value());
    MINI_ICS_CHECK_EQ(err, CodecError::kNone);
    MINI_ICS_CHECK_EQ(parsed->magic, kProtocolMagic);
    MINI_ICS_CHECK_EQ(parsed->version, kProtocolVersion);
    MINI_ICS_CHECK(parsed->Type() == MessageType::kMmioWriteReq);
    MINI_ICS_CHECK_EQ(parsed->txn_id, 42ull);
    MINI_ICS_CHECK_EQ(parsed->address, 0x1000ull);
    MINI_ICS_CHECK_EQ(parsed->byte_enable, 0xFull);
    MINI_ICS_CHECK_EQ(parsed->sim_timestamp, 12345ull);
    MINI_ICS_CHECK_EQ(parsed->payload_len, 4u);
    MINI_ICS_CHECK_EQ(parsed->transfer_len, 4u);
    MINI_ICS_CHECK(parsed->Status() == StatusCode::kSuccess);
}

MINI_ICS_TEST(HeaderEndiannessIsLittleEndian) {
    // txn_id = 0x1122334455667788 should appear byte-reversed at offset
    // 16 in the wire buffer if encoding is little-endian.
    Header h = MakeHeader(MessageType::kHello, 0x1122334455667788ull, 0, 0, StatusCode::kSuccess,
                           0, 0);
    auto bytes = SerializeHeader(h);
    MINI_ICS_CHECK_EQ(bytes[16], 0x88);
    MINI_ICS_CHECK_EQ(bytes[17], 0x77);
    MINI_ICS_CHECK_EQ(bytes[18], 0x66);
    MINI_ICS_CHECK_EQ(bytes[19], 0x55);
    MINI_ICS_CHECK_EQ(bytes[20], 0x44);
    MINI_ICS_CHECK_EQ(bytes[21], 0x33);
    MINI_ICS_CHECK_EQ(bytes[22], 0x22);
    MINI_ICS_CHECK_EQ(bytes[23], 0x11);

    // magic (u32) at offset 0 likewise little-endian.
    MINI_ICS_CHECK_EQ(bytes[0], static_cast<std::uint8_t>(kProtocolMagic & 0xFF));
    MINI_ICS_CHECK_EQ(bytes[3], static_cast<std::uint8_t>((kProtocolMagic >> 24) & 0xFF));
}

MINI_ICS_TEST(TransferLengthUsesVersion2ReservedBytes) {
    Header h = MakeHeader(MessageType::kDmaReadReq, 7, 0x2000, 0,
                          StatusCode::kSuccess, 0, 0, kFlagNone, 0x11223344);
    auto bytes = SerializeHeader(h);
    MINI_ICS_CHECK_EQ(bytes[56], 0x44);
    MINI_ICS_CHECK_EQ(bytes[57], 0x33);
    MINI_ICS_CHECK_EQ(bytes[58], 0x22);
    MINI_ICS_CHECK_EQ(bytes[59], 0x11);

    CodecError err = CodecError::kNone;
    auto parsed = DeserializeHeader(bytes.data(), bytes.size(), &err);
    MINI_ICS_CHECK(parsed.has_value());
    MINI_ICS_CHECK_EQ(parsed->transfer_len, 0x11223344u);
}

MINI_ICS_TEST(RejectsInvalidMagic) {
    Header h = MakeHeader(MessageType::kHello, 1, 0, 0, StatusCode::kSuccess, 0, 0);
    auto bytes = SerializeHeader(h);
    bytes[0] ^= 0xFF;  // corrupt magic

    CodecError err = CodecError::kNone;
    auto parsed = DeserializeHeader(bytes.data(), bytes.size(), &err);
    MINI_ICS_CHECK(!parsed.has_value());
    MINI_ICS_CHECK(err == CodecError::kBadMagic);
}

MINI_ICS_TEST(RejectsUnsupportedVersion) {
    Header h = MakeHeader(MessageType::kHello, 1, 0, 0, StatusCode::kSuccess, 0, 0);
    h.version = kProtocolVersion + 1;
    auto bytes = SerializeHeader(h);

    CodecError err = CodecError::kNone;
    auto parsed = DeserializeHeader(bytes.data(), bytes.size(), &err);
    MINI_ICS_CHECK(!parsed.has_value());
    MINI_ICS_CHECK(err == CodecError::kBadVersion);
}

MINI_ICS_TEST(RejectsTruncatedHeader) {
    Header h = MakeHeader(MessageType::kHello, 1, 0, 0, StatusCode::kSuccess, 0, 0);
    auto bytes = SerializeHeader(h);

    CodecError err = CodecError::kNone;
    auto parsed = DeserializeHeader(bytes.data(), bytes.size() - 10, &err);
    MINI_ICS_CHECK(!parsed.has_value());
    MINI_ICS_CHECK(err == CodecError::kTruncated);
}

MINI_ICS_TEST(RejectsBadHeaderLengthField) {
    Header h = MakeHeader(MessageType::kHello, 1, 0, 0, StatusCode::kSuccess, 0, 0);
    h.header_len = kHeaderSize + 8;  // claims a header size we don't support
    auto bytes = SerializeHeader(h);

    CodecError err = CodecError::kNone;
    auto parsed = DeserializeHeader(bytes.data(), bytes.size(), &err);
    MINI_ICS_CHECK(!parsed.has_value());
    MINI_ICS_CHECK(err == CodecError::kBadHeaderLength);
}

MINI_ICS_TEST(RejectsExcessivePayloadLength) {
    Header h = MakeHeader(MessageType::kMmioWriteReq, 1, 0, 0, StatusCode::kSuccess, 0,
                           kMaxPayloadSize + 1);
    auto bytes = SerializeHeader(h);

    CodecError err = CodecError::kNone;
    auto parsed = DeserializeHeader(bytes.data(), bytes.size(), &err);
    MINI_ICS_CHECK(!parsed.has_value());
    MINI_ICS_CHECK(err == CodecError::kBadPayloadLength);
}

MINI_ICS_TEST(SerializeMessageIncludesPayload) {
    Header h = MakeHeader(MessageType::kMmioWriteReq, 5, 0x40, 0xF, StatusCode::kSuccess, 0, 4);
    std::vector<std::uint8_t> payload = {0xDE, 0xAD, 0xBE, 0xEF};
    auto full = SerializeMessage(h, payload);
    MINI_ICS_CHECK_EQ(full.size(), kHeaderSize + 4);
    MINI_ICS_CHECK_EQ(full[kHeaderSize + 0], 0xDE);
    MINI_ICS_CHECK_EQ(full[kHeaderSize + 3], 0xEF);
}

MINI_ICS_TEST(MessageTypeAndStatusToStringAreStable) {
    MINI_ICS_CHECK_EQ(ToString(MessageType::kMmioReadReq), std::string("MMIO_READ_REQ"));
    MINI_ICS_CHECK_EQ(ToString(MessageType::kMsiX), std::string("MSI_X"));
    MINI_ICS_CHECK_EQ(ToString(StatusCode::kSuccess), std::string("SUCCESS"));
    MINI_ICS_CHECK_EQ(ToString(StatusCode::kErrorTimeout), std::string("ERROR_TIMEOUT"));
}

int main() { return RunAll("ProtocolTest"); }
