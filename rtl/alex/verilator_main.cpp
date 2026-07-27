// Portable Verilator 4.x entry point for alex_verilator_tb.
#include "Valex_verilator_tb.h"
#include "verilated.h"

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    auto *top = new Valex_verilator_tb;

    while (!Verilated::gotFinish()) {
        top->eval();
        Verilated::timeInc(1);
    }

    top->final();
    delete top;
    return 0;
}
