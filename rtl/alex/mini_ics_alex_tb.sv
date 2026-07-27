// Live QEMU -> Mini-ICS -> generic PCIe TLP -> Alex AXI-Lite endpoint.
//
// This top is intentionally transaction-serial: Mini-ICS host messages are
// converted into one or two one-DW Memory TLPs and driven into
// pcie_axil_master_minimal.  It is the Questa bring-up path that makes the
// QEMU-originated AXI-Lite activity visible in waves.  The DMA sequence is
// kept deterministic until dma_if_pcie is connected in the next stage.
`timescale 1ns/1ps

module mini_ics_alex_tb;
    import mini_ics_dpi_pkg::*;

    localparam int DATA_W = 256;
    localparam int STRB_W = DATA_W/32;
    localparam int MSG_MMIO_READ_REQ  = 3;
    localparam int MSG_MMIO_WRITE_REQ = 5;
    localparam int MSG_RESET_REQ      = 12;
    localparam int MSG_SHUTDOWN       = 15;
    localparam int MSG_MMIO_READ_RSP  = 4;
    localparam int MSG_MMIO_WRITE_RSP = 6;
    localparam int MSG_RESET_RSP      = 13;
    localparam int MSG_DMA_READ_REQ   = 7;
    localparam int MSG_DMA_WRITE_REQ  = 9;
    localparam int MSG_MSIX           = 11;
    localparam int STATUS_SUCCESS     = 0;
    localparam int STATUS_BAD_LENGTH  = 4;

    logic clk = 1'b0;
    logic rst = 1'b1;
    always #5 clk = ~clk;

    logic [DATA_W-1:0] rx_req_tlp_data = '0;
    logic [127:0] rx_req_tlp_hdr = '0;
    logic rx_req_tlp_valid = 1'b0;
    logic rx_req_tlp_sop = 1'b0;
    logic rx_req_tlp_eop = 1'b0;
    wire rx_req_tlp_ready;
    wire [DATA_W-1:0] tx_cpl_tlp_data;
    wire [STRB_W-1:0] tx_cpl_tlp_strb;
    wire [127:0] tx_cpl_tlp_hdr;
    wire tx_cpl_tlp_valid;
    wire tx_cpl_tlp_sop;
    wire tx_cpl_tlp_eop;
    logic tx_cpl_tlp_ready = 1'b1;
    wire msix_valid;
    wire [10:0] msix_vector;
    wire axil_doorbell_pulse;
    wire [31:0] axil_doorbell_value;

    pcie_vip_alex_endpoint #(
        .TLP_DATA_WIDTH(DATA_W), .TLP_STRB_WIDTH(STRB_W), .TLP_HDR_WIDTH(128)
    ) dut (
        .clk(clk), .rst(rst),
        .rx_req_tlp_data(rx_req_tlp_data), .rx_req_tlp_hdr(rx_req_tlp_hdr),
        .rx_req_tlp_valid(rx_req_tlp_valid), .rx_req_tlp_sop(rx_req_tlp_sop),
        .rx_req_tlp_eop(rx_req_tlp_eop), .rx_req_tlp_ready(rx_req_tlp_ready),
        .tx_cpl_tlp_data(tx_cpl_tlp_data), .tx_cpl_tlp_strb(tx_cpl_tlp_strb),
        .tx_cpl_tlp_hdr(tx_cpl_tlp_hdr), .tx_cpl_tlp_valid(tx_cpl_tlp_valid),
        .tx_cpl_tlp_sop(tx_cpl_tlp_sop), .tx_cpl_tlp_eop(tx_cpl_tlp_eop),
        .tx_cpl_tlp_ready(tx_cpl_tlp_ready), .msix_valid(msix_valid),
        .msix_vector(msix_vector), .axil_doorbell_pulse(axil_doorbell_pulse),
        .axil_doorbell_value(axil_doorbell_value)
    );

    string socket_path;
    logic sim_pass = 1'b0;
    logic sim_done = 1'b0;
    longint unsigned asq_addr = 0;
    longint unsigned acq_addr = 0;
    logic shutdown_seen = 1'b0;

    initial begin
        if (!$value$plusargs("MINI_ICS_SOCKET=%s", socket_path))
            socket_path = "/tmp/pcie-vip-questa.sock";
        repeat (4) @(posedge clk);
        rst = 1'b0;
        if (mini_ics_init(socket_path) != 0)
            $fatal(1, "mini_ics_init failed for %s", socket_path);
        $display("[ALEX][%0t ns] Mini-ICS listener ready on %s", $time, socket_path);
        while (!shutdown_seen) begin
            @(posedge clk);
            poll_one();
        end
        repeat (5) @(posedge clk);
        mini_ics_shutdown();
        sim_done = 1'b1;
    end

    function automatic [3:0] first_be(input longint unsigned address,
                                      input int unsigned length);
        integer i;
        begin
            first_be = 4'b0;
            for (i = 0; i < 4; i = i + 1)
                if (i >= address[1:0] && i < address[1:0] + length)
                    first_be[i] = 1'b1;
        end
    endfunction

    function automatic [127:0] make_mem_tlp(
        input bit is_write, input longint unsigned address,
        input int unsigned length, input int unsigned tag,
        input int unsigned requester_id, input [31:0] data);
        reg [127:0] h;
        begin
            h = '0;
            h[127:125] = is_write ? 3'b010 : 3'b000;
            h[124:120] = 5'b00000;
            h[105:96]  = 10'd1;
            h[95:80]   = requester_id[15:0];
            h[79:72]   = tag[7:0];
            h[71:68]   = first_be(address, length);
            h[67:64]   = first_be(address, length);
            // 3-DW Memory TLP address: bits 63:34 hold address[31:2].
            // Bits 33:32 are the header PH field, not address bits.
            h[63:34]   = address[31:2];
            h[33:32]   = 2'b00;
            make_mem_tlp = h;
        end
    endfunction

    task automatic drive_tlp(input bit is_write, input longint unsigned address,
                             input int unsigned length, input int unsigned tag,
                             input [31:0] data, output [31:0] completion_data,
                             output int unsigned completion_status);
        begin
            completion_data = 32'd0;
            completion_status = STATUS_SUCCESS;
            rx_req_tlp_hdr   <= make_mem_tlp(is_write, address, length, tag, 0, data);
            rx_req_tlp_data  <= '0;
            rx_req_tlp_data[31:0] <= data;
            rx_req_tlp_valid <= 1'b1;
            rx_req_tlp_sop   <= 1'b1;
            rx_req_tlp_eop   <= 1'b1;
            do @(posedge clk); while (!rx_req_tlp_ready);
            rx_req_tlp_valid <= 1'b0;
            rx_req_tlp_sop   <= 1'b0;
            rx_req_tlp_eop   <= 1'b0;
            if (is_write) begin
                // PCIe Memory Writes are posted and do not produce a PCIe
                // completion TLP.  The AXI-Lite write response is the
                // completion point for this BFM transaction.
                do @(posedge clk); while (!(dut.axil_bvalid && dut.axil_bready));
                if (dut.axil_bresp != 2'b00)
                    completion_status = 1;
            end else begin
                do @(posedge clk); while (!tx_cpl_tlp_valid);
                completion_data = tx_cpl_tlp_data[31:0];
                // Completion status is bits 47:45 in Alex's completion header.
                if (tx_cpl_tlp_hdr[47:45] != 3'b000)
                    completion_status = 1;
            end
            @(posedge clk);
        end
    endtask

    task automatic handle_mmio_read(input longint unsigned txn_id,
                                     input longint unsigned address,
                                     input int unsigned length);
        byte unsigned response[];
        int unsigned offset, chunk, status, idx;
        reg [31:0] value;
        begin
            if (length == 0 || length > 8 || (length != 1 && length != 2 && length != 4 && length != 8)) begin
                response = new[0];
                void'(mini_ics_send_response(MSG_MMIO_READ_RSP, txn_id, address,
                                              STATUS_BAD_LENGTH, response, 0));
                return;
            end
            response = new[length];
            $display("[ALEX][%0t ns][TXN %0d] MMIO_READ addr=0x%08h len=%0d", $time, txn_id, address, length);
            for (idx = 0; idx < length; idx = idx + 1) response[idx] = 0;
            for (offset = 0; offset < length; offset += chunk) begin
                chunk = ((length - offset) >= 4) ? 4 : (length - offset);
                drive_tlp(1'b0, address + offset, chunk, offset/4 + 1,
                          32'd0, value, status);
                if (status != STATUS_SUCCESS) begin
                    response = new[0];
                    void'(mini_ics_send_response(MSG_MMIO_READ_RSP, txn_id, address,
                                                  status, response, 0));
                    return;
                end
                for (idx = 0; idx < chunk; idx = idx + 1)
                    response[offset + idx] = value[idx*8 +: 8];
            end
            void'(mini_ics_send_response(MSG_MMIO_READ_RSP, txn_id, address,
                                         STATUS_SUCCESS, response, length));
        end
    endtask

    task automatic handle_mmio_write(input longint unsigned txn_id,
                                     input longint unsigned address,
                                     input int unsigned length);
        byte unsigned payload[];
        int unsigned idx, offset, chunk, status;
        reg [31:0] value;
        byte unsigned empty[];
        begin
            if (length == 0 || length > 8 || (length != 1 && length != 2 && length != 4 && length != 8)) begin
                empty = new[0];
                void'(mini_ics_send_response(MSG_MMIO_WRITE_RSP, txn_id, address,
                                              STATUS_BAD_LENGTH, empty, 0));
                return;
            end
            payload = new[length];
            $display("[ALEX][%0t ns][TXN %0d] MMIO_WRITE addr=0x%08h len=%0d", $time, txn_id, address, length);
            for (idx = 0; idx < length; idx = idx + 1)
                void'(mini_ics_get_payload_byte(idx, payload[idx]));
            for (offset = 0; offset < length; offset += chunk) begin
                chunk = ((length - offset) >= 4) ? 4 : (length - offset);
                value = 32'd0;
                for (idx = 0; idx < chunk; idx = idx + 1)
                    value[idx*8 +: 8] = payload[offset + idx];
                drive_tlp(1'b1, address + offset, chunk, offset/4 + 1,
                          value, value, status);
                if (status != STATUS_SUCCESS) begin
                    empty = new[0];
                    void'(mini_ics_send_response(MSG_MMIO_WRITE_RSP, txn_id, address,
                                                  status, empty, 0));
                    return;
                end
            end
            if (address == 64'h28 && length == 8) begin
                asq_addr = 0;
                for (idx = 0; idx < 8; idx = idx + 1)
                    asq_addr = asq_addr | (64'(payload[idx]) << (8*idx));
            end else if (address == 64'h28 && length == 4) begin
                asq_addr[31:0] = {payload[3], payload[2], payload[1], payload[0]};
            end else if (address == 64'h2c && length == 4) begin
                asq_addr[63:32] = {payload[3], payload[2], payload[1], payload[0]};
            end
            if (address == 64'h30 && length == 8) begin
                acq_addr = 0;
                for (idx = 0; idx < 8; idx = idx + 1)
                    acq_addr = acq_addr | (64'(payload[idx]) << (8*idx));
            end else if (address == 64'h30 && length == 4) begin
                acq_addr[31:0] = {payload[3], payload[2], payload[1], payload[0]};
            end else if (address == 64'h34 && length == 4) begin
                acq_addr[63:32] = {payload[3], payload[2], payload[1], payload[0]};
            end
            empty = new[0];
            void'(mini_ics_send_response(MSG_MMIO_WRITE_RSP, txn_id, address,
                                         STATUS_SUCCESS, empty, 0));
            $display("[ALEX][%0t ns][TXN %0d] MMIO_WRITE_RSP status=SUCCESS", $time, txn_id);
            if (address == 64'h1000) run_deterministic_dma();
        end
    endtask

    task automatic run_deterministic_dma();
        byte unsigned empty[];
        byte unsigned completion[];
        longint unsigned txn;
        int unsigned status, idx;
        begin
            empty = new[0];
            void'(mini_ics_send_request(MSG_DMA_READ_REQ, txn, asq_addr, empty,
                                         64, status));
            completion = new[16];
            for (idx = 0; idx < 16; idx = idx + 1) completion[idx] = 8'hc0 + idx;
            void'(mini_ics_send_request(MSG_DMA_WRITE_REQ, txn, acq_addr, completion,
                                         16, status));
            void'(mini_ics_send_request(MSG_MSIX, txn, 0, empty, 0, status));
            sim_pass = 1'b1;
        end
    endtask

    task automatic poll_one();
        int unsigned msg_type, length, status;
        longint unsigned txn_id, address;
        begin
            if (mini_ics_poll(msg_type, txn_id, address, length, status) <= 0)
                return;
            case (msg_type)
                MSG_MMIO_READ_REQ:  handle_mmio_read(txn_id, address, length);
                MSG_MMIO_WRITE_REQ: handle_mmio_write(txn_id, address, length);
                MSG_RESET_REQ: begin
                    byte unsigned empty[];
                    empty = new[0];
                    void'(mini_ics_send_response(MSG_RESET_RSP, txn_id, 0,
                                                  STATUS_SUCCESS, empty, 0));
                end
                MSG_SHUTDOWN: shutdown_seen = 1'b1;
                default: ;
            endcase
        end
    endtask
endmodule
