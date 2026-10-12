// The eye swapchain pairs' rebuild for a new render resolution
// (platform/include/vr/openxr_eye_resize.h), driven through a model of the
// backends' ResizeWritablePair and the integration's pacing: two pairs taking
// turns, retired pairs kept alive for a few cycles, a runtime that refuses a
// swapchain past a cap, and pipelined pacing that only lets a pair be rebuilt
// on a cycle with no packet in flight.
#include "vr/openxr_eye_resize.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
void Check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "vr eye resize regression failed: %s\n", what);
        std::abort();
    }
}

constexpr uint32_t kRetiredCycles = 8; // kOpenXRRetiredSwapchainCycles

struct Model {
    // Each pair is its eye size (both eyes alike here); `written` takes turns.
    uint32_t pairs[2] = {100, 100};
    uint32_t written = 0;
    uint32_t target = 100;
    uint32_t requested = 100;
    // Retired pairs' cycles left.
    std::vector<uint32_t> retired;
    // How many pairs the runtime lets the session hold at once.
    uint32_t pair_cap = 3;
    // The fixed rules (the gate, pipelining paused while resizing) or the old ones.
    bool fixed = true;
    bool pipelined = true;
    bool drain = false;
    PortVr::OpenXREyeResizeGate gate;
    uint32_t attempts = 0;
    uint32_t refusals = 0;
    uint32_t size_switches = 0;
    uint32_t last_packet = 100;

    void Request(uint32_t size) {
        if (size != requested) {
            requested = size;
            target = size;
            gate.Requested();
            // A new scale drains the packet in flight for a cycle.
            drain = true;
        }
    }

    bool Resizing() const { return (pairs[0] != target || pairs[1] != target) && gate.Pending(); }

    // One cycle: reap, maybe rebuild the written pair, arm a packet on it, swap.
    void Cycle() {
        for (auto it = retired.begin(); it != retired.end();) {
            if (--*it == 0) {
                it = retired.erase(it);
            } else {
                ++it;
            }
        }
        // ResizeWritablePair runs only with no packet in flight.
        const bool nothing_in_flight = !pipelined || drain || (fixed && Resizing());
        drain = false;
        uint32_t& pair = pairs[written];
        if (nothing_in_flight && pair != target && (!fixed || gate.MayRebuild(!retired.empty()))) {
            ++attempts;
            const uint32_t held = 2 + static_cast<uint32_t>(retired.size());
            if (held + 1 > pair_cap) {
                ++refusals;
                gate.Refused();
                target = pair;
            } else {
                retired.push_back(kRetiredCycles);
                pair = target;
            }
        }
        if (pair != last_packet) {
            ++size_switches;
            last_packet = pair;
        }
        written ^= 1u;
    }

    void Run(uint32_t cycles) {
        for (uint32_t i = 0; i < cycles; ++i) {
            Cycle();
        }
    }

    bool Settled() const { return pairs[0] == pairs[1] && pairs[0] == target; }
};
} // namespace

int main() {
    {
        // The old pipelined pacing: one drained cycle per change rebuilds one pair; the
        // other keeps its size, and the packets alternate sizes for good.
        Model m;
        m.fixed = false;
        m.Request(80);
        m.Run(400);
        Check(!m.Settled() && m.size_switches > 390, "the model reproduces the endless size swap");
    }
    {
        // One change: the first pair at once, the second once the first's old pair is gone.
        Model m;
        m.Request(80);
        m.Run(40);
        Check(m.Settled() && m.pairs[0] == 80, "a new render resolution reaches both pairs while pipelined");
        Check(m.refusals == 0 && m.attempts == 2, "each pair is rebuilt once, never refused");
        Check(m.size_switches <= kRetiredCycles + 2, "the packets alternate sizes only until the second pair follows");
        Check(!m.Resizing(), "pipelining resumes once both pairs have the size");
    }
    {
        // Without pipelining too.
        Model m;
        m.pipelined = false;
        m.Request(80);
        m.Run(40);
        Check(m.Settled() && m.pairs[0] == 80 && m.refusals == 0, "a new render resolution reaches both pairs");
    }
    {
        // A slider dragged a step a frame: never more than three pairs held, so never refused.
        Model m;
        for (uint32_t size : {95u, 90u, 85u, 80u, 75u, 70u, 65u}) {
            m.Request(size);
            m.Run(1);
        }
        m.Run(60);
        Check(m.refusals == 0, "a drag waits for the retired pairs instead of running into the runtime's cap");
        Check(m.Settled() && m.pairs[0] == 65, "after a drag both pairs reach the last size asked for");
    }
    {
        // A runtime that cannot hold a third pair: the request is refused once and kept refused.
        Model m;
        m.pair_cap = 2;
        m.Request(80);
        m.Run(100);
        Check(m.Settled() && m.pairs[0] == 100, "a size the runtime refuses leaves both pairs as they were");
        Check(m.refusals == 1 && m.size_switches == 0, "a refused size is not asked for again");
        m.Request(70);
        m.Run(10);
        Check(m.refusals == 2, "a new request is tried again");
    }
    {
        // Refused for the second pair after the first was rebuilt (memory, say): one try to bring the
        // first back, then nothing more whatever the outcome, and pipelining resumes.
        Model m;
        m.Request(80);
        m.Run(1);
        m.pair_cap = 2;
        m.Run(200);
        Check(m.attempts == 3 && m.refusals == 2, "refusals in both directions stop the rebuilds");
        Check(!m.Resizing(), "pipelining resumes after the rebuilds stop");
    }
    std::puts("port_vr_eye_resize_tests: ok");
    return 0;
}
