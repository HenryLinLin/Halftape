#include "session.hpp"
#include "wscore/sing.hpp"
#include "mactap/source_control.hpp"
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <unistd.h>
#include <pthread.h>
namespace ws {
template<size_t N> static void text(char (&out)[N], const std::string& value) { std::snprintf(out, N, "%s", value.c_str()); }
static double sane(double x, double lo, double hi) { return std::clamp(std::isfinite(x) ? x : lo, lo, hi); }
Session::Session() : jobs([this] { separator.unload(); loadedModel.clear(); }) {
    std::string base = (std::filesystem::temp_directory_path() / "halftape-XXXXXX").string();
    if (auto* p = mkdtemp(base.data())) tempDir = p;
}
Session::~Session() {
    ++generation; disarm(); output.stop(); jobs.stop();
    std::error_code ec; if (!tempDir.empty()) std::filesystem::remove_all(tempDir, ec);
}
void Session::error(const char* value) { std::lock_guard lock(mutex); message = value ? value : "Unknown error"; }
void Session::startOutput() {
    if (outputStarted || std::getenv("HALFTAPE_HEADLESS")) return;
    output.start([](void* context, float* l, float* r, unsigned n) noexcept {
        auto& self = *static_cast<Session*>(context);
        if (self.sing) self.sing->render(l, r, n); else self.engine.render(l, r, n);
    }, this, bool(sing));
    outputStarted = true;
}
int Session::sources(WSSource* out, int capacity) {
    auto list = ws::sources(); int count = std::min(int(list.size()), std::max(0, capacity));
    for (int i = 0; i < count; ++i) { out[i] = {}; out[i].id = list[i].id; out[i].active = list[i].active; text(out[i].name, list[i].name); text(out[i].bundle, list[i].bundle); }
    return count;
}
void Session::stopCapture() {
    captureStop.store(true); if (captureThread.joinable()) captureThread.join();
    tap.stop();
    if (sing) { output.stop(); outputStarted = false; sing.reset(); }
    if (capture) error("Capture stopped. The buffered audio is still available to grab.");
}
void Session::disarm() {
    stopCapture();
    capture.reset(); source = 0;
    sourceName.clear(); sourceBundle.clear();
}
int Session::arm(uint32_t id) {
    disarm();
    auto available = ws::sources();
    auto it = std::find_if(available.begin(), available.end(), [id](auto& s) { return s.id == id; });
    if (it == available.end()) throw std::runtime_error("This audio app has closed. Refresh the source list.");
    double sr = tap.start(id, false);
    source = id; sourceName = it->name; sourceBundle = it->bundle;
    capture = std::make_unique<CaptureStore>(unsigned(sr));
    captureStop.store(false);
    captureThread = std::thread([this] {
        std::array<Frame, 2048> frames;
        while (!captureStop.load()) {
            size_t n = 0; while (n < frames.size() && tap.queue.pop(frames[n])) ++n;
            if (n) { capture->append(frames.data(), n); if (sing) for (size_t i = 0; i < n; ++i) sing->feed(frames[i]); }
            else std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });
    error(("Capturing " + sourceName + ". Grab saves the most recent audio as a practice clip.").c_str());
    return 1;
}
void Session::activate(const Item& item) {
    active = item; gridOffset = 0;
    engine.publish(std::make_unique<Playback>(Playback{item.clip, item.stems}));
    engine.setLoop(0, std::min(8., item.clip->seconds()));
    engine.looping.store(true);
    engine.playing.store(true);
    if (counting) engine.countRequested.store(true);
}
void Session::submitClip(Stereo audio, unsigned sampleRate, std::string title) {
    auto ticket = ++generation;
    busy.store(true); progress.store(0);
    std::string path = sixPath;
    jobs.submit([this, ticket, audio = std::move(audio), sampleRate, title, path]() mutable {
        try {
            auto clip = std::make_shared<Clip>();
            clip->audio = resample(audio, sampleRate); trim(clip->audio);
            if (clip->audio[0].empty()) throw std::runtime_error("No audio captured. Play the source and allow Halftape in Privacy & Security → Screen & System Audio Recording. Some protected streams may be unavailable.");
            clip->id = ticket;
            ws::waveform(*clip); analyze(*clip);
            if (ticket != generation.load()) return;
            Item item{clip, std::make_shared<StemBank>(clip->audio[0].size()), title};
            if (!tempDir.empty()) {
                std::ofstream file(tempDir / (std::to_string(clip->id) + ".pcm"), std::ios::binary);
                for (auto& c : clip->audio) file.write(reinterpret_cast<const char*>(c.data()), c.size() * sizeof(float));
            }
            {
                std::lock_guard lock(mutex);
                if (ticket != generation.load()) return;
                history.push_front(item);
                if (history.size() > 20) {
                    std::error_code ec; std::filesystem::remove(tempDir / (std::to_string(history.back().clip->id) + ".pcm"), ec); history.pop_back();
                }
                activate(item);
                message = path.empty() ? "Playing the original mix. Choose a separation model to enable vocal removal." : "Separating stems. Unfinished regions play the original mix.";
            }
            separate(item, ticket, path);
        } catch (const std::exception& e) { if (ticket == generation.load()) error(e.what()); }
        if (ticket == generation.load()) busy.store(false);
    }, true);
}
void Session::separate(Item item, uint64_t ticket, std::string path) {
    if (path.empty()) return;
    if (path != loadedModel || !separator.loaded()) { separator.load(path, 6); loadedModel = path; }
    if (ticket != generation.load()) return;
    auto range = engine.loop.load();
    separateClip(separator, *item.clip, *item.stems, double(range >> 32) / rate, double(uint32_t(range)) / rate,
                 [&] { return ticket != generation.load(); }, [&](float p) { if (ticket == generation.load()) progress.store(p); });
    if (ticket == generation.load()) error("Six stems ready · guitar and piano are experimental");
}
void Session::grab(double seconds) {
    if (sing) setSing(false);
    if (!capture || capture->buffered() < .1) throw std::runtime_error("Click Start Capture and play audio in the source before grabbing a clip.");
    auto audio = capture->grab(sane(seconds, 1, 180));
    startOutput(); submitClip(std::move(audio), capture->sample_rate(), "Grabbed from " + sourceName);
    if (pauseSource) pauseSourceApp(sourceBundle);
}
void Session::demo() {
    if (sing) setSing(false);
    Stereo audio; for (auto& c : audio) c.resize(12 * rate);
    for (size_t i = 0; i < audio[0].size(); ++i) {
        double t = double(i) / rate, beat = std::fmod(t, .5);
        float x = .13 * (std::sin(t * 6.283185307 * 220) + .6 * std::sin(t * 6.283185307 * 261.6256) + .5 * std::sin(t * 6.283185307 * 329.6276));
        x += .3 * std::exp(-beat * 55) * std::sin(beat * 6.283185307 * 90);
        audio[0][i] = x; audio[1][i] = x * .9f;
    }
    startOutput(); submitClip(std::move(audio), rate, "Demo · A minor / 120 BPM");
}
void Session::status(WSStatus* s) {
    if (sing && sing->state() == 5) {
        auto reason = sing->error(); stopCapture();
        error(("Sing stopped and the source is audible again: " + reason).c_str());
    }
    *s = {}; s->key = -1;
    s->sing_model_ready = singModelReady;
    s->buffered = capture ? capture->buffered() : 0;
    s->capturing = capture && !captureStop.load();
    s->capture_source = source;
    text(s->capture_name, sourceName);
    s->busy = busy.load(); s->progress = progress.load();
    s->capture_drops = tap.dropped.load();
    s->position = engine.playhead.load(); s->playing = !sing && engine.playing.load(); s->loops = engine.loops.load();
    s->looping = engine.looping.load();
    s->speed = engine.speed.load(); s->pitch = engine.pitch.load();
    s->sing = bool(sing); s->quality = sing ? sing->quality.load() : 0;
    s->sing_latency = sing ? sing->delay() : 0; s->vocal_removal = sing ? sing->removal.load() : 1;
    if (sing) {
        s->sing_state = sing->state(); s->sing_buffered = sing->buffered();
        s->sing_position = sing->position(); s->sing_pitch = sing->pitch.load();
    }
    s->underruns = sing ? sing->underruns.load() : 0;
    auto range = engine.loop.load(); s->loop_a = double(range >> 32) / rate; s->loop_b = double(uint32_t(range)) / rate;
    std::lock_guard lock(mutex);
    engine.collect(); s->history_count = int(history.size()); text(s->message, message);
    if (active.clip) {
        s->duration = active.clip->seconds(); s->bpm = active.clip->bpm; s->key = active.clip->key; s->clip_id = active.clip->id;
        s->stems_ready = active.stems && active.stems->ready.load() > 0;
        if (active.stems && active.stems->size) {
            auto index = std::min(size_t(std::max(0., s->position) * rate) / segmentFrames, active.stems->size - 1);
            s->current_stems_ready = active.stems->blocks[index].load(std::memory_order_acquire) != nullptr;
        }
    }
}
void Session::play(bool value) {
    if (sing) {
        if (!value) return;
        setSing(false);
    }
    Item item;
    { std::lock_guard lock(mutex); item = active; }
    if (value && !item.clip) throw std::runtime_error("Grab a clip before starting Practice playback.");
    if (value) {
        startOutput();
        if (engine.playhead.load() >= item.clip->seconds()) seek(0);
        if (counting) engine.countRequested.store(true);
        if (!sixPath.empty() && !busy.load() && item.stems->ready.load() < item.stems->size) {
            auto ticket = ++generation; busy.store(true); std::string path = sixPath;
            jobs.submit([this, item, ticket, path] {
                try { separate(item, ticket, path); } catch (const std::exception& e) { if (ticket == generation.load()) error(e.what()); }
                if (ticket == generation.load()) busy.store(false);
            }, true);
        }
        error("Playing the practice clip. Pause the source app to hear only this clip.");
    }
    engine.playing.store(value);
}
void Session::seek(double seconds) {
    if (sing) throw std::runtime_error("Return to Clip to seek. Live Sing follows playback in the source app.");
    std::lock_guard lock(mutex);
    if (!active.clip) return;
    auto position = sane(seconds, 0, active.clip->seconds());
    auto range = engine.loop.load();
    if (position < double(range >> 32) / rate || position >= double(uint32_t(range)) / rate) engine.looping.store(false);
    engine.seek(active.clip->id, uint32_t(position * rate));
    engine.playhead.store(position);
}
void Session::loopEnabled(bool value) { if (!sing) engine.looping.store(value); }
void Session::speed(float value) { engine.speed.store(sane(value, .25, 1.5)); }
void Session::pitch(float value) { engine.pitch.store(sane(value, -13, 13)); }
void Session::loop(double a, double b, bool snap) {
    std::lock_guard lock(mutex); if (!active.clip) return;
    auto& clip = *active.clip;
    if (snap && !clip.beats.empty()) {
        auto nearest = [&](double t) { return *std::min_element(clip.beats.begin(), clip.beats.end(), [&](double x, double y) { return std::abs(x + gridOffset - t) < std::abs(y + gridOffset - t); }) + gridOffset; };
        a = nearest(a); b = nearest(b);
    }
    a = sane(a, 0, std::max(0., clip.seconds() - .02)); b = sane(b, a + .02, std::max(a + .02, clip.seconds()));
    engine.setLoop(a, b);
}
void Session::grid(double value) { gridOffset = sane(value, -2, 2); }
void Session::stem(int index, float level, bool mute, bool solo) {
    if (index < 0 || index >= 6) return;
    if (sing) throw std::runtime_error("Return to Clip to use the stem mixer. Live Sing has its own vocal removal control.");
    gain[index] = sane(level, 0, 2); muted[index] = mute; soloed[index] = solo;
    bool hasSolo = std::any_of(soloed.begin(), soloed.end(), [](bool x) { return x; });
    for (int i = 0; i < 6; ++i) engine.gains[i].store(muted[i] || (hasSolo && !soloed[i]) ? 0 : gain[i]);
}
void Session::trainer(bool enabled, float start, float step, int every) {
    engine.trainerEnabled.store(enabled); engine.trainerStep.store(sane(step, .01, .5)); engine.trainerEvery.store(std::clamp(every, 1, 100));
    if (enabled) { engine.speed.store(sane(start, .25, 1)); engine.loops.store(0); }
}
void Session::countIn(bool enabled) { counting = enabled; }
int Session::waveform(float* out, int cap) {
    std::lock_guard lock(mutex); if (!active.clip) return 0;
    int n = std::min(std::max(0, cap), int(active.clip->peaks.size())); std::copy_n(active.clip->peaks.data(), n, out); return n;
}
int Session::beats(double* out, int cap) {
    std::lock_guard lock(mutex); if (!active.clip) return 0;
    int n = std::min(std::max(0, cap), int(active.clip->beats.size()));
    for (int i = 0; i < n; ++i) out[i] = active.clip->beats[i] + gridOffset; return n;
}
int Session::histories(WSHistory* out, int cap) {
    std::lock_guard lock(mutex); int n = std::min(std::max(0, cap), int(history.size()));
    for (int i = 0; i < n; ++i) { out[i] = {}; out[i].id = history[i].clip->id; out[i].duration = history[i].clip->seconds(); text(out[i].title, history[i].title); }
    return n;
}
void Session::select(uint64_t id) {
    if (sing) setSing(false);
    ++generation; busy.store(false); startOutput();
    Item item;
    {
        std::lock_guard lock(mutex);
        auto i = std::find_if(history.begin(), history.end(), [id](auto& x) { return x.clip->id == id; });
        if (i == history.end()) return;
        activate(*i); item = *i;
    }
    if (!sixPath.empty() && item.stems->ready.load() < item.stems->size) {
        auto ticket = generation.load(); std::string path = sixPath; busy.store(true);
        jobs.submit([this, item, ticket, path] {
            try { separate(item, ticket, path); } catch (const std::exception& e) { if (ticket == generation.load()) error(e.what()); }
            if (ticket == generation.load()) busy.store(false);
        }, true);
    }
}
void Session::model(const char* six, const char* four) {
    sixPath = six ? six : ""; fourPath = four ? four : "";
    Item item;
    { std::lock_guard lock(mutex); item = active; }
    if (item.clip && !sixPath.empty() && !sing) {
        auto ticket = ++generation; busy.store(true); std::string path = sixPath;
        jobs.submit([this, item, ticket, path] {
            try { separate(item, ticket, path); } catch (const std::exception& e) { if (ticket == generation.load()) error(e.what()); }
            if (ticket == generation.load()) busy.store(false);
        }, true);
    }
}
void Session::setSing(bool enabled) {
    if (enabled == bool(sing)) return;
    if (!enabled) {
        stopCapture(); startOutput();
        error("Sing and capture stopped. The source is audible again.");
        return;
    }
    if (!source || captureStop.load()) throw std::runtime_error("Click Start Capture before starting Sing.");
    if (!singModelReady) throw std::runtime_error("Choose the streaming Sing model first. Download it with scripts/sing-model.sh.");
    auto sampleRate = capture->sample_rate();
    auto nextSing = std::make_unique<Sing>(sampleRate, singPath);
    ++generation; busy.store(false);
    jobs.submit([this] { separator.unload(); loadedModel.clear(); }, true);
    auto id = source; auto name = sourceName; auto bundle = sourceBundle;
    disarm(); output.stop(); outputStarted = false;
    try {
        double sr = tap.start(id, true);
        if (unsigned(sr) != sampleRate) throw std::runtime_error("The source audio format changed. Start Capture again before starting Sing.");
        capture = std::make_unique<CaptureStore>(unsigned(sr));
        sing = std::move(nextSing);
        source = id; sourceName = name; sourceBundle = bundle;
        startOutput(); captureStop.store(false); engine.playing.store(false);
        captureThread = std::thread([this] {
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
            std::array<Frame, 2048> frames;
            while (!captureStop.load()) {
                size_t n = 0; while (n < frames.size() && tap.queue.pop(frames[n])) ++n;
                if (n) { capture->append(frames.data(), n); for (size_t i = 0; i < n; ++i) sing->feed(frames[i]); }
                else std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        });
        error("Sing is processing live audio. Keep the source playing.");
    } catch (...) { disarm(); throw; }
}
void Session::vocals(float amount) { if (sing) sing->removal.store(sane(amount, 0, 1)); }
void Session::singPitch(float value) { if (sing) sing->pitch.store(sane(value, -6, 6)); }
void Session::singModel(const char* path) {
    singPath = path ? path : "";
    std::error_code error;
    singModelReady = !singPath.empty() && std::filesystem::is_regular_file(singPath, error);
}
}
