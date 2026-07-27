// Compatibility smoke test for the Alex endpoint on older simulator builds.
//
// There are no #delays, initial event controls, or cocotb/DPI calls here.
// The C++ harness owns the clock and advances one rising edge per iteration.
module alex_verilator_compat_tb (
    input logic clk,
    input logic rst
);
    localparam int DATA_W = 256;
    localparam int STRB_W = DATA_W/32;

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
    wire msix_valid;
    wire [10:0] msix_vector;
    wire axil_doorbell_pulse;
    wire [31:0] axil_doorbell_value;
    logic tx_cpl_tlp_ready = 1'b1;
    logic doorbell_seen = 1'b0;

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

    localparam [3:0]
        ST_CAP_SEND = 0, ST_CAP_HANDSHAKE = 1, ST_CAP_WAIT = 2,
        ST_CC_SEND = 3, ST_CC_HANDSHAKE = 4, ST_CC_WAIT = 5,
        ST_CC_READ_SEND = 6, ST_CC_READ_HANDSHAKE = 7, ST_CC_READ_WAIT = 8,
        ST_DB_SEND = 9, ST_DB_HANDSHAKE = 10, ST_DB_WAIT = 11,
        ST_DB_CHECK = 12, ST_CSTS_SEND = 13, ST_CSTS_HANDSHAKE = 14,
        ST_CSTS_WAIT = 15;

    logic [3:0] state = ST_CAP_SEND;
    logic [7:0] timeout_count = 0;

    function automatic [3:0] byte_enable(input [1:0] byte_offset);
        begin
            byte_enable = 4'b1111 << byte_offset;
        end
    endfunction

    function automatic [127:0] make_tlp(input bit is_write,
                                         input [31:0] address,
                                         input [7:0] tag,
                                         input [31:0] data);
        reg [127:0] h;
        begin
            h = '0;
            h[127:125] = is_write ? 3'b010 : 3'b000;
            h[105:96] = 10'd1;
            h[95:80] = 16'h0000;
            h[79:72] = tag;
            h[71:68] = byte_enable(address[1:0]);
            h[67:64] = byte_enable(address[1:0]);
            h[63:34] = address[31:2];
            make_tlp = h;
        end
    endfunction

    task automatic prepare_tlp(input bit is_write, input [31:0] address,
                               input [7:0] tag, input [31:0] data);
        begin
            rx_req_tlp_hdr <= make_tlp(is_write, address, tag, data);
            rx_req_tlp_data <= '0;
            rx_req_tlp_data[31:0] <= data;
            rx_req_tlp_valid <= 1'b1;
            rx_req_tlp_sop <= 1'b1;
            rx_req_tlp_eop <= 1'b1;
            timeout_count <= 0;
        end
    endtask

    task automatic fail(input [127:0] message);
        begin
            $display("[VERILATOR] FAIL: %0s", message);
            $fatal(1);
        end
    endtask

    always @(posedge clk) begin
        if (axil_doorbell_pulse)
            doorbell_seen <= 1'b1;

        if (rst) begin
            state <= ST_CAP_SEND;
            rx_req_tlp_valid <= 1'b0;
            rx_req_tlp_sop <= 1'b0;
            rx_req_tlp_eop <= 1'b0;
            timeout_count <= 0;
            doorbell_seen <= 1'b0;
        end else begin
            case (state)
                ST_CAP_SEND: begin
                    prepare_tlp(1'b0, 32'h0000, 8'd1, 0);
                    state <= ST_CAP_HANDSHAKE;
                end
                ST_CAP_HANDSHAKE: if (rx_req_tlp_ready) begin
                    rx_req_tlp_valid <= 1'b0;
                    rx_req_tlp_sop <= 1'b0;
                    rx_req_tlp_eop <= 1'b0;
                    state <= ST_CAP_WAIT;
                end
                ST_CAP_WAIT: if (tx_cpl_tlp_valid) begin
                    if (tx_cpl_tlp_data[31:0] != 32'h1) fail("CAP read");
                    state <= ST_CC_SEND;
                end else if (timeout_count == 8'd100) fail("CAP timeout");
                ST_CC_SEND: begin
                    prepare_tlp(1'b1, 32'h0014, 8'd2, 32'hdeadbeef);
                    state <= ST_CC_HANDSHAKE;
                end
                ST_CC_HANDSHAKE: if (rx_req_tlp_ready) begin
                    rx_req_tlp_valid <= 1'b0;
                    rx_req_tlp_sop <= 1'b0;
                    rx_req_tlp_eop <= 1'b0;
                    state <= ST_CC_WAIT;
                end
                ST_CC_WAIT: if (dut.axil_bvalid && dut.axil_bready) begin
                    if (dut.axil_bresp != 2'b00) fail("CC write");
                    state <= ST_CC_READ_SEND;
                end else if (timeout_count == 8'd100) fail("CC write timeout");
                ST_CC_READ_SEND: begin
                    prepare_tlp(1'b0, 32'h0014, 8'd3, 0);
                    state <= ST_CC_READ_HANDSHAKE;
                end
                ST_CC_READ_HANDSHAKE: if (rx_req_tlp_ready) begin
                    rx_req_tlp_valid <= 1'b0;
                    rx_req_tlp_sop <= 1'b0;
                    rx_req_tlp_eop <= 1'b0;
                    state <= ST_CC_READ_WAIT;
                end
                ST_CC_READ_WAIT: if (tx_cpl_tlp_valid) begin
                    if (tx_cpl_tlp_data[31:0] != 32'hdeadbeef) fail("CC readback");
                    state <= ST_DB_SEND;
                end else if (timeout_count == 8'd100) fail("CC read timeout");
                ST_DB_SEND: begin
                    prepare_tlp(1'b1, 32'h1000, 8'd4, 32'h1);
                    state <= ST_DB_HANDSHAKE;
                end
                ST_DB_HANDSHAKE: if (rx_req_tlp_ready) begin
                    rx_req_tlp_valid <= 1'b0;
                    rx_req_tlp_sop <= 1'b0;
                    rx_req_tlp_eop <= 1'b0;
                    state <= ST_DB_WAIT;
                end
                ST_DB_WAIT: if (dut.axil_bvalid && dut.axil_bready) begin
                    if (dut.axil_bresp != 2'b00) fail("doorbell write");
                    state <= ST_DB_CHECK;
                end else if (timeout_count == 8'd100) fail("doorbell timeout");
                ST_DB_CHECK: begin
                    if (!doorbell_seen || axil_doorbell_value != 32'h1)
                        fail("doorbell pulse");
                    state <= ST_CSTS_SEND;
                end
                ST_CSTS_SEND: begin
                    prepare_tlp(1'b0, 32'h001c, 8'd5, 0);
                    state <= ST_CSTS_HANDSHAKE;
                end
                ST_CSTS_HANDSHAKE: if (rx_req_tlp_ready) begin
                    rx_req_tlp_valid <= 1'b0;
                    rx_req_tlp_sop <= 1'b0;
                    rx_req_tlp_eop <= 1'b0;
                    state <= ST_CSTS_WAIT;
                end
                ST_CSTS_WAIT: if (tx_cpl_tlp_valid) begin
                    if (tx_cpl_tlp_data[31:0] != 32'h1) fail("CSTS read");
                    $display("=====================================");
                    $display(" VERILATOR ALEX AXI/TLP PASS");
                    $display("=====================================");
                    $finish;
                end else if (timeout_count == 8'd100) fail("CSTS timeout");
                default: fail("invalid state");
            endcase

            if (state != ST_CAP_SEND && state != ST_CC_SEND &&
                state != ST_CC_READ_SEND && state != ST_DB_SEND &&
                state != ST_CSTS_SEND && timeout_count != 8'hff)
                timeout_count <= timeout_count + 1'b1;
        end
    end
endmodule
