// Small AXI-Lite slave used at the end of the Alex PCIe BAR path.
//
// The process-level master is QEMU, but after the Mini-ICS/TLP boundary the
// Alex pcie_axil_master_minimal drives this interface.  Keeping the slave in
// its own module makes the AXI handshake and register state visible in waves.
module axi_lite_slave_model #(
    parameter ADDR_WIDTH = 64,
    parameter DATA_WIDTH = 32
) (
    input  wire                    clk,
    input  wire                    rst,

    input  wire [ADDR_WIDTH-1:0]   s_axil_awaddr,
    input  wire [2:0]              s_axil_awprot,
    input  wire                    s_axil_awvalid,
    output wire                    s_axil_awready,
    input  wire [DATA_WIDTH-1:0]   s_axil_wdata,
    input  wire [DATA_WIDTH/8-1:0] s_axil_wstrb,
    input  wire                    s_axil_wvalid,
    output wire                    s_axil_wready,
    output reg [1:0]               s_axil_bresp,
    output reg                     s_axil_bvalid,
    input  wire                    s_axil_bready,

    input  wire [ADDR_WIDTH-1:0]   s_axil_araddr,
    input  wire [2:0]              s_axil_arprot,
    input  wire                    s_axil_arvalid,
    output wire                    s_axil_arready,
    output reg [DATA_WIDTH-1:0]    s_axil_rdata,
    output reg [1:0]               s_axil_rresp,
    output reg                     s_axil_rvalid,
    input  wire                    s_axil_rready,

    output reg [31:0]              cc_reg,
    output reg [31:0]              csts_reg,
    output reg [31:0]              aqa_reg,
    output reg [63:0]              asq_reg,
    output reg [63:0]              acq_reg,
    output reg [63:0]              dma_desc_base_reg,
    output reg [63:0]              dma_cpl_base_reg,
    output reg [31:0]              dma_desc_count_reg,
    output reg [31:0]              dma_desc_tail_reg,
    output reg [31:0]              dma_status_reg,
    output reg [31:0]              dma_control_reg,
    output reg                     dma_doorbell_pulse,
    output reg                     doorbell_pulse,
    output reg [31:0]              doorbell_value
);

    localparam [1:0] AXIL_OKAY   = 2'b00;
    localparam [1:0] AXIL_DECERR = 2'b11;

    reg                    aw_pending;
    reg [ADDR_WIDTH-1:0]   awaddr_reg;
    reg                    w_pending;
    reg [DATA_WIDTH-1:0]   wdata_reg;
    reg [DATA_WIDTH/8-1:0] wstrb_reg;

    assign s_axil_awready = !aw_pending && !s_axil_bvalid;
    assign s_axil_wready  = !w_pending  && !s_axil_bvalid;
    assign s_axil_arready = !s_axil_rvalid;

    function automatic [31:0] apply_wstrb;
        input [31:0] old_value;
        input [31:0] new_value;
        input [3:0]  strb;
        integer k;
        begin
            apply_wstrb = old_value;
            for (k = 0; k < 4; k = k + 1)
                if (strb[k]) apply_wstrb[k*8 +: 8] = new_value[k*8 +: 8];
        end
    endfunction

    function automatic is_valid_write;
        input [12:0] address;
        begin
            case (address)
                13'h0014, 13'h0024, 13'h0028, 13'h002c,
                13'h0030, 13'h0034, 13'h1000,
                13'h1040, 13'h1044, 13'h1048, 13'h104c,
                13'h1050, 13'h1054, 13'h1058, 13'h105c:
                    is_valid_write = 1'b1;
                default: is_valid_write = 1'b0;
            endcase
        end
    endfunction

    function automatic is_valid_read;
        input [12:0] address;
        begin
            case (address)
                13'h0000, 13'h0014, 13'h001c, 13'h0024,
                13'h0028, 13'h002c, 13'h0030, 13'h0034: is_valid_read = 1'b1;
                13'h1040, 13'h1044, 13'h1048, 13'h104c,
                13'h1050, 13'h1054, 13'h1058, 13'h105c: is_valid_read = 1'b1;
                default: is_valid_read = 1'b0;
            endcase
        end
    endfunction

    function automatic [31:0] read_value;
        input [12:0] address;
        begin
            case (address)
                13'h0000: read_value = 32'h00000001; // CAP
                13'h0014: read_value = cc_reg;
                13'h001c: read_value = csts_reg;
                13'h0024: read_value = aqa_reg;
                13'h0028: read_value = asq_reg[31:0];
                13'h002c: read_value = asq_reg[63:32];
                13'h0030: read_value = acq_reg[31:0];
                13'h0034: read_value = acq_reg[63:32];
                13'h1040: read_value = dma_desc_base_reg[31:0];
                13'h1044: read_value = dma_desc_base_reg[63:32];
                13'h1048: read_value = dma_cpl_base_reg[31:0];
                13'h104c: read_value = dma_cpl_base_reg[63:32];
                13'h1050: read_value = dma_desc_count_reg;
                13'h1054: read_value = dma_desc_tail_reg;
                13'h1058: read_value = dma_status_reg;
                13'h105c: read_value = dma_control_reg;
                default: read_value = 32'h00000000;
            endcase
        end
    endfunction

    always @(posedge clk) begin
        if (rst) begin
            aw_pending     <= 1'b0;
            awaddr_reg     <= '0;
            w_pending      <= 1'b0;
            wdata_reg      <= '0;
            wstrb_reg      <= '0;
            s_axil_bresp   <= AXIL_OKAY;
            s_axil_bvalid  <= 1'b0;
            s_axil_rdata   <= '0;
            s_axil_rresp   <= AXIL_OKAY;
            s_axil_rvalid  <= 1'b0;
            cc_reg         <= '0;
            csts_reg       <= '0;
            aqa_reg        <= '0;
            asq_reg        <= '0;
            acq_reg        <= '0;
            dma_desc_base_reg <= '0;
            dma_cpl_base_reg <= '0;
            dma_desc_count_reg <= '0;
            dma_desc_tail_reg <= '0;
            dma_status_reg <= '0;
            dma_control_reg <= '0;
            dma_doorbell_pulse <= 1'b0;
            doorbell_pulse <= 1'b0;
            dma_doorbell_pulse <= 1'b0;
            doorbell_value <= '0;
        end else begin
            doorbell_pulse <= 1'b0;

            if (s_axil_awvalid && s_axil_awready) begin
                aw_pending <= 1'b1;
                awaddr_reg <= s_axil_awaddr;
            end
            if (s_axil_wvalid && s_axil_wready) begin
                w_pending <= 1'b1;
                wdata_reg <= s_axil_wdata;
                wstrb_reg <= s_axil_wstrb;
            end

            if (s_axil_bvalid && s_axil_bready)
                s_axil_bvalid <= 1'b0;

            // Execute a write after both independent AXI-Lite channels have
            // arrived.  The one-cycle staging also handles masters that send
            // AW and W in different cycles.
            if (aw_pending && w_pending && !s_axil_bvalid) begin
                s_axil_bvalid <= 1'b1;
                $display("[AXIL][%0t ns] WRITE addr=0x%08h data=0x%08h strb=0x%0h",
                         $time, awaddr_reg, wdata_reg, wstrb_reg);
                if (is_valid_write(awaddr_reg[12:0])) begin
                    s_axil_bresp <= AXIL_OKAY;
                    case (awaddr_reg[12:0])
                        13'h0014: begin
                            cc_reg   <= apply_wstrb(cc_reg, wdata_reg, wstrb_reg);
                            csts_reg <= {31'd0,
                                (wstrb_reg[0] ? wdata_reg[0] : cc_reg[0])};
                        end
                        13'h0024: aqa_reg <= apply_wstrb(aqa_reg, wdata_reg, wstrb_reg);
                        13'h0028: asq_reg[31:0]  <= apply_wstrb(asq_reg[31:0],  wdata_reg, wstrb_reg);
                        13'h002c: asq_reg[63:32] <= apply_wstrb(asq_reg[63:32], wdata_reg, wstrb_reg);
                        13'h0030: acq_reg[31:0]  <= apply_wstrb(acq_reg[31:0],  wdata_reg, wstrb_reg);
                        13'h0034: acq_reg[63:32] <= apply_wstrb(acq_reg[63:32], wdata_reg, wstrb_reg);
                        13'h1040: dma_desc_base_reg[31:0] <= apply_wstrb(dma_desc_base_reg[31:0], wdata_reg, wstrb_reg);
                        13'h1044: dma_desc_base_reg[63:32] <= apply_wstrb(dma_desc_base_reg[63:32], wdata_reg, wstrb_reg);
                        13'h1048: dma_cpl_base_reg[31:0] <= apply_wstrb(dma_cpl_base_reg[31:0], wdata_reg, wstrb_reg);
                        13'h104c: dma_cpl_base_reg[63:32] <= apply_wstrb(dma_cpl_base_reg[63:32], wdata_reg, wstrb_reg);
                        13'h1050: dma_desc_count_reg <= apply_wstrb(dma_desc_count_reg, wdata_reg, wstrb_reg);
                        13'h1054: begin
                            dma_desc_tail_reg <= apply_wstrb(dma_desc_tail_reg, wdata_reg, wstrb_reg);
                            dma_doorbell_pulse <= 1'b1;
                        end
                        13'h1058: dma_status_reg <= apply_wstrb(dma_status_reg, wdata_reg, wstrb_reg);
                        13'h105c: dma_control_reg <= apply_wstrb(dma_control_reg, wdata_reg, wstrb_reg);
                        13'h1000: begin
                            doorbell_value <= wdata_reg;
                            doorbell_pulse <= 1'b1;
                        end
                    endcase
                end else begin
                    s_axil_bresp <= AXIL_DECERR;
                end
                aw_pending <= 1'b0;
                w_pending  <= 1'b0;
            end

            if (s_axil_arvalid && s_axil_arready) begin
                s_axil_rvalid <= 1'b1;
                $display("[AXIL][%0t ns] READ addr=0x%08h", $time, s_axil_araddr);
                if (is_valid_read(s_axil_araddr[12:0])) begin
                    s_axil_rresp <= AXIL_OKAY;
                    s_axil_rdata <= read_value(s_axil_araddr[12:0]);
                end else begin
                    s_axil_rresp <= AXIL_DECERR;
                    s_axil_rdata <= '0;
                end
            end
            if (s_axil_rvalid && s_axil_rready)
                s_axil_rvalid <= 1'b0;
        end
    end
endmodule
