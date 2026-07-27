VERILOG_PCIE ?= ../../third_party/verilog-pcie/rtl
.PHONY: questa_compile questa_qemu_compile verilator_alex
questa_compile:
	@mkdir -p build/questa
	vlib build/questa/work
	vlog -sv -work build/questa/work \
		$(VERILOG_PCIE)/pcie_axil_master_minimal.v \
		axi_lite_slave_model.sv \
		pcie_vip_alex_endpoint.sv alex_endpoint_tb.sv

questa_qemu_compile:
	@mkdir -p build/qemu_questa
	vlib build/qemu_questa/work
	vlog -sv -work build/qemu_questa/work \
		../../mini-ics/dpi/mini_ics_dpi_pkg.sv \
		$(VERILOG_PCIE)/pcie_axil_master_minimal.v \
		axi_lite_slave_model.sv \
		pcie_vip_alex_endpoint.sv mini_ics_alex_tb.sv

verilator_alex:
	./run_verilator_alex.sh
