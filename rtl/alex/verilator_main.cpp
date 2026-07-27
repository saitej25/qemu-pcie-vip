// Portable Verilator 4.x entry point for alex_verilator_compat_tb.
#include "Valex_verilator_compat_tb.h"
#include "verilated.h"

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
        Verilated::timeInc(1);
        top->clk = 1;
        top->eval();
        Verilated::timeInc(1);
        top->clk = 0;
    }
    top->rst = 0;
    while (!Verilated::gotFinish()) {
        top->clk = 1;
        top->eval();
        Verilated::timeInc(1);
        top->clk = 0;
        top->eval();
        Verilated::timeInc(1);
    }

    top->final();
    delete top;
    return 0;
}
