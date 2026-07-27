// Transaction-level PCIe/NVMe endpoint model. Polls the Mini ICS DPI
// layer for host-initiated transactions (MMIO read/write, reset,
// shutdown), drives them into nvme_register_model, and -- once the
// submission-queue doorbell is rung -- autonomously runs the
// DMA-read -> DMA-write -> MSI-X sequence described in
// docs/architecture.md by calling out through DPI again.
//
// This is a transaction-level model, not a cycle-accurate PCIe endpoint:
// there is no TLP framing, no PCIe link training, no AXI/AXI-Stream
// interface. See docs/architecture.md ("Why transaction-level, not
// PCIe TLPs") for the reasoning.
import mini_ics_dpi_pkg::*;

module mini_ics_endpoint_model #(
    parameter string SOCKET_PATH = "/tmp/mini_ics.sock"  // used only if socket_path input is empty
) (
    input logic  clk,
    input logic  rst_n,
    input string socket_path,  // runtime-resolved path (e.g. from a +plusarg); "" means use SOCKET_PATH
    output logic sim_done,
    output logic sim_pass
);

    // ------------------------------------------------------------------
    // DPI initialization
    // ------------------------------------------------------------------
    logic  dpi_initialized;
    string resolved_socket_path;

    initial begin
        dpi_initialized = 1'b0;
        sim_done = 1'b0;
        sim_pass = 1'b0;
    end

    task automatic init_dpi();
        int rc;
        resolved_socket_path = (socket_path.len() > 0) ? socket_path : SOCKET_PATH;
        rc = mini_ics_init(resolved_socket_path);
        if (rc != 0) begin
            $display("[RTL ] FATAL: mini_ics_init failed (rc=%0d) for socket %s", rc,
                      resolved_socket_path);
            $fatal(1, "mini_ics_init failed");
        end
        dpi_initialized = 1'b1;
        $display("[RTL ][%0t ns] mini_ics_init: connected on %s", $time, resolved_socket_path);
    endtask

    // ------------------------------------------------------------------
    // Register model instance
    // ------------------------------------------------------------------
    logic        reg_req_valid;
    logic        reg_req_is_write;
    logic [63:0] reg_req_addr;
    logic [63:0] reg_req_wdata;
    logic [7:0]  reg_req_byte_enable;
    logic [3:0]  reg_req_len;
    logic        reg_rsp_valid;
    logic [63:0] reg_rsp_rdata;
    logic        reg_rsp_addr_invalid;
    logic        doorbell_pulse;
    logic [31:0] doorbell_value;
    logic        ctrl_enabled;
    logic [63:0] admin_sq_addr;
    logic [63:0] admin_cq_addr;

    nvme_register_model u_regs (
        .clk              (clk),
        .rst_n            (rst_n),
        .req_valid        (reg_req_valid),
        .req_is_write     (reg_req_is_write),
        .req_addr         (reg_req_addr),
        .req_wdata        (reg_req_wdata),
        .req_byte_enable  (reg_req_byte_enable),
        .req_len          (reg_req_len),
        .rsp_valid        (reg_rsp_valid),
        .rsp_rdata        (reg_rsp_rdata),
        .rsp_addr_invalid (reg_rsp_addr_invalid),
        .doorbell_pulse   (doorbell_pulse),
        .doorbell_value   (doorbell_value),
        .ctrl_enabled     (ctrl_enabled),
        .admin_sq_addr    (admin_sq_addr),
        .admin_cq_addr    (admin_cq_addr)
    );

    // ------------------------------------------------------------------
    // Message type encoding, matching include/mini_ics/protocol.hpp.
    // SystemVerilog has no access to the C++ enum, so these constants
    // are the SV-side source of truth and must be kept numerically
    // identical to mini_ics::MessageType.
    // ------------------------------------------------------------------
    localparam int unsigned MSG_HELLO          = 1;
    localparam int unsigned MSG_HELLO_ACK      = 2;
    localparam int unsigned MSG_MMIO_READ_REQ  = 3;
    localparam int unsigned MSG_MMIO_READ_RSP  = 4;
    localparam int unsigned MSG_MMIO_WRITE_REQ = 5;
    localparam int unsigned MSG_MMIO_WRITE_RSP = 6;
    localparam int unsigned MSG_DMA_READ_REQ   = 7;
    localparam int unsigned MSG_DMA_READ_RSP   = 8;
    localparam int unsigned MSG_DMA_WRITE_REQ  = 9;
    localparam int unsigned MSG_DMA_WRITE_RSP  = 10;
    localparam int unsigned MSG_MSI_X          = 11;
    localparam int unsigned MSG_RESET_REQ      = 12;
    localparam int unsigned MSG_RESET_RSP      = 13;
    localparam int unsigned MSG_ERROR          = 14;
    localparam int unsigned MSG_SHUTDOWN       = 15;

    localparam int unsigned STATUS_SUCCESS             = 0;
    localparam int unsigned STATUS_ERROR_INVALID_ADDR   = 7;
    localparam int unsigned STATUS_ERROR_BAD_LENGTH     = 4;

    localparam int unsigned MSIX_VECTOR                = 0;

    // ------------------------------------------------------------------
    // Poll loop: one attempt per clock edge, matching "poll every clock
    // cycle" from the spec. mini_ics_poll() itself is bounded internally
    // (see dpi/mini_ics_dpi.cpp, kPollTimeoutMs) so this never stalls
    // the simulation.
    // ------------------------------------------------------------------
    logic shutdown_seen;

    initial begin
        shutdown_seen = 1'b0;
        reg_req_valid = 1'b0;
        reg_req_is_write = 1'b0;
        reg_req_addr = '0;
        reg_req_wdata = '0;
        reg_req_byte_enable = '0;
        reg_req_len = '0;

        wait (rst_n === 1'b1);
        init_dpi();

        while (!shutdown_seen) begin
            @(posedge clk);
            poll_and_dispatch();
        end

        // Give in-flight sends a moment to land before tearing down.
        repeat (5) @(posedge clk);
        mini_ics_shutdown();
        $display("[RTL ][%0t ns] mini_ics_shutdown complete", $time);
        sim_done = 1'b1;
    end

    task automatic poll_and_dispatch();
        int unsigned          msg_type;
        longint unsigned      txn_id;
        longint unsigned      addr;
        int unsigned          length;
        int unsigned          status;
        int                   rc;
        byte unsigned         payload[];
        byte unsigned         value;
        int                   i;

        rc = mini_ics_poll(msg_type, txn_id, addr, length, status);
        if (rc <= 0) begin
            return;  // 0 = nothing pending, <0 = no client / not running
        end

        if (length > 0) begin
            payload = new[length];
            for (i = 0; i < length; i++) begin
                void'(mini_ics_get_payload_byte(i, value));
                payload[i] = value;
            end
        end

        case (msg_type)
            MSG_MMIO_WRITE_REQ: handle_mmio_write(txn_id, addr, payload, length);
            MSG_MMIO_READ_REQ:  handle_mmio_read(txn_id, addr, length);
            MSG_RESET_REQ:      handle_reset(txn_id);
            MSG_SHUTDOWN: begin
                $display("[RTL ][%0t ns][TXN %0d] SHUTDOWN received", $time, txn_id);
                shutdown_seen = 1'b1;
            end
            default: begin
                $display("[RTL ][%0t ns][TXN %0d] WARNING: unexpected inbound msg_type=%0d",
                          $time, txn_id, msg_type);
            end
        endcase
    endtask

    task automatic handle_reset(longint unsigned txn_id);
        byte unsigned empty_payload[];
        int rc;
        empty_payload = new[0];
        $display("[RTL ][%0t ns][TXN %0d] RESET applied", $time, txn_id);
        rc = mini_ics_send_response(MSG_RESET_RSP, txn_id, 64'd0, STATUS_SUCCESS, empty_payload, 0);
        if (rc != 0) $display("[RTL ] WARNING: RESET_RSP send failed rc=%0d", rc);
    endtask

    task automatic handle_mmio_write(longint unsigned txn_id, longint unsigned addr,
                                      byte unsigned payload[], int unsigned length);
        logic [63:0] wdata;
        int unsigned status;
        byte unsigned empty_payload[];
        int rc;
        int i;
        string status_str;

        empty_payload = new[0];
        wdata = '0;
        for (i = 0; i < length && i < 8; i++) begin
            wdata[i*8 +: 8] = payload[i];
        end

        reg_req_valid    = 1'b1;
        reg_req_is_write = 1'b1;
        reg_req_addr     = addr;
        reg_req_wdata    = wdata;
        reg_req_len      = length[3:0];
        @(posedge clk);
        reg_req_valid = 1'b0;
        wait (reg_rsp_valid === 1'b1);

        status = reg_rsp_addr_invalid ? STATUS_ERROR_INVALID_ADDR : STATUS_SUCCESS;
        rc = mini_ics_send_response(MSG_MMIO_WRITE_RSP, txn_id, addr, status, empty_payload, 0);
        if (rc != 0) $display("[RTL ] WARNING: MMIO_WRITE_RSP send failed rc=%0d", rc);
        status_str = (status == STATUS_SUCCESS) ? "SUCCESS" : "ERROR_INVALID_ADDRESS";
        $display("[RTL ][%0t ns][TXN %0d] MMIO_WRITE_RSP status=%s", $time, txn_id, status_str);

        if (status == STATUS_SUCCESS && doorbell_pulse) begin
            $display("[RTL ][%0t ns][TXN %0d] SQ0 doorbell rung, tail=%0d", $time, txn_id,
                      doorbell_value);
            run_dma_sequence();
        end
    endtask

    task automatic handle_mmio_read(longint unsigned txn_id, longint unsigned addr,
                                    int unsigned length);
        int unsigned  status;
        byte unsigned rsp_payload[];
        int rc;
        int i;
        string status_str;

        reg_req_valid    = 1'b1;
        reg_req_is_write = 1'b0;
        reg_req_addr     = addr;
        reg_req_len      = length[3:0];
        @(posedge clk);
        reg_req_valid = 1'b0;
        wait (reg_rsp_valid === 1'b1);

        status = reg_rsp_addr_invalid ? STATUS_ERROR_INVALID_ADDR : STATUS_SUCCESS;
        if (status == STATUS_SUCCESS) begin
            rsp_payload = new[length];
            for (i = 0; i < length; i++) rsp_payload[i] = reg_rsp_rdata[i*8 +: 8];
        end else begin
            rsp_payload = new[0];
        end

        rc = mini_ics_send_response(MSG_MMIO_READ_RSP, txn_id, addr, status, rsp_payload,
                                     rsp_payload.size());
        if (rc != 0) $display("[RTL ] WARNING: MMIO_READ_RSP send failed rc=%0d", rc);
        status_str = (status == STATUS_SUCCESS) ? "SUCCESS" : "ERROR_INVALID_ADDRESS";
        $display("[RTL ][%0t ns][TXN %0d] MMIO_READ_RSP status=%s", $time, txn_id, status_str);
    endtask

    // Drives the RTL-initiated DMA read (mock NVMe command fetch), DMA
    // write (mock completion post), and MSI-X once the doorbell rings.
    task automatic run_dma_sequence();
        longint unsigned read_txn_id;
        int unsigned      read_status;
        byte unsigned     empty_payload[];
        byte unsigned     cmd_data[];
        byte unsigned     completion[];
        longint unsigned  write_txn_id;
        int unsigned      write_status;
        longint unsigned  msix_txn_id;
        int unsigned      msix_status;
        int rc;
        int i;
        byte unsigned value;

        empty_payload = new[0];

        rc = mini_ics_send_request(MSG_DMA_READ_REQ, read_txn_id, admin_sq_addr,
                                    empty_payload, 64, read_status);
        $display("[RTL ][%0t ns][TXN %0d] DMA_READ addr=0x%08h len=64", $time, read_txn_id,
                  admin_sq_addr);
        if (rc != 0) begin
            $display("[RTL ] ERROR: DMA read failed rc=%0d status=%0d", rc, read_status);
            return;
        end

        cmd_data = new[64];
        for (i = 0; i < 64; i++) begin
            void'(mini_ics_get_response_payload_byte(i, value));
            cmd_data[i] = value;
        end
        $display("[RTL ][%0t ns][TXN %0d] DMA_READ_RSP len=64", $time, read_txn_id);

        // Build the 16-byte mock completion (0xC0 + i), matching what
        // src/host_client.cpp verifies.
        completion = new[16];
        for (i = 0; i < 16; i++) completion[i] = 8'hC0 + i[7:0];

        rc = mini_ics_send_request(MSG_DMA_WRITE_REQ, write_txn_id, admin_cq_addr,
                                    completion, 16, write_status);
        $display("[RTL ][%0t ns][TXN %0d] DMA_WRITE addr=0x%08h len=16", $time, write_txn_id,
                  admin_cq_addr);
        if (rc != 0) begin
            $display("[RTL ] ERROR: DMA write failed rc=%0d status=%0d", rc, write_status);
            return;
        end

        rc = mini_ics_send_request(MSG_MSI_X, msix_txn_id, MSIX_VECTOR, empty_payload, 0,
                                    msix_status);
        $display("[RTL ][%0t ns][TXN %0d] MSI_X vector=%0d", $time, msix_txn_id, MSIX_VECTOR);

        sim_pass = 1'b1;
    endtask

endmodule : mini_ics_endpoint_model
