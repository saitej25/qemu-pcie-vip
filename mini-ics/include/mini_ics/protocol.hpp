// Mini ICS wire protocol: fixed-size header + variable payload.
//
// Wire encoding is explicit little-endian, byte-by-byte. We never cast a
// C++ struct onto the socket buffer: compilers are free to insert padding,
// reorder bitfields, and pick host endianness, none of which is portable
// or stable across compiler versions. Serialize()/Deserialize() below do
// the packing/unpacking by hand so the on-wire layout is a project
// invariant, not an accident of whatever compiled the two ends.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mini_ics {

// Bumped whenever the header layout or semantics change incompatibly.
constexpr std::uint16_t kProtocolVersion = 2;

// ASCII "MICS" read as a little-endian u32. Chosen to be recognizable in a
// hex dump and vanishingly unlikely to appear at the start of a random or
// unrelated byte stream.
constexpr std::uint32_t kProtocolMagic = 0x5343494Du;

// Header is fixed-size and always sent/received in full before payload.
constexpr std::size_t kHeaderSize = 64;

// Practical upper bound on a single message payload. Keeps a corrupted or
// hostile length field from making us attempt a multi-gigabyte allocation.
constexpr std::uint32_t kMaxPayloadSize = 16u * 1024u * 1024u;

enum class MessageType : std::uint16_t {
    kHello = 1,
    kHelloAck = 2,
    kMmioReadReq = 3,
    kMmioReadRsp = 4,
    kMmioWriteReq = 5,
    kMmioWriteRsp = 6,
    kDmaReadReq = 7,
    kDmaReadRsp = 8,
    kDmaWriteReq = 9,
    kDmaWriteRsp = 10,
    kMsiX = 11,
    kResetReq = 12,
    kResetRsp = 13,
    kError = 14,
    kShutdown = 15,
};

// Human-readable name for logging; returns "UNKNOWN(n)" for unrecognized
// values rather than asserting, since this is called from log paths that
// must not throw on malformed/adversarial input.
std::string ToString(MessageType type);

enum class StatusCode : std::uint32_t {
    kSuccess = 0,
    kErrorGeneric = 1,
    kErrorBadMagic = 2,
    kErrorBadVersion = 3,
    kErrorBadLength = 4,
    kErrorTimeout = 5,
    kErrorOutOfBounds = 6,
    kErrorInvalidAddress = 7,
    kErrorNotConnected = 8,
    kErrorDisconnected = 9,
    kErrorUnknownTransaction = 10,
    kErrorUnsupported = 11,
};

std::string ToString(StatusCode status);

// Bit flags for the header Flags field. Currently only used to mark a
// message as a response vs. request-shaped for logging purposes; kept as
// a bitmask (not derived from MessageType) so future flags (e.g.
// "poisoned", "needs-ack") can be added without changing message types.
enum FlagBits : std::uint16_t {
    kFlagNone = 0,
    kFlagIsResponse = 1u << 0,
};

// In-memory representation of the fixed header. Field order here mirrors
// the wire layout documented in docs/protocol.md.
struct Header {
    std::uint32_t magic = kProtocolMagic;
    std::uint16_t version = kProtocolVersion;
    std::uint16_t msg_type = 0;  // MessageType, kept raw so unknown values deserialize instead of UB
    std::uint16_t flags = kFlagNone;
    std::uint16_t header_len = kHeaderSize;
    std::uint32_t payload_len = 0;
    std::uint64_t txn_id = 0;
    std::uint64_t sim_timestamp = 0;  // simulation-time nanoseconds, 0 on host-only messages
    std::uint64_t address = 0;
    std::uint64_t byte_enable = 0;  // one bit per byte lane, up to 64 bytes; all-ones if not applicable
    std::uint32_t status = 0;       // StatusCode
    std::uint32_t reserved = 0;
    std::uint32_t transfer_len = 0; // requested MMIO/DMA bytes, independent of payload_len
    std::uint32_t reserved2 = 0;

    MessageType Type() const { return static_cast<MessageType>(msg_type); }
    StatusCode Status() const { return static_cast<StatusCode>(status); }
};

static_assert(sizeof(Header) >= kHeaderSize,
              "in-memory Header must be able to hold every wire field");

// A fully decoded message: header plus payload bytes (payload may be empty).
struct Message {
    Header header;
    std::vector<std::uint8_t> payload;
};

enum class CodecError {
    kNone = 0,
    kBadMagic,
    kBadVersion,
    kBadHeaderLength,
    kBadPayloadLength,
    kTruncated,
};

std::string ToString(CodecError error);

// Serializes header fields (NOT including payload) into exactly
// kHeaderSize bytes, little-endian.
std::array<std::uint8_t, kHeaderSize> SerializeHeader(const Header& header);

// Parses kHeaderSize bytes (caller must supply exactly that many) into a
// Header, validating magic/version/header_len/payload_len bounds. On
// failure returns the specific CodecError via `out_error` and the
// returned optional is empty.
std::optional<Header> DeserializeHeader(const std::uint8_t* data, std::size_t len,
                                         CodecError* out_error);

// Serializes a full message (header + payload) into a contiguous buffer
// ready to write to a socket.
std::vector<std::uint8_t> SerializeMessage(const Header& header,
                                            const std::vector<std::uint8_t>& payload);

// Convenience builder that fills in magic/version/header_len/payload_len
// automatically from the given fields and payload.
Header MakeHeader(MessageType type, std::uint64_t txn_id, std::uint64_t address,
                   std::uint64_t byte_enable, StatusCode status, std::uint64_t sim_timestamp,
                   std::uint32_t payload_len, std::uint16_t flags = kFlagNone,
                   std::uint32_t transfer_len = 0);

}  // namespace mini_ics
