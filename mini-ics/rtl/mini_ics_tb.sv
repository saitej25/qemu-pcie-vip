// Top-level testbench: generates a simple free-running clock and reset,
// instantiates mini_ics_endpoint_model, waits for it to report
// completion, and prints a single unambiguous PASS/FAIL line.
//
// Timing is a deterministic simulated clock (10ns period, plain #delay
// reset deassertion) -- there is no wall-clock dependency here; see
// docs/architecture.md, "Simulation-time versus wall-clock-time".
`timescale 1ns/1ps

module mini_ics_tb;

    localparam int CLK_PERIOD_NS = 10;
    localparam int MAX_CYCLES = 200000;  // hard bound so a stuck DPI call can't hang forever

    logic clk;
    logic rst_n;
    logic sim_done;
    logic sim_pass;

    // Socket path can be overridden at invocation time via
    // +MINI_ICS_SOCKET=/path/to.sock (see scripts/run_questa.sh /
    // run_verilator.sh); defaults to /tmp/mini_ics.sock for interactive
    // use.
    string socket_path;
    initial begin
        if (!$value$plusargs("MINI_ICS_SOCKET=%s", socket_path)) begin
            socket_path = "/tmp/mini_ics.sock";
        end
    end

    initial clk = 1'b0;
    always #(CLK_PERIOD_NS/2) clk = ~clk;

    initial begin
        rst_n = 1'b0;
        repeat (4) @(posedge clk);
        rst_n = 1'b1;
    end

    // socket_path is resolved from +MINI_ICS_SOCKET at simulation time
    // (see above), which a module parameter can't observe at
    // elaboration; mini_ics_endpoint_model takes it as a runtime input
    // instead for exactly that reason.
    mini_ics_endpoint_model u_endpoint (
        .clk         (clk),
        .rst_n       (rst_n),
        .socket_path (socket_path),
        .sim_done    (sim_done),
        .sim_pass    (sim_pass)
    );

    int cycle_count;
    initial cycle_count = 0;
    always @(posedge clk) begin
        cycle_count <= cycle_count + 1;
    end

    // Basic liveness assertion: the endpoint must reach sim_done before
    // MAX_CYCLES elapse, otherwise something in the DPI/host handshake
    // is stuck and we want a clear failure rather than a silent hang.
    initial begin
        wait (sim_done === 1'b1 || cycle_count >= MAX_CYCLES);
        if (cycle_count >= MAX_CYCLES && sim_done !== 1'b1) begin
            $display("=====================================");
            $display(" MINI ICS TESTBENCH: FAIL (timeout, no sim_done after %0d cycles)",
                      MAX_CYCLES);
            $display("=====================================");
            $finish;
        end

        if (sim_pass) begin
            $display("=====================================");
            $display(" MINI ICS TESTBENCH: PASS");
            $display("=====================================");
        end else begin
            $display("=====================================");
            $display(" MINI ICS TESTBENCH: FAIL (endpoint did not report sim_pass)");
            $display("=====================================");
        end
        $finish;
    end

endmodule : mini_ics_tb
