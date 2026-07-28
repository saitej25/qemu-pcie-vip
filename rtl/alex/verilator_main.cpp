// Portable Verilator 4.x entry point for alex_verilator_compat_tb.
#include "Valex_verilator_compat_tb.h"
#include "verilated.h"

// Verilator 4.x emits a reference to this legacy timestamp callback when
// compiling SystemVerilog that uses $time, even without SystemC enabled.
double sc_time_stamp()
{
    return 0.0;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    auto *top = new Valex_verilator_compat_tb;
    top->clk = 0;
    top->rst = 1;

    // Give the endpoint four rising edges in reset, matching the Questa
    // bring-up test, then release reset before running the self-checking FSM.
    for (int i = 0; i < 4; ++i) {
        top->eval();
        top->clk = 1;
        top->eval();
        top->clk = 0;
    }
    top->rst = 0;
    while (!Verilated::gotFinish()) {
        top->clk = 1;
        top->eval();
        top->clk = 0;
        top->eval();
    }

    top->final();
    delete top;
    return 0;
}
