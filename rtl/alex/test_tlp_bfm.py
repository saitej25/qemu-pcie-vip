from tlp_bfm import memory_read, memory_write


def test_memory_read_header():
    tlp = memory_read(0x14, requester_id=0x1234, tag=7)
    assert not tlp.is_write
    assert tlp.address == 0x14
    assert tlp.requester_id == 0x1234
    assert tlp.first_be == 0xf


def test_memory_write_header_and_data():
    tlp = memory_write(0x14, 0xdead, requester_id=1, tag=2)
    assert tlp.is_write
    assert tlp.address == 0x14
    assert tlp.data == 0xdead
