#include "session.hpp"
#include <exception>
#include <memory>

struct WS { ws::Session session; };

namespace {
template<class F>
void invoke(WS* handle, F&& action) noexcept {
    if (!handle) return;
    try {
        action(handle->session);
    } catch (const std::exception& e) {
        try { handle->session.error(e.what()); } catch (...) {}
    } catch (...) {
        try { handle->session.error("Unknown error."); } catch (...) {}
    }
}
template<class F>
int invokeInt(WS* handle, F&& action) noexcept {
    if (!handle) return 0;
    try {
        return action(handle->session);
    } catch (const std::exception& e) {
        try { handle->session.error(e.what()); } catch (...) {}
    } catch (...) {
        try { handle->session.error("Unknown error."); } catch (...) {}
    }
    return 0;
}
}

extern "C" {
WS* ws_create(void) {
    try { return new WS; } catch (...) { return nullptr; }
}
void ws_destroy(WS* h) { try { delete h; } catch (...) {} }
int ws_sources(WS* h, WSSource* out, int capacity) {
    return invokeInt(h, [&](ws::Session& s) { return out ? s.sources(out, capacity) : 0; });
}
int ws_arm(WS* h, uint32_t source) { return invokeInt(h, [&](ws::Session& s) { return s.arm(source); }); }
void ws_disarm(WS* h) { invoke(h, [](ws::Session& s) { s.disarm(); }); }
void ws_stop_capture(WS* h) { invoke(h, [](ws::Session& s) { s.stopCapture(); }); }
void ws_grab(WS* h, double seconds) { invoke(h, [&](ws::Session& s) { s.grab(seconds); }); }
void ws_demo(WS* h) { invoke(h, [](ws::Session& s) { s.demo(); }); }
void ws_status(WS* h, WSStatus* out) { invoke(h, [&](ws::Session& s) { if (out) s.status(out); }); }
void ws_play(WS* h, int playing) { invoke(h, [&](ws::Session& s) { s.play(playing != 0); }); }
void ws_speed(WS* h, float speed) { invoke(h, [&](ws::Session& s) { s.speed(speed); }); }
void ws_pitch(WS* h, float semitones) { invoke(h, [&](ws::Session& s) { s.pitch(semitones); }); }
void ws_loop(WS* h, double a, double b, int snap) { invoke(h, [&](ws::Session& s) { s.loop(a, b, snap != 0); }); }
void ws_grid_offset(WS* h, double seconds) { invoke(h, [&](ws::Session& s) { s.grid(seconds); }); }
void ws_stem(WS* h, int index, float gain, int mute, int solo) {
    invoke(h, [&](ws::Session& s) { s.stem(index, gain, mute != 0, solo != 0); });
}
void ws_trainer(WS* h, int enabled, float start, float increment, int every) {
    invoke(h, [&](ws::Session& s) { s.trainer(enabled != 0, start, increment, every); });
}
void ws_count_in(WS* h, int enabled) { invoke(h, [&](ws::Session& s) { s.countIn(enabled != 0); }); }
int ws_waveform(WS* h, float* out, int capacity) {
    return invokeInt(h, [&](ws::Session& s) { return out ? s.waveform(out, capacity) : 0; });
}
int ws_beats(WS* h, double* out, int capacity) {
    return invokeInt(h, [&](ws::Session& s) { return out ? s.beats(out, capacity) : 0; });
}
int ws_history(WS* h, WSHistory* out, int capacity) {
    return invokeInt(h, [&](ws::Session& s) { return out ? s.histories(out, capacity) : 0; });
}
void ws_select(WS* h, uint64_t id) { invoke(h, [&](ws::Session& s) { s.select(id); }); }
void ws_set_model(WS* h, const char* sixPath, const char* fourPath) {
    invoke(h, [&](ws::Session& s) { s.model(sixPath, fourPath); });
}
void ws_set_sing_model(WS* h, const char* path) {
    invoke(h, [&](ws::Session& s) { s.singModel(path); });
}
void ws_sing(WS* h, int enabled) { invoke(h, [&](ws::Session& s) { s.setSing(enabled != 0); }); }
void ws_vocals(WS* h, float removal) { invoke(h, [&](ws::Session& s) { s.vocals(removal); }); }
void ws_pause_source(WS* h, int enabled) { invoke(h, [&](ws::Session& s) { s.setPause(enabled != 0); }); }
void ws_seek(WS* h, double seconds) { invoke(h, [&](ws::Session& s) { s.seek(seconds); }); }
void ws_loop_enabled(WS* h, int enabled) { invoke(h, [&](ws::Session& s) { s.loopEnabled(enabled != 0); }); }
void ws_sing_pitch(WS* h, float semitones) { invoke(h, [&](ws::Session& s) { s.singPitch(semitones); }); }
}
