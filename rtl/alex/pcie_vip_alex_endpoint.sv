// Alex BAR/TLP endpoint boundary for the PCIe VIP.
//
// The ownership/direction is:
//
//   QEMU (process-level MMIO master)
//       -> Mini-ICS/TLP adapter
//       -> pcie_axil_master_minimal
//       -> axi_lite_slave_model (this module's register target)
//
// QEMU is not wired directly to AXI signals; it is the origin of the guest
// MMIO transaction.  The Alex PCIe block is the AXI-Lite master once that
// transaction has crossed the generic TLP boundary.
module pcie_vip_alex_endpoint #(
    parameter TLP_DATA_WIDTH = 256,
    parameter TLP_STRB_WIDTH = TLP_DATA_WIDTH/32,
    parameter TLP_HDR_WIDTH = 128
) (
    input wire clk,
    input wire rst,
    input wire [TLP_DATA_WIDTH-1:0] rx_req_tlp_data,
    input wire [TLP_HDR_WIDTH-1:0] rx_req_tlp_hdr,
    input wire rx_req_tlp_valid,
    input wire rx_req_tlp_sop,
    input wire rx_req_tlp_eop,
    output wire rx_req_tlp_ready,
    output wire [TLP_DATA_WIDTH-1:0] tx_cpl_tlp_data,
    output wire [TLP_STRB_WIDTH-1:0] tx_cpl_tlp_strb,
    output wire [TLP_HDR_WIDTH-1:0] tx_cpl_tlp_hdr,
    output wire tx_cpl_tlp_valid,
    output wire tx_cpl_tlp_sop,
    output wire tx_cpl_tlp_eop,
    input wire tx_cpl_tlp_ready,
    output reg msix_valid,
    output reg [10:0] msix_vector,
    output wire [63:0] dma_desc_base,
    output wire [63:0] dma_cpl_base,
    output wire [31:0] dma_desc_count,
    output wire [31:0] dma_desc_tail,
    output wire [31:0] dma_status,
    output wire [31:0] dma_control,
    output wire dma_doorbell_pulse,
    output wire axil_doorbell_pulse,
    output wire [31:0] axil_doorbell_value
);

    // These nets intentionally remain named and hierarchical so the
    // QEMU-to-AXI transaction can be inspected directly in Questa waves.
    wire [63:0] axil_awaddr;
    wire [2:0]  axil_awprot;
    wire        axil_awvalid;
    wire        axil_awready;
    wire [31:0] axil_wdata;
    wire [3:0]  axil_wstrb;
    wire        axil_wvalid;
    wire        axil_wready;
    wire [1:0]  axil_bresp;
    wire        axil_bvalid;
    wire        axil_bready;
    wire [63:0] axil_araddr;
    wire [2:0]  axil_arprot;
    wire        axil_arvalid;
    wire        axil_arready;
    wire [31:0] axil_rdata;
    wire [1:0]  axil_rresp;
    wire        axil_rvalid;
    wire        axil_rready;

    wire [31:0] cc_reg;
    wire [31:0] csts_reg;
    wire [31:0] aqa_reg;
    wire [63:0] asq_reg;
    wire [63:0] acq_reg;
    wire        doorbell_pulse;
    wire [31:0] doorbell_value;

    pcie_axil_master_minimal #(
        .TLP_DATA_WIDTH(TLP_DATA_WIDTH),
        .TLP_STRB_WIDTH(TLP_STRB_WIDTH),
        .TLP_HDR_WIDTH(TLP_HDR_WIDTH),
        .TLP_SEG_COUNT(1),
        .AXIL_DATA_WIDTH(32),
        .AXIL_ADDR_WIDTH(64)
    ) axil_master_inst (
        .clk(clk), .rst(rst),
        .rx_req_tlp_data(rx_req_tlp_data), .rx_req_tlp_hdr(rx_req_tlp_hdr),
        .rx_req_tlp_valid(rx_req_tlp_valid), .rx_req_tlp_sop(rx_req_tlp_sop),
        .rx_req_tlp_eop(rx_req_tlp_eop), .rx_req_tlp_ready(rx_req_tlp_ready),
        .tx_cpl_tlp_data(tx_cpl_tlp_data), .tx_cpl_tlp_strb(tx_cpl_tlp_strb),
        .tx_cpl_tlp_hdr(tx_cpl_tlp_hdr), .tx_cpl_tlp_valid(tx_cpl_tlp_valid),
        .tx_cpl_tlp_sop(tx_cpl_tlp_sop), .tx_cpl_tlp_eop(tx_cpl_tlp_eop),
        .tx_cpl_tlp_ready(tx_cpl_tlp_ready),
        .m_axil_awaddr(axil_awaddr), .m_axil_awprot(axil_awprot),
        .m_axil_awvalid(axil_awvalid), .m_axil_awready(axil_awready),
        .m_axil_wdata(axil_wdata), .m_axil_wstrb(axil_wstrb),
        .m_axil_wvalid(axil_wvalid), .m_axil_wready(axil_wready),
        .m_axil_bresp(axil_bresp), .m_axil_bvalid(axil_bvalid),
        .m_axil_bready(axil_bready),
        .m_axil_araddr(axil_araddr), .m_axil_arprot(axil_arprot),
        .m_axil_arvalid(axil_arvalid), .m_axil_arready(axil_arready),
        .m_axil_rdata(axil_rdata), .m_axil_rresp(axil_rresp),
        .m_axil_rvalid(axil_rvalid), .m_axil_rready(axil_rready),
        .completer_id(16'h0000), .status_error_cor(), .status_error_uncor()
    );

    axi_lite_slave_model axil_slave_model (
        .clk(clk), .rst(rst),
        .s_axil_awaddr(axil_awaddr), .s_axil_awprot(axil_awprot),
        .s_axil_awvalid(axil_awvalid), .s_axil_awready(axil_awready),
        .s_axil_wdata(axil_wdata), .s_axil_wstrb(axil_wstrb),
        .s_axil_wvalid(axil_wvalid), .s_axil_wready(axil_wready),
        .s_axil_bresp(axil_bresp), .s_axil_bvalid(axil_bvalid),
        .s_axil_bready(axil_bready),
        .s_axil_araddr(axil_araddr), .s_axil_arprot(axil_arprot),
        .s_axil_arvalid(axil_arvalid), .s_axil_arready(axil_arready),
        .s_axil_rdata(axil_rdata), .s_axil_rresp(axil_rresp),
        .s_axil_rvalid(axil_rvalid), .s_axil_rready(axil_rready),
        .cc_reg(cc_reg), .csts_reg(csts_reg), .aqa_reg(aqa_reg),
        .asq_reg(asq_reg), .acq_reg(acq_reg),
        .dma_desc_base_reg(dma_desc_base), .dma_cpl_base_reg(dma_cpl_base),
        .dma_desc_count_reg(dma_desc_count), .dma_desc_tail_reg(dma_desc_tail),
        .dma_status_reg(dma_status), .dma_control_reg(dma_control),
        .dma_doorbell_pulse(dma_doorbell_pulse),
        .doorbell_pulse(doorbell_pulse), .doorbell_value(doorbell_value)
    );

    assign axil_doorbell_pulse = doorbell_pulse;
    assign axil_doorbell_value = doorbell_value;

    // MSI-X generation is driven by the endpoint adapter in the full system.
    // Keep the pins deterministic in this generic BAR/TLP wrapper.
    always @(posedge clk) begin
        if (rst) begin
            msix_valid  <= 1'b0;
            msix_vector <= 11'd0;
        end else begin
            msix_valid <= 1'b0;
        end
    end

`ifndef SYNTHESIS
    always @(posedge clk) begin
        if (!rst) begin
            if (rx_req_tlp_valid && rx_req_tlp_ready)
                $display("[RTL-TLP][%0t] RX_REQ hdr=%032h data=%064h",
                         $time, rx_req_tlp_hdr, rx_req_tlp_data);
            if (tx_cpl_tlp_valid && tx_cpl_tlp_ready)
                $display("[RTL-TLP][%0t] TX_CPL hdr=%032h data=%064h",
                         $time, tx_cpl_tlp_hdr, tx_cpl_tlp_data);
            if (msix_valid)
                $display("[RTL-MSIX][%0t] vector=%0d", $time, msix_vector);
        end
    end
`endif
endmodule
