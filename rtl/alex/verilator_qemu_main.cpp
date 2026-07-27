// Full QEMU -> Mini-ICS -> Verilator -> Alex AXI-Lite harness.
#include "Valex_qemu_verilator_top.h"
#include "verilated.h"
#include "mini_ics/protocol.hpp"
#include "mini_ics/server.hpp"
#include <chrono>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <string>
#include <thread>
#include <vector>

double sc_time_stamp() { return 0.0; }
using mini_ics::Header;
using mini_ics::Message;
using mini_ics::MessageType;
using mini_ics::MiniIcsServer;
using mini_ics::StatusCode;

namespace {
class Harness {
public:
    Harness(Valex_qemu_verilator_top *top) : top_(top) {}
    void Tick() {
        top_->clk=0; top_->eval(); top_->clk=1; top_->eval(); top_->clk=0;
        top_->eval(); ++sim_time_;
        if (top_->doorbell_pulse) doorbell_seen_=true;
    }
    bool Mmio(bool write, uint64_t address, uint32_t length, uint64_t data,
              uint64_t *result, uint32_t *status) {
        top_->mmio_req_write=write; top_->mmio_req_addr=address;
        top_->mmio_req_len=length; top_->mmio_req_data=data;
        top_->mmio_req_valid=0;
        for (int i=0; i<100 && !top_->mmio_req_ready; ++i) Tick();
        if (!top_->mmio_req_ready) return false;
        top_->mmio_req_valid=1; Tick(); top_->mmio_req_valid=0;
        for (int i=0; i<200 && !top_->mmio_rsp_valid; ++i) Tick();
        if (!top_->mmio_rsp_valid) return false;
        *result=top_->mmio_rsp_data; *status=top_->mmio_rsp_status; Tick();
        return true;
    }
    void Reset() { top_->rst=1; for (int i=0;i<4;++i) Tick(); top_->rst=0; }
    void InitDmaPorts() {
        top_->dma_read_desc_valid = 0;
        top_->dma_read_desc_pcie_addr = 0;
        top_->dma_read_desc_ram_addr = 0;
        top_->dma_read_desc_len = 0;
        top_->dma_read_desc_tag = 0;
        top_->dma_write_desc_valid = 0;
        top_->dma_write_desc_pcie_addr = 0;
        top_->dma_write_desc_ram_addr = 0;
        top_->dma_write_desc_len = 0;
        top_->dma_write_desc_tag = 0;
        top_->dma_rd_req_ready = 1;
        top_->dma_wr_req_ready = 1;
        top_->dma_rx_cpl_valid = 0;
        top_->dma_rx_cpl_sop = 0;
        top_->dma_rx_cpl_eop = 0;
        top_->dma_rx_cpl_error = 0;
        top_->dma_rx_cpl_hdr_lo = top_->dma_rx_cpl_hdr_hi = 0;
        top_->dma_rx_cpl_data0 = top_->dma_rx_cpl_data1 = 0;
        top_->dma_rx_cpl_data2 = top_->dma_rx_cpl_data3 = 0;
    }
    void SendCompletion(uint8_t tag, uint16_t byte_count, uint8_t lower_addr,
                        const std::vector<uint8_t>& data, uint32_t offset,
                        uint32_t length, bool error) {
        uint64_t hdr_lo = 0, hdr_hi = 0;
        hdr_hi |= uint64_t(0x4a) << 56; // fmt=3DW+data, type=Completion
        hdr_hi |= uint64_t(length / 4) << 32;
        hdr_hi |= uint64_t(byte_count) & 0xfff;
        hdr_lo |= uint64_t(tag) << 40;
        hdr_lo |= uint64_t(lower_addr & 0x7f) << 32;
        if (error) hdr_hi |= uint64_t(1) << 13;
        top_->dma_rx_cpl_hdr_hi = hdr_hi;
        top_->dma_rx_cpl_hdr_lo = hdr_lo;
        uint64_t words[4] = {0, 0, 0, 0};
        for (uint32_t i = 0; i < length && i < 32; ++i)
            words[i / 8] |= uint64_t(data[offset + i]) << (8 * (i % 8));
        top_->dma_rx_cpl_data0 = words[0]; top_->dma_rx_cpl_data1 = words[1];
        top_->dma_rx_cpl_data2 = words[2]; top_->dma_rx_cpl_data3 = words[3];
        top_->dma_rx_cpl_valid = 1;
        top_->dma_rx_cpl_sop = 1;
        top_->dma_rx_cpl_eop = 1;
        Tick();
        top_->dma_rx_cpl_valid = 0;
        top_->dma_rx_cpl_sop = 0;
        top_->dma_rx_cpl_eop = 0;
    }
    bool DmaRead(MiniIcsServer& server, uint64_t address, uint16_t ram_addr,
                 uint16_t length, uint8_t tag, uint64_t sim_time) {
        top_->dma_read_desc_pcie_addr = address;
        top_->dma_read_desc_ram_addr = ram_addr;
        top_->dma_read_desc_len = length;
        top_->dma_read_desc_tag = tag;
        top_->dma_read_desc_valid = 1;
        bool accepted = false;
        for (int i = 0; i < 1000; ++i) {
            Tick();
            if (top_->dma_read_desc_ready) { accepted = true; break; }
        }
        top_->dma_read_desc_valid = 0;
        if (!accepted) return false;
        for (int i = 0; i < 2000; ++i) {
            Tick();
            if (top_->dma_rd_req_valid) {
                const uint16_t req_len = top_->dma_rd_req_len;
                auto rsp = server.SendRequestToHost(MessageType::kDmaReadReq,
                                                     top_->dma_rd_req_addr, 0, {},
                                                     req_len, sim_time, true);
                std::vector<uint8_t> payload = rsp ? rsp->payload : std::vector<uint8_t>{};
                bool error = !rsp || rsp->header.Status() != StatusCode::kSuccess;
                uint32_t sent = 0;
                while (!error && sent < req_len) {
                    uint32_t chunk = std::min<uint32_t>(32, req_len - sent);
                    SendCompletion(tag, req_len - sent,
                                   uint8_t((top_->dma_rd_req_addr + sent) & 0x7f),
                                   payload, sent, chunk, false);
                    sent += chunk;
                }
                if (error) SendCompletion(tag, 0, 0, {}, 0, 4, true);
            }
            if (top_->dma_read_status_valid) return top_->dma_read_status_error == 0;
        }
        return false;
    }
    bool DmaWrite(MiniIcsServer& server, uint64_t address, uint16_t ram_addr,
                  uint16_t length, uint8_t tag, uint64_t sim_time) {
        top_->dma_write_desc_pcie_addr = address;
        top_->dma_write_desc_ram_addr = ram_addr;
        top_->dma_write_desc_len = length;
        top_->dma_write_desc_tag = tag;
        top_->dma_write_desc_valid = 1;
        bool accepted = false;
        for (int i = 0; i < 1000; ++i) {
            Tick();
            if (top_->dma_write_desc_ready) { accepted = true; break; }
        }
        top_->dma_write_desc_valid = 0;
        if (!accepted) return false;
        for (int i = 0; i < 2000; ++i) {
            Tick();
            if (top_->dma_wr_req_valid) {
                const uint16_t req_len = top_->dma_wr_req_len;
                std::vector<uint8_t> payload(req_len, 0);
                uint64_t words[4] = {top_->dma_wr_req_data0, top_->dma_wr_req_data1,
                                     top_->dma_wr_req_data2, top_->dma_wr_req_data3};
                for (uint32_t j = 0; j < req_len && j < 32; ++j)
                    payload[j] = uint8_t(words[j / 8] >> (8 * (j % 8)));
                auto rsp = server.SendRequestToHost(MessageType::kDmaWriteReq,
                                                     top_->dma_wr_req_addr, 0, payload,
                                                     req_len, sim_time, true);
                if (!rsp || rsp->header.Status() != StatusCode::kSuccess) return false;
            }
            if (top_->dma_write_status_valid) return top_->dma_write_status_error == 0;
        }
        return false;
    }
    uint64_t sim_time() const { return sim_time_; }
    bool doorbell_seen() const { return doorbell_seen_; }
    void clear_doorbell() { doorbell_seen_=false; }
private:
    Valex_qemu_verilator_top *top_;
    uint64_t sim_time_=0;
    bool doorbell_seen_=false;
};

uint64_t DecodeLe(const std::vector<uint8_t>& p) {
    uint64_t v=0; for (size_t i=0;i<p.size() && i<8;++i) v |= uint64_t(p[i]) << (8*i); return v;
}
std::vector<uint8_t> EncodeLe(uint64_t value, uint32_t length) {
    std::vector<uint8_t> p(length); for (uint32_t i=0;i<length;++i) p[i]=uint8_t(value>>(8*i)); return p;
}

uint16_t ReadLe16(const std::vector<uint8_t>& p, size_t off) {
    return off + 2 <= p.size() ? uint16_t(p[off]) | uint16_t(p[off+1]) << 8 : 0;
}
uint32_t ReadLe32(const std::vector<uint8_t>& p, size_t off) {
    uint32_t v = 0;
    for (size_t i = 0; i < 4 && off + i < p.size(); ++i) v |= uint32_t(p[off+i]) << (8*i);
    return v;
}
uint64_t ReadLe64(const std::vector<uint8_t>& p, size_t off) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8 && off + i < p.size(); ++i) v |= uint64_t(p[off+i]) << (8*i);
    return v;
}
void StoreLe(std::vector<uint8_t>& p, size_t off, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) p[off+i] = uint8_t(v >> (8*i));
}

bool RunGenericDma(Harness& harness, MiniIcsServer& server, uint64_t desc_base, uint64_t cpl_base,
                   uint32_t count, uint32_t tail, uint32_t* head,
                   uint64_t sim_time) {
    constexpr uint32_t kDescBytes = 32;
    constexpr uint32_t kCplBytes = 16;
    constexpr uint32_t kRamBytes = 64 * 1024;
    constexpr uint32_t kMaxTransfer = 4096;
    if (!count || count > 1024 || tail < *head || tail - *head > count)
        return false;

    for (uint32_t index = *head; index < tail; ++index) {
        const uint32_t slot = index % count;
        auto desc_rsp = server.SendRequestToHost(MessageType::kDmaReadReq,
                                                  desc_base + uint64_t(slot) * kDescBytes,
                                                  0, {}, kDescBytes, sim_time, true);
        uint32_t status = 1;
        uint32_t bytes = 0;
        uint32_t tag = 0;
        uint8_t flags = 0;
        std::vector<uint8_t> desc;
        if (desc_rsp && desc_rsp->header.Status() == StatusCode::kSuccess &&
            desc_rsp->payload.size() == kDescBytes) {
            desc = desc_rsp->payload;
            const uint64_t host_addr = ReadLe64(desc, 0);
            const uint32_t ram_addr = ReadLe32(desc, 8);
            const uint16_t length = ReadLe16(desc, 12);
            const uint8_t opcode = desc[14];
            flags = desc[15];
            tag = ReadLe32(desc, 16);
            if (length && length <= kMaxTransfer &&
                uint64_t(ram_addr) + length <= kRamBytes &&
                (opcode == 0 || opcode == 1)) {
                if (opcode == 0) {
                    if (harness.DmaRead(server, host_addr, ram_addr, length,
                                        uint8_t(tag), sim_time)) {
                        status = 0;
                        bytes = length;
                    }
                } else {
                    if (harness.DmaWrite(server, host_addr, ram_addr, length,
                                         uint8_t(tag), sim_time)) {
                        status = 0;
                        bytes = length;
                    }
                }
            } else {
                status = 6;
            }
        } else if (desc_rsp) {
            status = static_cast<uint32_t>(desc_rsp->header.status);
        }

        std::vector<uint8_t> cpl(kCplBytes, 0);
        StoreLe(cpl, 0, tag, 4);
        StoreLe(cpl, 4, status, 2);
        StoreLe(cpl, 8, bytes, 4);
        StoreLe(cpl, 12, index, 4);
        auto cpl_rsp = server.SendRequestToHost(MessageType::kDmaWriteReq,
                                                 cpl_base + uint64_t(slot) * kCplBytes,
                                                 0, cpl, kCplBytes, sim_time, true);
        if (!cpl_rsp || cpl_rsp->header.Status() != StatusCode::kSuccess)
            return false;
        if (flags & 1) {
            server.SendRequestToHost(MessageType::kMsiX, 0, 0, {}, 0, sim_time, false);
        }
        if (status != 0) return false;
    }
    *head = tail;
    return true;
}
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    const std::string socket_path=argc>1 ? argv[1] : "/tmp/pcie-vip-verilator.sock";
    MiniIcsServer server(socket_path,50,5000);
    if (!server.Start()) return 1;
    auto *top=new Valex_qemu_verilator_top;
    top->mmio_req_valid=0; top->rst=1;
    Harness harness(top); harness.InitDmaPorts(); harness.Reset();
    uint64_t asq=0, acq=0, desc_base=0, cpl_base=0;
    uint32_t desc_count=0, desc_head=0, dma_control=0, dma_status=0;
    bool stop=false;
    while (!stop && !Verilated::gotFinish()) {
        auto inbound=server.PollInbound(0);
        if (!inbound) { harness.Tick(); std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
        const Message &msg=*inbound; const auto type=msg.header.Type();
        if (type==MessageType::kResetReq) {
            harness.Reset();
            Header rsp=mini_ics::MakeHeader(MessageType::kResetRsp,msg.header.txn_id,0,0,
                StatusCode::kSuccess,harness.sim_time(),0,mini_ics::kFlagIsResponse);
            server.SendResponse(rsp,{});
        } else if (type==MessageType::kMmioReadReq || type==MessageType::kMmioWriteReq) {
            const uint32_t length=msg.header.transfer_len ? msg.header.transfer_len : uint32_t(msg.payload.size());
            uint64_t result=0; uint32_t status=1;
            bool ok;
            if (type == MessageType::kMmioReadReq && msg.header.address == 0x1058) {
                result = dma_status;
                status = 0;
                ok = true;
            } else {
                ok=harness.Mmio(type==MessageType::kMmioWriteReq,msg.header.address,length,
                                DecodeLe(msg.payload),&result,&status);
            }
            if (!ok) status=5;
            if (type==MessageType::kMmioWriteReq) {
                if (msg.header.address==0x28) asq=DecodeLe(msg.payload);
                if (msg.header.address==0x30) acq=DecodeLe(msg.payload);
                if (msg.header.address==0x1040) desc_base = (desc_base & 0xffffffff00000000ULL) | DecodeLe(msg.payload);
                if (msg.header.address==0x1044) desc_base = (desc_base & 0xffffffffULL) | (DecodeLe(msg.payload) << 32);
                if (msg.header.address==0x1048) cpl_base = (cpl_base & 0xffffffff00000000ULL) | DecodeLe(msg.payload);
                if (msg.header.address==0x104c) cpl_base = (cpl_base & 0xffffffffULL) | (DecodeLe(msg.payload) << 32);
                if (msg.header.address==0x1050) desc_count = uint32_t(DecodeLe(msg.payload));
                if (msg.header.address==0x105c) {
                    dma_control = uint32_t(DecodeLe(msg.payload));
                    if (dma_control & 2) dma_status = 0;
                }
                Header rsp=mini_ics::MakeHeader(MessageType::kMmioWriteRsp,msg.header.txn_id,
                    msg.header.address,0,status==0 ? StatusCode::kSuccess : StatusCode::kErrorGeneric,
                    harness.sim_time(),0,mini_ics::kFlagIsResponse);
                server.SendResponse(rsp,{});
                if (msg.header.address==0x1054 && (dma_control & 1) && desc_count) {
                    const uint32_t tail = uint32_t(DecodeLe(msg.payload));
                    if (!RunGenericDma(harness, server, desc_base, cpl_base, desc_count, tail,
                                       &desc_head, harness.sim_time()))
                        dma_status = 1;
                    else
                        dma_status = 0;
                }
                if (msg.header.address==0x1000 && harness.doorbell_seen()) {
                    harness.clear_doorbell();
                    auto read=server.SendRequestToHost(MessageType::kDmaReadReq,asq,0,{},64,harness.sim_time(),true);
                    if (read) {
                        std::vector<uint8_t> completion(16);
                        for (size_t i=0;i<completion.size();++i) completion[i]=uint8_t(0xc0+i);
                        server.SendRequestToHost(MessageType::kDmaWriteReq,acq,0,completion,16,harness.sim_time(),true);
                        server.SendRequestToHost(MessageType::kMsiX,0,0,{},0,harness.sim_time(),false);
                    }
                }
            } else {
                auto payload=status==0 ? EncodeLe(result,length) : std::vector<uint8_t>{};
                Header rsp=mini_ics::MakeHeader(MessageType::kMmioReadRsp,msg.header.txn_id,msg.header.address,0,
                    status==0 ? StatusCode::kSuccess : StatusCode::kErrorGeneric,harness.sim_time(),
                    uint32_t(payload.size()),mini_ics::kFlagIsResponse,uint32_t(payload.size()));
                server.SendResponse(rsp,payload);
            }
        } else if (type==MessageType::kShutdown) stop=true;
    }
    server.Stop(); top->final(); delete top; return 0;
}
