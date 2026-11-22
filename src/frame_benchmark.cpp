// Frame-time benchmark: replays the SAME scripted camera tour through a paced 60 Hz loop in three
// loading strategies and reports what the main thread actually experienced.
//
//   sync        the render thread reads + decodes missing tiles itself (what a naive loader does)
//   reactive    async streaming, but only visible tiles are requested (no prediction, no cancel)
//   predictive  async streaming with camera-velocity prediction, priorities, promotion and cancellation
//
// "Frame work" is the time the main thread spends inside a frame (loading logic + uploads + a fixed
// simulated render cost), excluding the sleep that paces the loop. A frame whose work exceeds the
// 16.7 ms budget misses 60 FPS. Pop-in counts tile-frames where a visible tile was not yet loaded.
//
// Asset files are real PNGs; storage latency beyond the OS file cache is simulated (--latency-ms).
// GPU uploads are modelled as a memcpy of the decoded pixels into a per-tile buffer.
#include "streamer/TileWorld.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace streamer;
using namespace streamer::tiles;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

namespace {

struct Config {
    double seconds = 20.0;
    int latencyMs = 8;
    double speed = 1.0;          // tour speed multiplier: 3 = camera moves three times faster
    double renderMs = 4.0;       // fixed simulated cost of the rest of the frame (draw calls, game logic)
    std::size_t threads = 0;
    std::size_t cacheTiles = 90;
    int uploadsPerFrame = 4;
    std::vector<std::string> modes = {"sync", "reactive", "predictive"};
    fs::path assets;
};

double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

double percentile(std::vector<double> v, double p) {
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))];
}

void spin(double milliseconds) {
    const auto end = Clock::now() + std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double, std::milli>(milliseconds));
    while (Clock::now() < end) {}
}

struct RunResult {
    std::string mode;
    std::vector<double> frameMs;
    std::uint64_t popInTileFrames = 0, visibleTileFrames = 0;
    double syncStallMs = 0;
    double peakCpuMB = 0, peakGpuMB = 0;
    double wallMs = 0;
    Stats stats;
    std::uint64_t requestsIssued = 0, cancelsWon = 0;
};

RunResult run(const Config& c, const std::string& mode) {
    std::shared_ptr<AssetSource> source = std::make_shared<FileSource>(c.assets.string());
    if (c.latencyMs > 0) source = std::make_shared<LatencySource>(source, std::chrono::milliseconds(c.latencyMs));
    auto manager = std::make_unique<AssetManager>(source, Options{c.threads, c.cacheTiles * DecodedTileBytes});
    TileStreamer streamer(*manager);
    UploadQueue uploads;
    PlanSettings plan;
    plan.predictive = mode == "predictive";

    std::unordered_map<int, std::vector<std::uint8_t>> gpu; // simulated GPU-resident tiles
    auto upload = [&gpu](TileId id, const AssetPtr& a) { gpu[id.key()] = a->bytes; };

    RunResult r;
    r.mode = mode;
    const int frames = static_cast<int>(c.seconds * 60.0);
    const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / 60.0));
    Camera prev = autopilot(0);
    const double speed = c.speed;
    float lookVX = 0, lookVY = 0;
    auto deadline = Clock::now();
    const auto wallStart = Clock::now();

    for (int f = 0; f < frames; ++f) {
        const auto t0 = Clock::now();
        const Camera cam = autopilot(f / 60.0 * speed);
        lookVX += ((cam.x - prev.x) * 60.0f - lookVX) * 0.1f; // velocity in px/s, smoothed like the demo
        lookVY += ((cam.y - prev.y) * 60.0f - lookVY) * 0.1f;
        prev = cam;
        const TileRect view = viewportTiles(cam);

        if (mode == "sync") {
            for (int ty = view.y0; ty <= view.y1; ++ty)
                for (int tx = view.x0; tx <= view.x1; ++tx) {
                    const TileId id{tx, ty};
                    if (gpu.count(id.key())) continue;
                    const auto s0 = Clock::now();
                    upload(id, decodeAsset(tilePath(id), source->read(tilePath(id))));
                    r.syncStallMs += ms(Clock::now() - s0);
                }
        } else {
            const auto resident = [&gpu](TileId id) { return gpu.count(id.key()) > 0; };
            streamer.update(planTiles(cam, lookVX, lookVY, plan), resident);
            for (ReadyTile& t : streamer.collect(1000)) uploads.add(std::move(t));
            for (const ReadyTile& t : uploads.take(cam, static_cast<std::size_t>(c.uploadsPerFrame), resident)) upload(t.id, t.asset);
        }

        const TileRect keep = keepRange(cam); // release tiles well off screen
        for (auto it = gpu.begin(); it != gpu.end();) it = keep.contains(tileFromKey(it->first)) ? std::next(it) : gpu.erase(it);

        spin(c.renderMs);
        r.frameMs.push_back(ms(Clock::now() - t0));

        for (int ty = view.y0; ty <= view.y1; ++ty)
            for (int tx = view.x0; tx <= view.x1; ++tx) {
                ++r.visibleTileFrames;
                if (!gpu.count(TileId{tx, ty}.key())) ++r.popInTileFrames;
            }
        r.peakGpuMB = std::max(r.peakGpuMB, static_cast<double>(gpu.size() * DecodedTileBytes) / 1048576.0);
        if (mode != "sync") r.peakCpuMB = std::max(r.peakCpuMB, static_cast<double>(manager->stats().cachedBytes) / 1048576.0);

        deadline += period;
        if (Clock::now() < deadline) std::this_thread::sleep_until(deadline);
        else deadline = Clock::now(); // overran: no catch-up burst
    }
    r.wallMs = ms(Clock::now() - wallStart);
    r.stats = manager->stats();
    r.requestsIssued = streamer.requestsIssued();
    r.cancelsWon = streamer.cancelsWon();
    return r;
}

void warmFileCache(const fs::path& dir) { // every mode then sees the same OS-cache state
    FileSource source(dir.string());
    for (int ty = 0; ty < WorldTilesY; ++ty)
        for (int tx = 0; tx < WorldTilesX; ++tx) source.read(tilePath({tx, ty}));
}

} // namespace

int main(int argc, char** argv) {
    Config c;
    c.threads = std::max(2u, std::thread::hardware_concurrency());
    c.assets = fs::temp_directory_path() / "streamer_frame_benchmark_assets";
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string k = argv[i];
        const char* v = argv[i + 1];
        if (k == "--seconds") c.seconds = std::atof(v);
        else if (k == "--latency-ms") c.latencyMs = std::atoi(v);
        else if (k == "--speed") c.speed = std::atof(v);
        else if (k == "--render-ms") c.renderMs = std::atof(v);
        else if (k == "--threads") c.threads = std::strtoul(v, nullptr, 10);
        else if (k == "--cache-tiles") c.cacheTiles = std::strtoul(v, nullptr, 10);
        else if (k == "--assets") c.assets = v;
        else if (k == "--modes") {
            c.modes.clear();
            std::string s = v, item;
            for (char ch : s + ",") { if (ch == ',') { if (!item.empty()) c.modes.push_back(item); item.clear(); } else item += ch; }
        } else { std::fprintf(stderr, "unknown option %s\n", k.c_str()); return 1; }
    }
    if (c.seconds < 1 || c.speed <= 0 || c.threads == 0 || c.cacheTiles == 0 || c.modes.empty()) { std::fprintf(stderr, "bad arguments\n"); return 1; }
    for (const auto& m : c.modes) if (m != "sync" && m != "reactive" && m != "predictive") { std::fprintf(stderr, "unknown mode %s\n", m.c_str()); return 1; }

    ensureAssets(c.assets);
    warmFileCache(c.assets);

    std::printf("# Frame-time benchmark\n\n");
    std::printf("Scripted camera tour (speed x%.1f) over a %dx%d tile world of real PNGs, %.0f s per mode at a paced 60 Hz (%d frames), %zu worker threads (%u hardware threads),\n"
                "cache budget %zu tiles (%.1f MB), +%d ms simulated read latency per file, %.1f ms simulated render cost per frame, <=%d GPU uploads per frame.\n"
                "Frame work = main-thread time in the frame excluding pacing sleep; budget for 60 FPS is 16.7 ms.\n\n",
                c.speed, WorldTilesX, WorldTilesY, c.seconds, static_cast<int>(c.seconds * 60), c.threads, std::thread::hardware_concurrency(), c.cacheTiles,
                static_cast<double>(c.cacheTiles * DecodedTileBytes) / 1048576.0, c.latencyMs, c.renderMs, c.uploadsPerFrame);

    std::vector<RunResult> results;
    for (const auto& mode : c.modes) results.push_back(run(c, mode));

    std::printf("| mode | P50 | P95 | P99 | max | frames > 16.7 ms | pop-in tile-frames | worker util | peak CPU cache | peak GPU tiles |\n");
    std::printf("|---|---|---|---|---|---|---|---|---|---|\n");
    for (const RunResult& r : results) {
        std::size_t over = 0;
        for (double v : r.frameMs) over += v > 16.7;
        const double util = r.mode == "sync" ? 0.0 : 100.0 * r.stats.busyMs / (static_cast<double>(c.threads) * r.wallMs);
        std::printf("| %s | %.2f ms | %.2f ms | %.2f ms | %.1f ms | %.1f%% (%zu) | %.2f%% (%llu) | %s | %.1f MB | %.1f MB |\n", r.mode.c_str(),
                    percentile(r.frameMs, 0.50), percentile(r.frameMs, 0.95), percentile(r.frameMs, 0.99), *std::max_element(r.frameMs.begin(), r.frameMs.end()),
                    100.0 * static_cast<double>(over) / static_cast<double>(r.frameMs.size()), over,
                    100.0 * static_cast<double>(r.popInTileFrames) / static_cast<double>(r.visibleTileFrames), static_cast<unsigned long long>(r.popInTileFrames),
                    r.mode == "sync" ? "n/a" : (std::to_string(static_cast<int>(util + 0.5)) + "%").c_str(),
                    r.peakCpuMB, r.peakGpuMB);
    }

    std::printf("\n| mode | loads started | cache hits | cancelled (queued, never read) | priority promotions | load latency mean / p95 | main-thread sync stall |\n|---|---|---|---|---|---|---|\n");
    for (const RunResult& r : results) {
        if (r.mode == "sync") {
            std::printf("| sync | n/a | n/a | n/a | n/a | n/a | %.0f ms total |\n", r.syncStallMs);
            continue;
        }
        std::printf("| %s | %llu | %llu (%.0f%% of requests) | %llu | %llu | %.0f / %.0f ms | 0 |\n", r.mode.c_str(), static_cast<unsigned long long>(r.stats.loadsStarted),
                    static_cast<unsigned long long>(r.stats.cacheHits), 100.0 * static_cast<double>(r.stats.cacheHits) / static_cast<double>(std::max<std::uint64_t>(1, r.stats.requests)),
                    static_cast<unsigned long long>(r.stats.cancelled), static_cast<unsigned long long>(r.stats.boosts), r.stats.meanLatencyMs, r.stats.p95LatencyMs);
    }
    std::printf("\nSingle run per mode on identical input; frame-time tails vary a little between runs, so repeat before quoting digits.\n");
    return 0;
}
