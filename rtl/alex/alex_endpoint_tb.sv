`timescale 1ns/1ps

module alex_endpoint_tb;
    localparam int DATA_W = 256;
    localparam int STRB_W = DATA_W/32;
    logic clk = 0;
    logic rst = 1;
    always #5 clk = ~clk;
    logic [DATA_W-1:0] rx_req_tlp_data = '0;
    logic [127:0] rx_req_tlp_hdr = '0;
    logic rx_req_tlp_valid = 0, rx_req_tlp_sop = 0, rx_req_tlp_eop = 0;
    wire rx_req_tlp_ready;
    wire [DATA_W-1:0] tx_cpl_tlp_data;
    wire [STRB_W-1:0] tx_cpl_tlp_strb;
    wire [127:0] tx_cpl_tlp_hdr;
    wire tx_cpl_tlp_valid, tx_cpl_tlp_sop, tx_cpl_tlp_eop;
    logic tx_cpl_tlp_ready = 1;
    wire msix_valid;
    wire [10:0] msix_vector;
    wire axil_doorbell_pulse;
    wire [31:0] axil_doorbell_value;
    pcie_vip_alex_endpoint #(.TLP_DATA_WIDTH(DATA_W), .TLP_STRB_WIDTH(STRB_W), .TLP_HDR_WIDTH(128)) dut (
        .clk, .rst, .rx_req_tlp_data, .rx_req_tlp_hdr, .rx_req_tlp_valid,
        .rx_req_tlp_sop, .rx_req_tlp_eop, .rx_req_tlp_ready,
        .tx_cpl_tlp_data, .tx_cpl_tlp_strb, .tx_cpl_tlp_hdr,
        .tx_cpl_tlp_valid, .tx_cpl_tlp_sop, .tx_cpl_tlp_eop,
        .tx_cpl_tlp_ready, .msix_valid, .msix_vector,
        .axil_doorbell_pulse, .axil_doorbell_value);
    initial begin repeat (4) @(posedge clk); rst <= 0; end
endmodule
