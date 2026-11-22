// Benchmarks the streaming system on REAL files: PNG textures are written to a temp directory,
// then read from disk and decoded to RGBA exactly as a game would.
//
// Two caveats, both printed in the output:
//   * Files are read right after being written, so they sit in the OS file cache. That removes seek
//     time; what is measured is syscall + PNG decode + threading. Use --latency-us to add a fixed
//     per-read delay modelling slow storage (spinning disk, network, console optical drive).
//   * Speed-up from threads is bounded by your core count when decode-bound, so numbers differ
//     per machine. Run it on yours before quoting anything.
#include "streamer/AssetManager.hpp"
#include "streamer/Png.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace streamer;
using Clock = std::chrono::steady_clock;
namespace fs = std::filesystem;

namespace {

struct Config {
    std::size_t assets = 200;
    std::size_t requests = 3200;
    int side = 128;              // texture is side x side RGBA8
    std::size_t threads = 0;     // 0 = hardware concurrency
    std::vector<int> latenciesUs = {0, 2000};
    double cacheFraction = 0.25; // cache budget as a fraction of the decoded working set
    double zipf = 1.0;           // popularity skew; 0 = uniform
    std::size_t frameSize = 16;  // requests issued together, like the assets one game frame needs
    unsigned seed = 1;
    int repeats = 3;
};

double ms(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
std::string assetName(std::size_t i) { return "tex_" + std::to_string(i) + ".png"; }

void generateAssets(const fs::path& dir, const Config& c) {
    fs::create_directories(dir);
    std::mt19937 rng(c.seed);
    for (std::size_t i = 0; i < c.assets; ++i) {
        Image img;
        img.width = img.height = c.side;
        img.rgba.resize(static_cast<std::size_t>(c.side) * c.side * 4);
        for (int y = 0; y < c.side; ++y) {
            for (int x = 0; x < c.side; ++x) {
                std::uint8_t* px = &img.rgba[(static_cast<std::size_t>(y) * c.side + x) * 4];
                px[0] = static_cast<std::uint8_t>((x * 2 + i) & 255);
                px[1] = static_cast<std::uint8_t>((y * 2 + rng() % 16) & 255);
                px[2] = static_cast<std::uint8_t>(((x ^ y) + i * 3) & 255);
                px[3] = 255;
            }
        }
        const Bytes png = encodePng(img);
        std::ofstream f(dir / assetName(i), std::ios::binary);
        f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    }
}

std::vector<std::string> makeWorkload(const Config& c) {
    std::vector<double> weights(c.assets);
    for (std::size_t i = 0; i < c.assets; ++i) weights[i] = 1.0 / std::pow(static_cast<double>(i + 1), c.zipf);
    std::mt19937 rng(c.seed);
    std::discrete_distribution<std::size_t> pick(weights.begin(), weights.end());
    std::vector<std::string> names;
    for (std::size_t i = 0; i < c.requests; ++i) names.push_back(assetName(pick(rng)));
    return names;
}

std::shared_ptr<AssetSource> makeSource(const fs::path& dir, int latencyUs) {
    auto files = std::make_shared<FileSource>(dir.string());
    if (latencyUs <= 0) return files;
    return std::make_shared<LatencySource>(files, std::chrono::microseconds(latencyUs));
}

struct Run {
    double meanFrameMs = 0, p95FrameMs = 0, meanSubmitMs = 0;
    Stats stats;
};

void summarize(Run& r, std::vector<double> frameMs, double submitTotalMs) {
    std::sort(frameMs.begin(), frameMs.end());
    double sum = 0;
    for (double v : frameMs) sum += v;
    r.meanFrameMs = sum / static_cast<double>(frameMs.size());
    r.p95FrameMs = frameMs[static_cast<std::size_t>(0.95 * static_cast<double>(frameMs.size() - 1))];
    r.meanSubmitMs = submitTotalMs / static_cast<double>(frameMs.size());
}

// Baseline: what a naive loader does. Read and decode on the calling thread, no cache.
Run runSynchronous(const Config& c, const fs::path& dir, int latencyUs, const std::vector<std::string>& workload) {
    const auto source = makeSource(dir, latencyUs);
    std::vector<double> frames;
    double sink = 0;
    double total = 0;
    for (std::size_t i = 0; i < workload.size(); i += c.frameSize) {
        const auto f0 = Clock::now();
        for (std::size_t j = i; j < std::min(workload.size(), i + c.frameSize); ++j) {
            sink += static_cast<double>(decodeAsset(workload[j], source->read(workload[j]))->width);
        }
        frames.push_back(ms(Clock::now() - f0));
        total += frames.back();
    }
    Run r;
    summarize(r, frames, total); // the baseline is busy for the whole frame
    if (sink < 0) std::puts("");
    return r;
}

Run runManaged(const Config& c, const fs::path& dir, int latencyUs, const std::vector<std::string>& workload,
               std::size_t threads, std::size_t cacheBytes) {
    AssetManager manager(makeSource(dir, latencyUs), {threads, cacheBytes});
    std::vector<double> frames;
    double submitTotal = 0;
    for (std::size_t i = 0; i < workload.size(); i += c.frameSize) {
        const auto f0 = Clock::now();
        std::vector<AssetFuture> futures;
        for (std::size_t j = i; j < std::min(workload.size(), i + c.frameSize); ++j) {
            futures.push_back(manager.request(workload[j], priority::Normal));
        }
        submitTotal += ms(Clock::now() - f0);
        for (auto& f : futures) {
            if (!f.get()) std::exit(2);
        }
        frames.push_back(ms(Clock::now() - f0));
    }
    Run r;
    summarize(r, frames, submitTotal);
    r.stats = manager.stats();
    return r;
}

template <class F>
Run median(int repeats, F&& f) {
    std::vector<Run> runs;
    for (int i = 0; i < repeats; ++i) runs.push_back(f());
    std::sort(runs.begin(), runs.end(), [](const Run& a, const Run& b) { return a.meanFrameMs < b.meanFrameMs; });
    return runs[runs.size() / 2];
}

void printRow(const char* name, const Run& r, const Run& base) {
    const double saved = 100.0 * (base.meanFrameMs - r.meanFrameMs) / base.meanFrameMs;
    const double req = static_cast<double>(r.stats.requests ? r.stats.requests : 1);
    std::printf("| %-34s | %10.3f | %9.3f | %9.3f | %7.1f%% | %5.1f%% | %9.1f%% | %6llu |\n", name, r.meanFrameMs, r.p95FrameMs, r.meanSubmitMs, saved,
                100.0 * static_cast<double>(r.stats.cacheHits) / req, 100.0 * static_cast<double>(r.stats.coalesced) / req,
                static_cast<unsigned long long>(r.stats.loadsStarted));
}

void runSuite(const Config& c, const fs::path& dir, int latencyUs, const std::vector<std::string>& workload, std::size_t cacheBytes) {
    std::printf("### Real PNG files, extra simulated read latency: %d us\n\n", latencyUs);
    std::printf("| %-34s | %10s | %9s | %9s | %8s | %5s | %9s | %6s |\n", "Configuration", "mean frame", "p95 frame", "submit/fr", "vs base", "hits", "coalesced", "loads");
    std::printf("|%s|%s|%s|%s|%s|%s|%s|%s|\n", std::string(36, '-').c_str(), std::string(12, '-').c_str(), std::string(11, '-').c_str(),
                std::string(11, '-').c_str(), std::string(10, '-').c_str(), std::string(7, '-').c_str(), std::string(11, '-').c_str(), std::string(8, '-').c_str());
    const Run base = median(c.repeats, [&] { return runSynchronous(c, dir, latencyUs, workload); });
    printRow("synchronous, no cache (baseline)", base, base);
    printRow("thread pool, no cache", median(c.repeats, [&] { return runManaged(c, dir, latencyUs, workload, c.threads, 0); }), base);
    printRow("1 thread + LRU cache", median(c.repeats, [&] { return runManaged(c, dir, latencyUs, workload, 1, cacheBytes); }), base);
    printRow("thread pool + LRU cache (full)", median(c.repeats, [&] { return runManaged(c, dir, latencyUs, workload, c.threads, cacheBytes); }), base);
    std::printf("\n");
}

// Priority promotion: 200 low-priority loads are queued, then the LAST one is requested again at
// High priority. Compare when it becomes ready with and without that promotion.
void runPromotionDemo(const Config& c, const fs::path& dir) {
    const int latencyUs = 2000;
    auto run = [&](bool promote) {
        AssetManager manager(makeSource(dir, latencyUs), {c.threads, 0});
        std::vector<AssetFuture> fs;
        const auto t0 = Clock::now();
        for (std::size_t i = 0; i < c.assets; ++i) fs.push_back(manager.request(assetName(i), priority::Low));
        AssetFuture target = promote ? manager.request(assetName(c.assets - 1), priority::High) : fs.back();
        target.get();
        const double readyMs = ms(Clock::now() - t0);
        for (auto& f : fs) f.get();
        return readyMs;
    };
    const double without = run(false), with = run(true);
    std::printf("### Priority promotion (%zu loads queued, %zu threads, +%d us per read)\n\n", c.assets, c.threads, latencyUs);
    std::printf("The last-queued texture is ready after %.1f ms if left at Low priority, but after %.1f ms when it is re-requested at High priority.\n",
                without, with);
}

} // namespace

int main(int argc, char** argv) {
    Config c;
    c.threads = std::max(2u, std::thread::hardware_concurrency());
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string key = argv[i];
        const char* val = argv[i + 1];
        if (key == "--assets") c.assets = std::strtoul(val, nullptr, 10);
        else if (key == "--requests") c.requests = std::strtoul(val, nullptr, 10);
        else if (key == "--side") c.side = std::atoi(val);
        else if (key == "--threads") c.threads = std::strtoul(val, nullptr, 10);
        else if (key == "--cache-fraction") c.cacheFraction = std::atof(val);
        else if (key == "--zipf") c.zipf = std::atof(val);
        else if (key == "--frame-size") c.frameSize = std::strtoul(val, nullptr, 10);
        else if (key == "--seed") c.seed = static_cast<unsigned>(std::strtoul(val, nullptr, 10));
        else if (key == "--repeats") c.repeats = std::atoi(val);
        else if (key == "--latency-us") { // comma separated list of extra per-read delays to run
            c.latenciesUs.clear();
            std::stringstream ss(val);
            for (std::string item; std::getline(ss, item, ',');) c.latenciesUs.push_back(std::atoi(item.c_str()));
        } else {
            std::fprintf(stderr, "unknown option %s\n", key.c_str());
            return 1;
        }
    }
    if (c.assets == 0 || c.requests == 0 || c.threads == 0 || c.frameSize == 0 || c.repeats < 1 || c.side < 1 || c.latenciesUs.empty()) {
        std::fprintf(stderr, "assets, requests, threads, frame-size, repeats and side must be positive\n");
        return 1;
    }

    const fs::path dir = fs::temp_directory_path() / "streamer_benchmark_assets";
    fs::remove_all(dir);
    generateAssets(dir, c);
    const std::vector<std::string> workload = makeWorkload(c);
    const std::size_t decodedBytes = static_cast<std::size_t>(c.side) * c.side * 4;
    const auto cacheBytes = static_cast<std::size_t>(c.cacheFraction * static_cast<double>(c.assets * decodedBytes));

    std::printf("# Asset streaming benchmark\n\n");
    std::printf("%zu requests over %zu PNG textures (%dx%d RGBA, Zipf s=%.2f), %zu worker threads, %u hardware threads available.\n", c.requests,
                c.assets, c.side, c.side, c.zipf, c.threads, std::thread::hardware_concurrency());
    std::printf("Requests arrive in frames of %zu and each frame is awaited before the next. Cache budget %zu KiB (%.0f%% of the decoded working set).\n",
                c.frameSize, cacheBytes / 1024, c.cacheFraction * 100.0);
    std::printf("Median of %d runs; times in ms. 'frame' = time until all of a frame's textures are ready. Files are in the OS file cache.\n\n", c.repeats);

    for (int latency : c.latenciesUs) runSuite(c, dir, latency, workload, cacheBytes);
    std::printf("'submit/fr' is how long the calling thread is busy per frame before it is free to keep rendering; the baseline blocks for the\n"
                "whole frame. 'hits' and 'coalesced' are percentages of requests; the remainder were real loads.\n\n");
    runPromotionDemo(c, dir);
    fs::remove_all(dir);
    return 0;
}
