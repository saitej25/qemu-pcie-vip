// Minimal NVMe-inspired register block. Implements just enough of the
// real NVMe register layout (CAP/CC/CSTS/AQA/ASQ/ACQ + one submission
// queue doorbell) to drive the MVP demo sequence in
// docs/architecture.md; it is explicitly NOT a full NVMe controller.
//
// Address map (byte offsets), matching src/host_client.cpp and the
// software model in src/server.cpp's --demo mode so the same host
// client / demo sequence works against either:
//   0x0000  CAP   (64-bit, read-only)          Controller Capabilities
//   0x0014  CC    (32-bit, read/write)         Controller Configuration
//   0x001C  CSTS  (32-bit, read-only)          Controller Status
//   0x0024  AQA   (32-bit, read/write)         Admin Queue Attributes
//   0x0028  ASQ   (64-bit, read/write)         Admin Submission Queue Base
//   0x0030  ACQ   (64-bit, read/write)         Admin Completion Queue Base
//   0x1000  SQ0TDBL (32-bit, write-only)       Submission Queue 0 Doorbell
//
// The core register block occupies the first 4 KB (0x000-0xFFF); the
// doorbell lives in the next 4 KB page, mirroring real NVMe BAR layout
// where doorbells follow the fixed register block. Total addressable
// window validated by this model is therefore 8 KB.
module nvme_register_model (
    input logic clk,
    input logic rst_n,

    // Register-level access interface driven by mini_ics_endpoint_model.
    input  logic        req_valid,
    input  logic        req_is_write,
    input  logic [63:0] req_addr,
    input  logic [63:0] req_wdata,
    input  logic [7:0]  req_byte_enable,   // one bit per byte lane, up to 8 bytes
    input  logic [3:0]  req_len,           // 4 or 8 bytes
    output logic        rsp_valid,
    output logic [63:0] rsp_rdata,
    output logic        rsp_addr_invalid,

    output logic        doorbell_pulse,    // one-cycle pulse when SQ0TDBL is written
    output logic [31:0] doorbell_value,
    output logic        ctrl_enabled,      // mirrors CC.EN, for testbench visibility
    output logic [63:0] admin_sq_addr,
    output logic [63:0] admin_cq_addr
);

    localparam logic [63:0] ADDR_CAP     = 64'h0000;
    localparam logic [63:0] ADDR_CC      = 64'h0014;
    localparam logic [63:0] ADDR_CSTS    = 64'h001C;
    localparam logic [63:0] ADDR_AQA     = 64'h0024;
    localparam logic [63:0] ADDR_ASQ     = 64'h0028;
    localparam logic [63:0] ADDR_ACQ     = 64'h0030;
    localparam logic [63:0] ADDR_SQ0TDBL = 64'h1000;

    localparam logic [63:0] MMIO_WINDOW_SIZE = 64'h2000;  // 8 KB, see module header comment

    // Controller Capabilities: fixed, read-only value for this MVP.
    // Bit 0 set simply marks "capabilities present"; no real NVMe CAP
    // fields (MQES, TO, etc.) are modeled yet.
    localparam logic [63:0] CAP_VALUE = 64'h0000_0000_0000_0001;

    logic [31:0] cc_reg;
    logic [31:0] csts_reg;
    logic [31:0] aqa_reg;
    logic [63:0] asq_reg;
    logic [63:0] acq_reg;

    assign ctrl_enabled = cc_reg[0];
    assign admin_sq_addr = asq_reg;
    assign admin_cq_addr = acq_reg;

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            cc_reg         <= '0;
            csts_reg       <= '0;
            aqa_reg        <= '0;
            asq_reg        <= '0;
            acq_reg        <= '0;
            rsp_valid      <= 1'b0;
            rsp_rdata      <= '0;
            rsp_addr_invalid <= 1'b0;
            doorbell_pulse <= 1'b0;
            doorbell_value <= '0;
        end else begin
            rsp_valid      <= 1'b0;
            doorbell_pulse <= 1'b0;
            rsp_addr_invalid <= 1'b0;

            if (req_valid) begin
                if (req_addr >= MMIO_WINDOW_SIZE) begin
                    rsp_valid        <= 1'b1;
                    rsp_addr_invalid <= 1'b1;
                    rsp_rdata        <= '0;
                end else if (req_is_write) begin
                    rsp_valid <= 1'b1;
                    unique case (req_addr)
                        ADDR_CC: begin
                            cc_reg <= req_wdata[31:0];
                            if (req_wdata[0]) begin
                                csts_reg[0] <= 1'b1;  // CSTS.RDY follows CC.EN
                            end else begin
                                csts_reg[0] <= 1'b0;
                            end
                        end
                        ADDR_AQA: aqa_reg <= req_wdata[31:0];
                        ADDR_ASQ: asq_reg <= req_wdata;
                        ADDR_ACQ: acq_reg <= req_wdata;
                        ADDR_SQ0TDBL: begin
                            doorbell_pulse <= 1'b1;
                            doorbell_value <= req_wdata[31:0];
                        end
                        default: rsp_addr_invalid <= 1'b1;
                    endcase
                end else begin
                    // Read
                    rsp_valid <= 1'b1;
                    unique case (req_addr)
                        ADDR_CAP:  rsp_rdata <= CAP_VALUE;
                        ADDR_CC:   rsp_rdata <= {32'b0, cc_reg};
                        ADDR_CSTS: rsp_rdata <= {32'b0, csts_reg};
                        ADDR_AQA:  rsp_rdata <= {32'b0, aqa_reg};
                        ADDR_ASQ:  rsp_rdata <= asq_reg;
                        ADDR_ACQ:  rsp_rdata <= acq_reg;
                        default: begin
                            rsp_addr_invalid <= 1'b1;
                            rsp_rdata        <= '0;
                        end
                    endcase
                end
            end
        end
    end

endmodule : nvme_register_model
