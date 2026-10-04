#pragma once
#include "wscore/streaming.hpp"

class TestSeparator final : public ws::LiveSeparator {
    ws::LiveBlock previous{};
    bool pending = false;
public:
    void reset() override { previous = {}; pending = false; }
    bool process(const ws::LiveBlock& input, ws::LiveStems& output) override {
        output = {};
        for (unsigned i = 0; i < ws::liveHop; ++i) {
            float center = (previous[i].l + previous[i].r) * .5f;
            output[2][i] = {center, center};
            output[3][i] = {previous[i].l - center, previous[i].r - center};
        }
        previous = input;
        bool valid = pending; pending = true; return valid;
    }
};
