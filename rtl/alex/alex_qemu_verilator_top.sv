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
