#pragma once
#include "wsbridge.h"
#include "mactap/platform.hpp"
#include "wscore/engine.hpp"
#include "wscore/jobs.hpp"
#include "wscore/analysis.hpp"
#include "wscore/resample.hpp"
#include <deque>
#include <filesystem>
namespace ws {
class Sing;
class Session {
    struct Item { std::shared_ptr<Clip> clip; std::shared_ptr<StemBank> stems; std::string title; };
    Tap tap;
    Output output;
    Engine engine;
    Separator separator;
    std::string loadedModel;
    std::mutex mutex;
    std::deque<Item> history;
    Item active;
    std::string message = "Choose an audio app to start buffering.";
    std::unique_ptr<CaptureStore> capture;
    std::thread captureThread;
    std::atomic<bool> captureStop{true};
    std::atomic<uint64_t> generation{0};
    std::atomic<bool> busy{false};
    std::atomic<float> progress{0};
    std::unique_ptr<Sing> sing;
    Jobs jobs;
    uint32_t source = 0;
    std::string sourceName, sourceBundle, sixPath, fourPath;
    uint64_t nextID = 1;
    bool pauseSource = false, counting = false, outputStarted = false;
    double gridOffset = 0;
    std::array<float, 6> gain{1,1,1,1,1,1};
    std::array<bool, 6> muted{}, soloed{};
    std::filesystem::path tempDir;
    void startOutput();
    void submitClip(Stereo audio, unsigned sampleRate, std::string title);
    void separate(Item item, uint64_t ticket, std::string path);
    void activate(const Item& item);
public:
    Session();
    ~Session();
    void error(const char* text);
    int sources(WSSource*, int);
    int arm(uint32_t);
    void disarm();
    void grab(double);
    void demo();
    void status(WSStatus*);
    void play(bool);
    void speed(float);
    void pitch(float);
    void loop(double, double, bool);
    void grid(double);
    void stem(int, float, bool, bool);
    void trainer(bool, float, float, int);
    void countIn(bool);
    int waveform(float*, int);
    int beats(double*, int);
    int histories(WSHistory*, int);
    void select(uint64_t);
    void model(const char*, const char*);
    void setSing(bool);
    void vocals(float);
    void setPause(bool value) { pauseSource = value; }
};
}
