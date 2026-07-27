`timescale 1ns/1ps

// Self-contained Verilator smoke test for the generic Alex endpoint.
//
// This deliberately does not use Mini-ICS DPI or cocotb.  It drives the same
// 128-bit, one-segment Memory Read/Write TLP boundary used by the live QEMU
// path and checks the AXI-Lite register target.  Keeping this test pure SV
// makes it usable with `verilator --binary` on a clean build host.
module alex_verilator_tb;
    localparam int DATA_W = 256;
    localparam int STRB_W = DATA_W/32;

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

    always @(posedge clk)
        if (axil_doorbell_pulse) doorbell_seen <= 1'b1;

    function automatic [3:0] byte_enable(input [1:0] byte_offset,
                                         input int unsigned length);
        integer i;
        begin
            byte_enable = 4'b0;
            for (i = 0; i < 4; i = i + 1)
                if (i >= byte_offset && i < byte_offset + length)
                    byte_enable[i] = 1'b1;
        end
    endfunction

    function automatic [127:0] make_tlp(input bit is_write,
                                         input [31:0] address,
                                         input int unsigned length,
                                         input int unsigned tag,
                                         input [31:0] data);
        reg [127:0] h;
        begin
            h = '0;
            h[127:125] = is_write ? 3'b010 : 3'b000; // 3DW with/without data
            h[105:96]  = 10'd1;                     // one DW
            h[95:80]   = 16'h0000;                  // requester ID
            h[79:72]   = tag[7:0];
            h[71:68]   = byte_enable(address[1:0], length);
            h[67:64]   = byte_enable(address[1:0], length);
            h[63:34]   = address[31:2];             // 3DW address field
            make_tlp = h;
        end
    endfunction

    task automatic send_tlp(input bit is_write, input [31:0] address,
                            input int unsigned length, input int unsigned tag,
                            input [31:0] data, output [31:0] result);
        integer cycles;
        begin
            result = 32'h0;
            rx_req_tlp_hdr   <= make_tlp(is_write, address, length, tag, data);
            rx_req_tlp_data  <= '0;
            rx_req_tlp_data[31:0] <= data;
            rx_req_tlp_valid <= 1'b1;
            rx_req_tlp_sop   <= 1'b1;
            rx_req_tlp_eop   <= 1'b1;
            cycles = 0;
            while (!rx_req_tlp_ready && cycles < 100) begin
                @(posedge clk); cycles = cycles + 1;
            end
            if (cycles >= 100) $fatal(1, "TLP request was not accepted");
            @(posedge clk);
            rx_req_tlp_valid <= 1'b0;
            rx_req_tlp_sop   <= 1'b0;
            rx_req_tlp_eop   <= 1'b0;
            cycles = 0;
            if (is_write) begin
                while (!(dut.axil_bvalid && dut.axil_bready) && cycles < 100) begin
                    @(posedge clk); cycles = cycles + 1;
                end
                if (cycles >= 100 || dut.axil_bresp != 2'b00)
                    $fatal(1, "AXI-Lite write failed at 0x%08x", address);
            end else begin
                while (!tx_cpl_tlp_valid && cycles < 100) begin
                    @(posedge clk); cycles = cycles + 1;
                end
                if (cycles >= 100) $fatal(1, "completion was not produced");
                if (tx_cpl_tlp_hdr[47:45] != 3'b000)
                    $fatal(1, "completion status was not successful");
                result = tx_cpl_tlp_data[31:0];
                @(posedge clk);
            end
        end
    endtask

    task automatic expect_read(input [31:0] address, input [31:0] expected);
        reg [31:0] value;
        begin
            send_tlp(1'b0, address, 4, 1, 0, value);
            if (value !== expected)
                $fatal(1, "read 0x%08x returned 0x%08x, expected 0x%08x",
                       address, value, expected);
            $display("[VERILATOR][AXI] read 0x%08x = 0x%08x", address, value);
        end
    endtask

    initial begin
        reg [31:0] unused;
        if ($test$plusargs("TRACE")) begin
            $dumpfile("alex_verilator.fst");
            $dumpvars(0, alex_verilator_tb);
        end
        repeat (4) @(posedge clk);
        rst = 1'b0;
        @(posedge clk);

        expect_read(32'h0000, 32'h00000001); // CAP
        send_tlp(1'b1, 32'h0014, 4, 2, 32'hdeadbeef, unused); // CC
        expect_read(32'h0014, 32'hdeadbeef);
        send_tlp(1'b1, 32'h1000, 4, 3, 32'h00000001, unused); // SQ doorbell
        if (!doorbell_seen || axil_doorbell_value !== 32'h00000001)
            $fatal(1, "doorbell pulse/value was not observed");
        expect_read(32'h001c, 32'h00000001); // CSTS.RDY follows CC.EN

        $display("=====================================");
        $display(" VERILATOR ALEX AXI/TLP PASS");
        $display("=====================================");
        $finish;
    end
endmodule
