// Generic Alex dma_if_pcie wrapper used by the descriptor engine.
// The descriptor/control plane may submit one read and one write at a time;
// the TLP streams are exported to the Mini-ICS adapter.
`timescale 1ns / 1ps
module pcie_vip_dma_if_pcie #(
    parameter TLP_DATA_WIDTH = 256,
    parameter TLP_STRB_WIDTH = TLP_DATA_WIDTH/32,
    parameter TLP_HDR_WIDTH = 128,
    parameter RAM_ADDR_WIDTH = 16
) (
    input wire clk,
    input wire rst,
    input wire [TLP_DATA_WIDTH-1:0] rx_cpl_tlp_data,
    input wire [TLP_HDR_WIDTH-1:0] rx_cpl_tlp_hdr,
    input wire [3:0] rx_cpl_tlp_error,
    input wire rx_cpl_tlp_valid, input wire rx_cpl_tlp_sop,
    input wire rx_cpl_tlp_eop, output wire rx_cpl_tlp_ready,
    output wire [TLP_HDR_WIDTH-1:0] tx_rd_req_tlp_hdr,
    output wire tx_rd_req_tlp_valid, output wire tx_rd_req_tlp_sop,
    output wire tx_rd_req_tlp_eop, input wire tx_rd_req_tlp_ready,
    output wire [TLP_DATA_WIDTH-1:0] tx_wr_req_tlp_data,
    output wire [TLP_STRB_WIDTH-1:0] tx_wr_req_tlp_strb,
    output wire [TLP_HDR_WIDTH-1:0] tx_wr_req_tlp_hdr,
    output wire tx_wr_req_tlp_valid, output wire tx_wr_req_tlp_sop,
    output wire tx_wr_req_tlp_eop, input wire tx_wr_req_tlp_ready,
    input wire read_desc_valid, output wire read_desc_ready,
    input wire [63:0] read_desc_pcie_addr,
    input wire [RAM_ADDR_WIDTH-1:0] read_desc_ram_addr,
    input wire [15:0] read_desc_len, input wire [7:0] read_desc_tag,
    output wire read_status_valid, output wire [7:0] read_status_tag,
    output wire [3:0] read_status_error,
    input wire write_desc_valid, output wire write_desc_ready,
    input wire [63:0] write_desc_pcie_addr,
    input wire [RAM_ADDR_WIDTH-1:0] write_desc_ram_addr,
    input wire [15:0] write_desc_len, input wire [7:0] write_desc_tag,
    output wire write_status_valid, output wire [7:0] write_status_tag,
    output wire [3:0] write_status_error
);
    localparam RAM_SEG_COUNT = 2;
    localparam RAM_SEG_DATA_WIDTH = TLP_DATA_WIDTH;
    localparam RAM_SEG_BE_WIDTH = RAM_SEG_DATA_WIDTH/8;
    localparam RAM_SEG_ADDR_WIDTH = RAM_ADDR_WIDTH-$clog2(RAM_SEG_COUNT*RAM_SEG_BE_WIDTH);

    wire [RAM_SEG_COUNT*2-1:0] ram_rd_cmd_sel;
    wire [RAM_SEG_COUNT*RAM_SEG_ADDR_WIDTH-1:0] ram_rd_cmd_addr;
    wire [RAM_SEG_COUNT-1:0] ram_rd_cmd_valid;
    reg [RAM_SEG_COUNT-1:0] ram_rd_cmd_ready = {RAM_SEG_COUNT{1'b1}};
    reg [RAM_SEG_COUNT*TLP_DATA_WIDTH-1:0] ram_rd_resp_data = '0;
    reg [RAM_SEG_COUNT-1:0] ram_rd_resp_valid = '0;
    wire [RAM_SEG_COUNT-1:0] ram_rd_resp_ready;
    wire [RAM_SEG_COUNT*2-1:0] ram_wr_cmd_sel;
    wire [RAM_SEG_COUNT*RAM_SEG_BE_WIDTH-1:0] ram_wr_cmd_be;
    wire [RAM_SEG_COUNT*RAM_SEG_ADDR_WIDTH-1:0] ram_wr_cmd_addr;
    wire [RAM_SEG_COUNT*TLP_DATA_WIDTH-1:0] ram_wr_cmd_data;
    wire [RAM_SEG_COUNT-1:0] ram_wr_cmd_valid;
    reg [RAM_SEG_COUNT-1:0] ram_wr_cmd_ready = {RAM_SEG_COUNT{1'b1}};
    reg [RAM_SEG_COUNT-1:0] ram_wr_done = '0;
    reg [7:0] ram [0:(1 << RAM_ADDR_WIDTH)-1];
    integer s, b;

    always @(posedge clk) begin
        ram_rd_resp_valid <= '0;
        ram_wr_done <= '0;
        for (s = 0; s < RAM_SEG_COUNT; s = s + 1) begin
            if (ram_rd_cmd_valid[s] && ram_rd_cmd_ready[s]) begin
                ram_rd_resp_valid[s] <= 1'b1;
                for (b = 0; b < TLP_DATA_WIDTH/8; b = b + 1)
                    ram_rd_resp_data[s*TLP_DATA_WIDTH + b*8 +: 8] <=
                        ram[(ram_rd_cmd_addr[s*RAM_SEG_ADDR_WIDTH +: RAM_SEG_ADDR_WIDTH] << 5) + b];
            end
            if (ram_wr_cmd_valid[s] && ram_wr_cmd_ready[s]) begin
                ram_wr_done[s] <= 1'b1;
                for (b = 0; b < RAM_SEG_BE_WIDTH; b = b + 1)
                    if (ram_wr_cmd_be[s*RAM_SEG_BE_WIDTH+b])
                        ram[(ram_wr_cmd_addr[s*RAM_SEG_ADDR_WIDTH +: RAM_SEG_ADDR_WIDTH] << 5) + b] <=
                            ram_wr_cmd_data[s*TLP_DATA_WIDTH + b*8 +: 8];
            end
        end
    end

    dma_if_pcie #(
        .TLP_DATA_WIDTH(TLP_DATA_WIDTH), .TLP_STRB_WIDTH(TLP_STRB_WIDTH),
        .TLP_HDR_WIDTH(TLP_HDR_WIDTH), .TLP_SEG_COUNT(1),
        .TX_SEQ_NUM_COUNT(1), .TX_SEQ_NUM_WIDTH(5), .RAM_SEL_WIDTH(2),
        .RAM_ADDR_WIDTH(RAM_ADDR_WIDTH), .RAM_SEG_COUNT(RAM_SEG_COUNT),
        .RAM_SEG_DATA_WIDTH(RAM_SEG_DATA_WIDTH),
        .RAM_SEG_BE_WIDTH(RAM_SEG_BE_WIDTH),
        .RAM_SEG_ADDR_WIDTH(RAM_SEG_ADDR_WIDTH), .PCIE_ADDR_WIDTH(64),
        .PCIE_TAG_COUNT(256), .LEN_WIDTH(16), .TAG_WIDTH(8)
    ) dma_if_inst (
        .clk(clk), .rst(rst),
        .rx_cpl_tlp_data(rx_cpl_tlp_data),
        .rx_cpl_tlp_hdr(rx_cpl_tlp_hdr),
        .rx_cpl_tlp_error(rx_cpl_tlp_error),
        .rx_cpl_tlp_valid(rx_cpl_tlp_valid), .rx_cpl_tlp_sop(rx_cpl_tlp_sop),
        .rx_cpl_tlp_eop(rx_cpl_tlp_eop), .rx_cpl_tlp_ready(rx_cpl_tlp_ready),
        .tx_rd_req_tlp_hdr(tx_rd_req_tlp_hdr),
        .tx_rd_req_tlp_seq(), .tx_rd_req_tlp_valid(tx_rd_req_tlp_valid),
        .tx_rd_req_tlp_sop(tx_rd_req_tlp_sop), .tx_rd_req_tlp_eop(tx_rd_req_tlp_eop),
        .tx_rd_req_tlp_ready(tx_rd_req_tlp_ready),
        .tx_wr_req_tlp_data(tx_wr_req_tlp_data), .tx_wr_req_tlp_strb(tx_wr_req_tlp_strb),
        .tx_wr_req_tlp_hdr(tx_wr_req_tlp_hdr), .tx_wr_req_tlp_seq(),
        .tx_wr_req_tlp_valid(tx_wr_req_tlp_valid), .tx_wr_req_tlp_sop(tx_wr_req_tlp_sop),
        .tx_wr_req_tlp_eop(tx_wr_req_tlp_eop), .tx_wr_req_tlp_ready(tx_wr_req_tlp_ready),
        .s_axis_rd_req_tx_seq_num(5'd0), .s_axis_rd_req_tx_seq_num_valid(1'b0),
        .s_axis_wr_req_tx_seq_num(5'd0), .s_axis_wr_req_tx_seq_num_valid(1'b0),
        .s_axis_read_desc_pcie_addr(read_desc_pcie_addr), .s_axis_read_desc_ram_sel(2'd0),
        .s_axis_read_desc_ram_addr(read_desc_ram_addr), .s_axis_read_desc_len(read_desc_len),
        .s_axis_read_desc_tag(read_desc_tag), .s_axis_read_desc_valid(read_desc_valid),
        .s_axis_read_desc_ready(read_desc_ready),
        .m_axis_read_desc_status_tag(read_status_tag),
        .m_axis_read_desc_status_error(read_status_error),
        .m_axis_read_desc_status_valid(read_status_valid),
        .s_axis_write_desc_pcie_addr(write_desc_pcie_addr), .s_axis_write_desc_ram_sel(2'd0),
        .s_axis_write_desc_ram_addr(write_desc_ram_addr), .s_axis_write_desc_imm(32'd0),
        .s_axis_write_desc_imm_en(1'b0), .s_axis_write_desc_len(write_desc_len),
        .s_axis_write_desc_tag(write_desc_tag), .s_axis_write_desc_valid(write_desc_valid),
        .s_axis_write_desc_ready(write_desc_ready),
        .m_axis_write_desc_status_tag(write_status_tag),
        .m_axis_write_desc_status_error(write_status_error),
        .m_axis_write_desc_status_valid(write_status_valid),
        .ram_rd_cmd_sel(ram_rd_cmd_sel), .ram_rd_cmd_addr(ram_rd_cmd_addr),
        .ram_rd_cmd_valid(ram_rd_cmd_valid), .ram_rd_cmd_ready(ram_rd_cmd_ready),
        .ram_rd_resp_data(ram_rd_resp_data), .ram_rd_resp_valid(ram_rd_resp_valid),
        .ram_rd_resp_ready(ram_rd_resp_ready), .ram_wr_cmd_sel(ram_wr_cmd_sel),
        .ram_wr_cmd_be(ram_wr_cmd_be), .ram_wr_cmd_addr(ram_wr_cmd_addr),
        .ram_wr_cmd_data(ram_wr_cmd_data), .ram_wr_cmd_valid(ram_wr_cmd_valid),
        .ram_wr_cmd_ready(ram_wr_cmd_ready), .ram_wr_done(ram_wr_done),
        .read_enable(1'b1), .write_enable(1'b1), .ext_tag_enable(1'b0),
        .rcb_128b(1'b0), .requester_id(16'd0), .max_read_request_size(3'd0),
        .max_payload_size(3'd0)
    );
endmodule
