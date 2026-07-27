// DPI-C import declarations for the Mini ICS transaction server.
//
// These signatures mirror dpi/mini_ics_dpi.hpp exactly. Two deliberate
// deviations from the spec's suggested API (both explained in
// docs/architecture.md, "DPI boundary" section):
//
//   1. mini_ics_send_response/mini_ics_send_request pass an explicit
//      `length` alongside the SV dynamic array `payload[]`. DPI-C can
//      infer array bounds from the open array itself via $size(), but
//      passing length explicitly keeps the C++ side simple (a
//      straightforward bounds check against the argument rather than a
//      call into svSize()) and matches the signatures given in the
//      prompt.
//   2. An additional mini_ics_get_response_payload_byte() function was
//      added beyond the four spec'd payload/response functions. The
//      spec's mini_ics_poll()/mini_ics_get_payload_byte() pair only
//      covers host-initiated inbound payloads. mini_ics_send_request()
//      (RTL-initiated DMA reads) also receives a payload back -- the
//      DMA_READ_RSP data -- and reusing the inbound staging buffer for
//      that would risk one call's data being clobbered by an inbound
//      poll happening in between. A second staging buffer plus accessor
//      keeps the two payload directions independent.
package mini_ics_dpi_pkg;

    import "DPI-C" function int mini_ics_init(input string socket_path);

    import "DPI-C" function int mini_ics_poll(
        output int unsigned msg_type,
        output longint unsigned transaction_id,
        output longint unsigned address,
        output int unsigned length,
        output int unsigned status
    );

    import "DPI-C" function int mini_ics_get_payload_byte(
        input int unsigned index,
        output byte unsigned value
    );

    import "DPI-C" function int mini_ics_send_response(
        input int unsigned msg_type,
        input longint unsigned transaction_id,
        input longint unsigned address,
        input int unsigned status,
        input byte unsigned payload[],
        input int unsigned length
    );

    import "DPI-C" function int mini_ics_send_request(
        input int unsigned msg_type,
        output longint unsigned transaction_id,
        input longint unsigned address,
        input byte unsigned payload[],
        input int unsigned length,
        output int unsigned out_status
    );

    import "DPI-C" function int mini_ics_get_response_payload_byte(
        input int unsigned index,
        output byte unsigned value
    );

    import "DPI-C" function void mini_ics_shutdown();

endpackage : mini_ics_dpi_pkg
