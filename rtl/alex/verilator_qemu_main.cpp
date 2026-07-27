// Full QEMU -> Mini-ICS -> Verilator -> Alex AXI-Lite harness.
#include "Valex_qemu_verilator_top.h"
#include "verilated.h"
#include "mini_ics/protocol.hpp"
#include "mini_ics/server.hpp"
#include <chrono>
#include <cstdint>
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
}

int main(int argc, char **argv) {
    Verilated::commandArgs(argc, argv);
    const std::string socket_path=argc>1 ? argv[1] : "/tmp/pcie-vip-verilator.sock";
    MiniIcsServer server(socket_path,50,5000);
    if (!server.Start()) return 1;
    auto *top=new Valex_qemu_verilator_top;
    top->mmio_req_valid=0; top->rst=1;
    Harness harness(top); harness.Reset();
    uint64_t asq=0, acq=0; bool stop=false;
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
            bool ok=harness.Mmio(type==MessageType::kMmioWriteReq,msg.header.address,length,
                                 DecodeLe(msg.payload),&result,&status);
            if (!ok) status=5;
            if (type==MessageType::kMmioWriteReq) {
                if (msg.header.address==0x28) asq=DecodeLe(msg.payload);
                if (msg.header.address==0x30) acq=DecodeLe(msg.payload);
                Header rsp=mini_ics::MakeHeader(MessageType::kMmioWriteRsp,msg.header.txn_id,
                    msg.header.address,0,status==0 ? StatusCode::kSuccess : StatusCode::kErrorGeneric,
                    harness.sim_time(),0,mini_ics::kFlagIsResponse);
                server.SendResponse(rsp,{});
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
