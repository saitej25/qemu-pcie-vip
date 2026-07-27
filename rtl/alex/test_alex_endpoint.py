import cocotb
from cocotb.clock import Clock
from cocotb.triggers import RisingEdge, Timer

from tlp_bfm import memory_read, memory_write


async def send_tlp(dut, tlp):
    dut.rx_req_tlp_data.value = tlp.data
    dut.rx_req_tlp_hdr.value = tlp.header
    dut.rx_req_tlp_sop.value = 1
    dut.rx_req_tlp_eop.value = 1
    dut.rx_req_tlp_valid.value = 1
    while not dut.rx_req_tlp_ready.value:
        await RisingEdge(dut.clk)
    await RisingEdge(dut.clk)
    dut.rx_req_tlp_valid.value = 0
    dut.rx_req_tlp_sop.value = 0
    dut.rx_req_tlp_eop.value = 0


@cocotb.test()
async def test_bar_tlp_read_write(dut):
    cocotb.start_soon(Clock(dut.clk, 10, units="ns").start())
    dut.rst.value = 1
    dut.tx_cpl_tlp_ready.value = 1
    await Timer(40, units="ns")
    dut.rst.value = 0
    await RisingEdge(dut.clk)

    await send_tlp(dut, memory_read(0x0, requester_id=0, tag=1))
    for _ in range(20):
        await RisingEdge(dut.clk)
        if dut.tx_cpl_tlp_valid.value:
            assert int(dut.tx_cpl_tlp_data.value) & 0xffffffff == 1
            break
    else:
        raise AssertionError("CAP read completion not observed")

    await send_tlp(dut, memory_write(0x14, 1, requester_id=0, tag=2))
    await Timer(50, units="ns")
    await send_tlp(dut, memory_read(0x1c, requester_id=0, tag=3))
    for _ in range(20):
        await RisingEdge(dut.clk)
        if dut.tx_cpl_tlp_valid.value:
            assert int(dut.tx_cpl_tlp_data.value) & 0xffffffff == 1
            return
    raise AssertionError("CSTS read completion not observed")
