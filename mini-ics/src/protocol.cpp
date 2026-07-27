#include "mini_ics/protocol.hpp"

#include <cstring>

namespace mini_ics {

namespace {

void PutU16(std::uint8_t* dst, std::uint16_t v) {
    dst[0] = static_cast<std::uint8_t>(v & 0xFF);
    dst[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
}

void PutU32(std::uint8_t* dst, std::uint32_t v) {
    dst[0] = static_cast<std::uint8_t>(v & 0xFF);
    dst[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
    dst[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
    dst[3] = static_cast<std::uint8_t>((v >> 24) & 0xFF);
}

void PutU64(std::uint8_t* dst, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        dst[i] = static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF);
    }
}

std::uint16_t GetU16(const std::uint8_t* src) {
    return static_cast<std::uint16_t>(src[0]) | (static_cast<std::uint16_t>(src[1]) << 8);
}

std::uint32_t GetU32(const std::uint8_t* src) {
    return static_cast<std::uint32_t>(src[0]) | (static_cast<std::uint32_t>(src[1]) << 8) |
           (static_cast<std::uint32_t>(src[2]) << 16) | (static_cast<std::uint32_t>(src[3]) << 24);
}

std::uint64_t GetU64(const std::uint8_t* src) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(src[i]) << (8 * i);
    }
    return v;
}

}  // namespace

std::string ToString(MessageType type) {
    switch (type) {
        case MessageType::kHello: return "HELLO";
        case MessageType::kHelloAck: return "HELLO_ACK";
        case MessageType::kMmioReadReq: return "MMIO_READ_REQ";
        case MessageType::kMmioReadRsp: return "MMIO_READ_RSP";
        case MessageType::kMmioWriteReq: return "MMIO_WRITE_REQ";
        case MessageType::kMmioWriteRsp: return "MMIO_WRITE_RSP";
        case MessageType::kDmaReadReq: return "DMA_READ_REQ";
        case MessageType::kDmaReadRsp: return "DMA_READ_RSP";
        case MessageType::kDmaWriteReq: return "DMA_WRITE_REQ";
        case MessageType::kDmaWriteRsp: return "DMA_WRITE_RSP";
        case MessageType::kMsiX: return "MSI_X";
        case MessageType::kResetReq: return "RESET_REQ";
        case MessageType::kResetRsp: return "RESET_RSP";
        case MessageType::kError: return "ERROR";
        case MessageType::kShutdown: return "SHUTDOWN";
    }
    return "UNKNOWN(" + std::to_string(static_cast<unsigned>(type)) + ")";
}

std::string ToString(StatusCode status) {
    switch (status) {
        case StatusCode::kSuccess: return "SUCCESS";
        case StatusCode::kErrorGeneric: return "ERROR_GENERIC";
        case StatusCode::kErrorBadMagic: return "ERROR_BAD_MAGIC";
        case StatusCode::kErrorBadVersion: return "ERROR_BAD_VERSION";
        case StatusCode::kErrorBadLength: return "ERROR_BAD_LENGTH";
        case StatusCode::kErrorTimeout: return "ERROR_TIMEOUT";
        case StatusCode::kErrorOutOfBounds: return "ERROR_OUT_OF_BOUNDS";
        case StatusCode::kErrorInvalidAddress: return "ERROR_INVALID_ADDRESS";
        case StatusCode::kErrorNotConnected: return "ERROR_NOT_CONNECTED";
        case StatusCode::kErrorDisconnected: return "ERROR_DISCONNECTED";
        case StatusCode::kErrorUnknownTransaction: return "ERROR_UNKNOWN_TRANSACTION";
        case StatusCode::kErrorUnsupported: return "ERROR_UNSUPPORTED";
    }
    return "UNKNOWN_STATUS(" + std::to_string(static_cast<unsigned>(status)) + ")";
}

std::string ToString(CodecError error) {
    switch (error) {
        case CodecError::kNone: return "NONE";
        case CodecError::kBadMagic: return "BAD_MAGIC";
        case CodecError::kBadVersion: return "BAD_VERSION";
        case CodecError::kBadHeaderLength: return "BAD_HEADER_LENGTH";
        case CodecError::kBadPayloadLength: return "BAD_PAYLOAD_LENGTH";
        case CodecError::kTruncated: return "TRUNCATED";
    }
    return "UNKNOWN_CODEC_ERROR";
}

std::array<std::uint8_t, kHeaderSize> SerializeHeader(const Header& header) {
    std::array<std::uint8_t, kHeaderSize> buf{};
    std::uint8_t* p = buf.data();

    PutU32(p + 0, header.magic);
    PutU16(p + 4, header.version);
    PutU16(p + 6, header.msg_type);
    PutU16(p + 8, header.flags);
    PutU16(p + 10, header.header_len);
    PutU32(p + 12, header.payload_len);
    PutU64(p + 16, header.txn_id);
    PutU64(p + 24, header.sim_timestamp);
    PutU64(p + 32, header.address);
    PutU64(p + 40, header.byte_enable);
    PutU32(p + 48, header.status);
    PutU32(p + 52, header.reserved);
    PutU32(p + 56, header.transfer_len);
    PutU32(p + 60, header.reserved2);

    return buf;
}

std::optional<Header> DeserializeHeader(const std::uint8_t* data, std::size_t len,
                                         CodecError* out_error) {
    auto fail = [&](CodecError e) -> std::optional<Header> {
        if (out_error) *out_error = e;
        return std::nullopt;
    };

    if (len < kHeaderSize) {
        return fail(CodecError::kTruncated);
    }

    Header h{};
    h.magic = GetU32(data + 0);
    h.version = GetU16(data + 4);
    h.msg_type = GetU16(data + 6);
    h.flags = GetU16(data + 8);
    h.header_len = GetU16(data + 10);
    h.payload_len = GetU32(data + 12);
    h.txn_id = GetU64(data + 16);
    h.sim_timestamp = GetU64(data + 24);
    h.address = GetU64(data + 32);
    h.byte_enable = GetU64(data + 40);
    h.status = GetU32(data + 48);
    h.reserved = GetU32(data + 52);
    h.transfer_len = GetU32(data + 56);
    h.reserved2 = GetU32(data + 60);

    if (h.magic != kProtocolMagic) {
        return fail(CodecError::kBadMagic);
    }
    if (h.version != kProtocolVersion) {
        return fail(CodecError::kBadVersion);
    }
    if (h.header_len != kHeaderSize) {
        return fail(CodecError::kBadHeaderLength);
    }
    if (h.payload_len > kMaxPayloadSize) {
        return fail(CodecError::kBadPayloadLength);
    }

    if (out_error) *out_error = CodecError::kNone;
    return h;
}

std::vector<std::uint8_t> SerializeMessage(const Header& header,
                                            const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    out.reserve(kHeaderSize + payload.size());

    auto hdr_bytes = SerializeHeader(header);
    out.insert(out.end(), hdr_bytes.begin(), hdr_bytes.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

Header MakeHeader(MessageType type, std::uint64_t txn_id, std::uint64_t address,
                   std::uint64_t byte_enable, StatusCode status, std::uint64_t sim_timestamp,
                   std::uint32_t payload_len, std::uint16_t flags,
                   std::uint32_t transfer_len) {
    Header h{};
    h.magic = kProtocolMagic;
    h.version = kProtocolVersion;
    h.msg_type = static_cast<std::uint16_t>(type);
    h.flags = flags;
    h.header_len = kHeaderSize;
    h.payload_len = payload_len;
    h.txn_id = txn_id;
    h.sim_timestamp = sim_timestamp;
    h.address = address;
    h.byte_enable = byte_enable;
    h.status = static_cast<std::uint32_t>(status);
    h.reserved = 0;
    if (transfer_len == 0) {
        transfer_len = payload_len;
        if (type == MessageType::kMmioReadReq && payload_len == 0) {
            transfer_len = static_cast<std::uint32_t>(
                __builtin_popcountll(static_cast<unsigned long long>(byte_enable)));
        }
    }
    h.transfer_len = transfer_len;
    h.reserved2 = 0;
    return h;
}

}  // namespace mini_ics
