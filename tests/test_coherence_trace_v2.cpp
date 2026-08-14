#include "coherence_trace_v2.h"

#include "coherence_protocol_v2.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace cxlmemsim;
using namespace cxlmemsim::protocol_v2;

namespace {

std::atomic<int> failures{};

#define CHECK(condition)                                                                                               \
    do {                                                                                                               \
        if (!(condition)) {                                                                                            \
            std::cerr << __func__ << ':' << __LINE__ << ": CHECK failed: " #condition << '\n';                         \
            failures.fetch_add(1, std::memory_order_relaxed);                                                          \
        }                                                                                                              \
    } while (false)

CoherenceFrame frame(Opcode opcode_value, std::uint16_t src, std::uint16_t dst) {
    auto result = initializeFrame(opcode_value);
    setSrcHost(result, src);
    setDstHost(result, dst);
    setSessionId(result, 11);
    setRequestId(result, 22);
    setSnoopId(result, 33);
    setAddress(result, 0x4000);
    setEpoch(result, 44);
    return result;
}

void testSchemaAndCounters() {
    const auto path =
        std::filesystem::temp_directory_path() / ("cxlmemsim-coherence-trace-" + std::to_string(getpid()) + ".jsonl");
    {
        CoherenceTraceV2 trace(path);
        auto registration = frame(Opcode::Register, 0, kServerHost);
        auto getm = frame(Opcode::Getm, 0, kServerHost);
        auto snoop = frame(Opcode::SnpDataInv, kServerHost, 0);
        auto ack = frame(Opcode::SnoopAck, 0, kServerHost);
        setAckStrength(ack, AckStrength::MODEL);
        setPayloadLength(ack, kLineSize);

        trace.record({"registration", registration, false});
        trace.record({"request", getm, false});
        trace.record({"snoop_send", snoop, false});
        trace.record({"snoop_ack", ack, true});
        trace.record({"dirty_completion", ack, true});

        const auto snapshot = trace.snapshot();
        CHECK(snapshot.registrations == 1);
        CHECK(snapshot.getm == 1);
        CHECK(snapshot.snp_data_inv == 1);
        CHECK(snapshot.model_acks == 1);
        CHECK(snapshot.dirty_data_completions == 1);
        const auto json = trace.snapshotJson();
        CHECK(json.find("\"server_copy_failures\":0") != std::string::npos);
    }

    std::ifstream input(path);
    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);)
        lines.push_back(std::move(line));
    CHECK(lines.size() == 5);
    static constexpr const char *keys[] = {
        "schema_version", "event",       "monotonic_ns", "opcode",       "src_host",
        "dst_host",       "session_id",  "request_id",   "snoop_id",     "line_address",
        "epoch",          "payload_len", "status",       "ack_strength", "dirty_data",
    };
    for (const auto &line : lines) {
        for (const auto *key : keys)
            CHECK(line.find('"' + std::string(key) + "\":") != std::string::npos);
    }
    std::filesystem::remove(path);
}

} // namespace

int main() {
    testSchemaAndCounters();
    return failures.load(std::memory_order_relaxed) == 0 ? 0 : 1;
}
