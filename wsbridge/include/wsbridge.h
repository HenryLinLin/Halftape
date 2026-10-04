#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct WS WS;
typedef struct { uint32_t id; char name[128]; char bundle[256]; int active; } WSSource;
typedef struct {
    double buffered, duration, position, bpm, loop_a, loop_b, progress, sing_latency;
    float speed, pitch, vocal_removal;
    int playing, busy, sing, quality, key, history_count;
    uint64_t clip_id, loops, capture_drops, underruns;
    char message[512];
    int capturing, stems_ready;
    uint32_t capture_source;
    char capture_name[128];
} WSStatus;
typedef struct { uint64_t id; double duration; char title[160]; } WSHistory;
WS* ws_create(void);
void ws_destroy(WS*);
int ws_sources(WS*, WSSource* out, int capacity);
int ws_arm(WS*, uint32_t source);
void ws_disarm(WS*);
void ws_stop_capture(WS*);
void ws_grab(WS*, double seconds);
void ws_demo(WS*);
void ws_status(WS*, WSStatus*);
void ws_play(WS*, int playing);
void ws_speed(WS*, float speed);
void ws_pitch(WS*, float semitones);
void ws_loop(WS*, double a, double b, int snap);
void ws_grid_offset(WS*, double seconds);
void ws_stem(WS*, int index, float gain, int mute, int solo);
void ws_trainer(WS*, int enabled, float start, float increment, int every);
void ws_count_in(WS*, int enabled);
int ws_waveform(WS*, float* out, int capacity);
int ws_beats(WS*, double* out, int capacity);
int ws_history(WS*, WSHistory* out, int capacity);
void ws_select(WS*, uint64_t id);
void ws_set_model(WS*, const char* six_path, const char* four_path);
void ws_sing(WS*, int enabled);
void ws_vocals(WS*, float removal);
void ws_pause_source(WS*, int enabled);
#ifdef __cplusplus
}
#endif
