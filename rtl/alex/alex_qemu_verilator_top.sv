// Clock-driven QEMU-facing wrapper for the Alex endpoint.
//
// The C++ Verilator harness owns the Mini-ICS socket and presents one
// transaction-level MMIO request at a time. This module converts that
// request into one or two Memory TLPs and drives pcie_axil_master_minimal.
// There are no simulator timing controls, so this works with Verilator 4.x.
module alex_qemu_verilator_top (
    input logic clk, input logic rst,
    input logic mmio_req_valid, output wire mmio_req_ready,
    input logic mmio_req_write, input logic [63:0] mmio_req_addr,
    input logic [3:0] mmio_req_len, input logic [63:0] mmio_req_data,
    output logic mmio_rsp_valid, output logic [63:0] mmio_rsp_data,
    output logic [1:0] mmio_rsp_status,
    output wire [63:0] dma_desc_base, output wire [63:0] dma_cpl_base,
    output wire [31:0] dma_desc_count, output wire [31:0] dma_desc_tail,
    output wire [31:0] dma_status, output wire [31:0] dma_control,
    output wire dma_doorbell_pulse,
    input wire dma_read_desc_valid, output wire dma_read_desc_ready,
    input wire [63:0] dma_read_desc_pcie_addr,
    input wire [15:0] dma_read_desc_ram_addr,
    input wire [15:0] dma_read_desc_len, input wire [7:0] dma_read_desc_tag,
    output wire dma_read_status_valid, output wire [7:0] dma_read_status_tag,
    output wire [3:0] dma_read_status_error,
    input wire dma_write_desc_valid, output wire dma_write_desc_ready,
    input wire [63:0] dma_write_desc_pcie_addr,
    input wire [15:0] dma_write_desc_ram_addr,
    input wire [15:0] dma_write_desc_len, input wire [7:0] dma_write_desc_tag,
    output wire dma_write_status_valid, output wire [7:0] dma_write_status_tag,
    output wire [3:0] dma_write_status_error,
    output wire [63:0] dma_rd_req_addr, output wire [15:0] dma_rd_req_len,
    output wire dma_rd_req_valid, output wire dma_rd_req_sop, output wire dma_rd_req_eop,
    input wire dma_rd_req_ready,
    output wire [63:0] dma_wr_req_addr, output wire [15:0] dma_wr_req_len,
    output wire dma_wr_req_valid, output wire dma_wr_req_sop, output wire dma_wr_req_eop,
    output wire [63:0] dma_wr_req_data0, output wire [63:0] dma_wr_req_data1,
    output wire [63:0] dma_wr_req_data2, output wire [63:0] dma_wr_req_data3,
    input wire dma_wr_req_ready,
    input wire dma_rx_cpl_valid, input wire dma_rx_cpl_sop, input wire dma_rx_cpl_eop,
    input wire [63:0] dma_rx_cpl_hdr_lo, input wire [63:0] dma_rx_cpl_hdr_hi,
    input wire [63:0] dma_rx_cpl_data0, input wire [63:0] dma_rx_cpl_data1,
    input wire [63:0] dma_rx_cpl_data2, input wire [63:0] dma_rx_cpl_data3,
    input wire [3:0] dma_rx_cpl_error, output wire dma_rx_cpl_ready,
    output wire doorbell_pulse, output wire [31:0] doorbell_value
);
    localparam int DATA_W = 256;
    localparam int STRB_W = DATA_W/32;
    logic [DATA_W-1:0] rx_req_tlp_data = '0;
    logic [127:0] rx_req_tlp_hdr = '0;
    logic rx_req_tlp_valid = 1'b0, rx_req_tlp_sop = 1'b0, rx_req_tlp_eop = 1'b0;
    wire rx_req_tlp_ready;
    wire [DATA_W-1:0] tx_cpl_tlp_data;
    wire [STRB_W-1:0] tx_cpl_tlp_strb;
    wire [127:0] tx_cpl_tlp_hdr;
    wire tx_cpl_tlp_valid, tx_cpl_tlp_sop, tx_cpl_tlp_eop;
    logic tx_cpl_tlp_ready = 1'b1;
    wire msix_valid;
    wire [10:0] msix_vector;
    wire [255:0] dma_tx_wr_data_int;
    wire [127:0] dma_tx_rd_hdr_int, dma_tx_wr_hdr_int;
    wire [255:0] dma_rx_cpl_data_int = {dma_rx_cpl_data3, dma_rx_cpl_data2,
                                         dma_rx_cpl_data1, dma_rx_cpl_data0};
    wire [127:0] dma_rx_cpl_hdr_int = {dma_rx_cpl_hdr_hi, dma_rx_cpl_hdr_lo};

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
        .dma_desc_base(dma_desc_base), .dma_cpl_base(dma_cpl_base),
        .dma_desc_count(dma_desc_count), .dma_desc_tail(dma_desc_tail),
        .dma_status(dma_status), .dma_control(dma_control),
        .dma_doorbell_pulse(dma_doorbell_pulse),
        .msix_vector(msix_vector), .axil_doorbell_pulse(doorbell_pulse),
        .axil_doorbell_value(doorbell_value)
    );

    assign dma_rd_req_addr = {dma_tx_rd_hdr_int[63:34], 2'b00};
    assign dma_rd_req_len = {6'd0, dma_tx_rd_hdr_int[105:96], 2'b00};
    assign dma_rd_req_valid = dma_tx_rd_req_valid_int;
    assign dma_rd_req_sop = dma_tx_rd_req_sop_int;
    assign dma_rd_req_eop = dma_tx_rd_req_eop_int;
    assign dma_wr_req_addr = {dma_tx_wr_hdr_int[63:34], 2'b00};
    assign dma_wr_req_len = {6'd0, dma_tx_wr_hdr_int[105:96], 2'b00};
    assign dma_wr_req_valid = dma_tx_wr_req_valid_int;
    assign dma_wr_req_sop = dma_tx_wr_req_sop_int;
    assign dma_wr_req_eop = dma_tx_wr_req_eop_int;
    assign dma_wr_req_data0 = dma_tx_wr_data_int[63:0];
    assign dma_wr_req_data1 = dma_tx_wr_data_int[127:64];
    assign dma_wr_req_data2 = dma_tx_wr_data_int[191:128];
    assign dma_wr_req_data3 = dma_tx_wr_data_int[255:192];

    wire [127:0] dma_tx_rd_hdr_int;
    wire dma_tx_rd_req_valid_int, dma_tx_rd_req_sop_int, dma_tx_rd_req_eop_int;
    wire [127:0] dma_tx_wr_hdr_int;
    wire dma_tx_wr_req_valid_int, dma_tx_wr_req_sop_int, dma_tx_wr_req_eop_int;

    pcie_vip_dma_if_pcie dma_engine (
        .clk(clk), .rst(rst), .rx_cpl_tlp_data(dma_rx_cpl_data_int),
        .rx_cpl_tlp_hdr(dma_rx_cpl_hdr_int), .rx_cpl_tlp_error(dma_rx_cpl_error),
        .rx_cpl_tlp_valid(dma_rx_cpl_valid), .rx_cpl_tlp_sop(dma_rx_cpl_sop),
        .rx_cpl_tlp_eop(dma_rx_cpl_eop), .rx_cpl_tlp_ready(dma_rx_cpl_ready),
        .tx_rd_req_tlp_hdr(dma_tx_rd_hdr_int), .tx_rd_req_tlp_valid(dma_tx_rd_req_valid_int),
        .tx_rd_req_tlp_sop(dma_tx_rd_req_sop_int), .tx_rd_req_tlp_eop(dma_tx_rd_req_eop_int),
        .tx_rd_req_tlp_ready(dma_rd_req_ready), .tx_wr_req_tlp_data(dma_tx_wr_data_int),
        .tx_wr_req_tlp_strb(), .tx_wr_req_tlp_hdr(dma_tx_wr_hdr_int),
        .tx_wr_req_tlp_valid(dma_tx_wr_req_valid_int), .tx_wr_req_tlp_sop(dma_tx_wr_req_sop_int),
        .tx_wr_req_tlp_eop(dma_tx_wr_req_eop_int), .tx_wr_req_tlp_ready(dma_wr_req_ready),
        .read_desc_valid(dma_read_desc_valid), .read_desc_ready(dma_read_desc_ready),
        .read_desc_pcie_addr(dma_read_desc_pcie_addr), .read_desc_ram_addr(dma_read_desc_ram_addr),
        .read_desc_len(dma_read_desc_len), .read_desc_tag(dma_read_desc_tag),
        .read_status_valid(dma_read_status_valid), .read_status_tag(dma_read_status_tag),
        .read_status_error(dma_read_status_error), .write_desc_valid(dma_write_desc_valid),
        .write_desc_ready(dma_write_desc_ready), .write_desc_pcie_addr(dma_write_desc_pcie_addr),
        .write_desc_ram_addr(dma_write_desc_ram_addr), .write_desc_len(dma_write_desc_len),
        .write_desc_tag(dma_write_desc_tag), .write_status_valid(dma_write_status_valid),
        .write_status_tag(dma_write_status_tag), .write_status_error(dma_write_status_error)
    );

    localparam [3:0] ST_IDLE=0, ST_SEND=1, ST_HANDSHAKE=2,
                     ST_WRITE_WAIT=3, ST_READ_WAIT=4, ST_RESPONSE=5;
    logic [3:0] state = ST_IDLE;
    logic req_write_reg;
    logic [63:0] req_addr_reg, req_data_reg;
    logic [3:0] req_len_reg, offset_reg, chunk_len_reg;
    logic [63:0] response_data_reg;
    logic [1:0] response_status_reg;
    logic [7:0] timeout_count;

    assign mmio_req_ready = (state == ST_IDLE) && !mmio_rsp_valid;

    function automatic [3:0] byte_enable(input [1:0] byte_offset,
                                         input [3:0] length);
        integer i;
        begin
            byte_enable = 4'b0;
            for (i=0; i<4; i=i+1)
                if (i >= byte_offset && i < byte_offset + length)
                    byte_enable[i] = 1'b1;
        end
    endfunction

    function automatic [127:0] make_tlp(input bit is_write,
                                         input [63:0] address,
                                         input [3:0] length,
                                         input [7:0] tag, input [31:0] data);
        reg [127:0] h;
        begin
            h='0; h[127:125] = is_write ? 3'b010 : 3'b000;
            h[105:96]=10'd1; h[79:72]=tag;
            h[71:68]=byte_enable(address[1:0], length);
            h[67:64]=byte_enable(address[1:0], length);
            h[63:34]=address[31:2]; make_tlp=h;
        end
    endfunction

    always @(posedge clk) begin
        if (rst) begin
            state<=ST_IDLE; rx_req_tlp_valid<=0; rx_req_tlp_sop<=0;
            rx_req_tlp_eop<=0; mmio_rsp_valid<=0; mmio_rsp_data<='0;
            mmio_rsp_status<=0; timeout_count<=0;
        end else begin
            if (mmio_rsp_valid) mmio_rsp_valid<=0;
            case (state)
                ST_IDLE: if (mmio_req_valid && mmio_req_ready) begin
                    if (mmio_req_len==0 || mmio_req_len>8 ||
                        (mmio_req_len!=1 && mmio_req_len!=2 &&
                         mmio_req_len!=4 && mmio_req_len!=8)) begin
                        mmio_rsp_data<='0; mmio_rsp_status<=2'b11;
                        mmio_rsp_valid<=1; 
                    end else begin
                        req_write_reg<=mmio_req_write; req_addr_reg<=mmio_req_addr;
                        req_len_reg<=mmio_req_len; req_data_reg<=mmio_req_data;
                        offset_reg<=0; response_data_reg<=0;
                        response_status_reg<=0; timeout_count<=0; state<=ST_SEND;
                    end
                end
                ST_SEND: begin
                    chunk_len_reg <= ((req_len_reg-offset_reg)>=4) ? 4 :
                                      (req_len_reg-offset_reg);
                    rx_req_tlp_hdr <= make_tlp(req_write_reg, req_addr_reg+offset_reg,
                        ((req_len_reg-offset_reg)>=4) ? 4 : (req_len_reg-offset_reg),
                        offset_reg+1, (offset_reg==0) ? req_data_reg[31:0] :
                        req_data_reg[63:32]);
                    rx_req_tlp_data<='0;
                    if (offset_reg==0) rx_req_tlp_data[31:0]<=req_data_reg[31:0];
                    else rx_req_tlp_data[31:0]<=req_data_reg[63:32];
                    rx_req_tlp_valid<=1; rx_req_tlp_sop<=1; rx_req_tlp_eop<=1;
                    timeout_count<=0; state<=ST_HANDSHAKE;
                end
                ST_HANDSHAKE: if (rx_req_tlp_ready) begin
                    rx_req_tlp_valid<=0; rx_req_tlp_sop<=0; rx_req_tlp_eop<=0;
                    state <= req_write_reg ? ST_WRITE_WAIT : ST_READ_WAIT;
                end
                ST_WRITE_WAIT: if (dut.axil_bvalid && dut.axil_bready) begin
                    if (dut.axil_bresp!=0) response_status_reg<=2'b11;
                    if (offset_reg+chunk_len_reg<req_len_reg) begin
                        offset_reg<=offset_reg+chunk_len_reg; state<=ST_SEND;
                    end else begin
                        mmio_rsp_data<=0;
                        mmio_rsp_status <= (dut.axil_bresp==0 && response_status_reg==0) ?
                                           2'b00 : 2'b11;
                        mmio_rsp_valid<=1; state<=ST_RESPONSE;
                    end
                end else if (timeout_count==8'd100) begin
                    mmio_rsp_status<=2'b11; mmio_rsp_valid<=1; state<=ST_RESPONSE;
                end else timeout_count<=timeout_count+1'b1;
                ST_READ_WAIT: if (tx_cpl_tlp_valid) begin
                    if (tx_cpl_tlp_hdr[47:45]!=3'b000) response_status_reg<=2'b11;
                    if (offset_reg==0) response_data_reg[31:0]<=tx_cpl_tlp_data[31:0];
                    else response_data_reg[63:32]<=tx_cpl_tlp_data[31:0];
                    if (offset_reg+chunk_len_reg<req_len_reg) begin
                        offset_reg<=offset_reg+chunk_len_reg; state<=ST_SEND;
                    end else begin
                        mmio_rsp_data <= (offset_reg==0) ? tx_cpl_tlp_data[31:0] :
                            {tx_cpl_tlp_data[31:0],response_data_reg[31:0]};
                        mmio_rsp_status <= (tx_cpl_tlp_hdr[47:45]==3'b000 &&
                                             response_status_reg==0) ? 2'b00 : 2'b11;
                        mmio_rsp_valid<=1; state<=ST_RESPONSE;
                    end
                end else if (timeout_count==8'd100) begin
                    mmio_rsp_status<=2'b11; mmio_rsp_valid<=1; state<=ST_RESPONSE;
                end else timeout_count<=timeout_count+1'b1;
                ST_RESPONSE: state<=ST_IDLE;
                default: state<=ST_IDLE;
            endcase
        end
    end
endmodule
