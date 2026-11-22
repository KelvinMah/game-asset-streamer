// Streaming demo: fly over a 4096x2560 px world made of 640 real PNG tiles that are read from disk,
// decoded on worker threads, and uploaded to the GPU on the render thread as they arrive.
//
//   Arrows / WASD  move the camera          Space  toggle autopilot (smooth tour of the world)
//   M              SYNC (blocking) vs ASYNC streaming, the point of the whole project
//   P              predictive prefetch on/off (async only): watch tiles pop in when it is off
//   Esc            quit
//
// The overlay shows frame time, P95/P99 and a frame-time graph, busy workers, queue depth, cache and
// GPU memory, load latency, hits/evictions/cancellations/promotions. In SYNC mode the main thread
// reads and decodes tiles itself and the graph spikes whenever new terrain scrolls in.
//
// Options: --assets DIR  --latency-ms N (extra per-read delay, default 8, models slow storage)
//          --threads N  --autopan  --sync  --reactive  --screenshot FILE.bmp --frames N (headless capture)
#include "streamer/BitmapFont.hpp"
#include "streamer/TileWorld.hpp"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <unordered_map>

using namespace streamer;
using namespace streamer::tiles;
namespace fs = std::filesystem;

namespace {

constexpr std::size_t CacheTiles = 90;      // decoded tiles the cache may hold (~5.6 MB)
constexpr int MaxUploadsPerFrame = 4;       // GPU uploads are budgeted so one burst cannot hitch a frame

void fill(SDL_Renderer* r, int x, int y, int w, int h, Uint8 R, Uint8 G, Uint8 B, Uint8 A = 255) {
    SDL_SetRenderDrawColor(r, R, G, B, A);
    const SDL_Rect q{x, y, w, h};
    SDL_RenderFillRect(r, &q);
}

void text(SDL_Renderer* r, const std::string& s, int x, int y, int scale, SDL_Color c) {
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    forEachTextPixel(s.c_str(), x, y, scale, [&](int px, int py, int w, int h) {
        const SDL_Rect q{px, py, w, h};
        SDL_RenderFillRect(r, &q);
    });
}

std::string fmt(const char* f, double a = 0, double b = 0, double c = 0, double d = 0) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), f, a, b, c, d);
    return buf;
}

double percentile(std::vector<float> v, double p) {
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))];
}

struct Options2 {
    fs::path assets = "demo_assets";
    int latencyMs = 8;
    std::size_t threads = 0;
    bool autopan = false, sync = false, predictive = true;
    std::string screenshot;
    int frames = 0;
};

using FrameClock = std::chrono::steady_clock;

} // namespace

int main(int argc, char** argv) {
    Options2 o;
    o.threads = std::max(2u, std::thread::hardware_concurrency());
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--assets") o.assets = next();
        else if (a == "--latency-ms") o.latencyMs = std::atoi(next());
        else if (a == "--threads") o.threads = std::strtoul(next(), nullptr, 10);
        else if (a == "--autopan") o.autopan = true;
        else if (a == "--sync") o.sync = true;
        else if (a == "--reactive") o.predictive = false;
        else if (a == "--screenshot") o.screenshot = next();
        else if (a == "--frames") o.frames = std::atoi(next());
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 1; }
    }
    if (o.threads == 0) o.threads = 2;
    if (!o.screenshot.empty()) { o.autopan = true; if (o.frames == 0) o.frames = 300; }
    ensureAssets(o.assets);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Asset Streamer", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ScreenW, ScreenH, SDL_WINDOW_SHOWN);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC) : nullptr;
    if (window && !renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!window || !renderer) {
        std::fprintf(stderr, "Window/renderer: %s\n", SDL_GetError());
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    std::shared_ptr<AssetSource> source = std::make_shared<FileSource>(o.assets.string());
    if (o.latencyMs > 0) source = std::make_shared<LatencySource>(source, std::chrono::milliseconds(o.latencyMs));
    auto manager = std::make_unique<AssetManager>(source, Options{o.threads, CacheTiles * DecodedTileBytes});
    auto streamer = std::make_unique<TileStreamer>(*manager);
    UploadQueue uploads;

    std::unordered_map<int, SDL_Texture*> textures; // tiles currently on the GPU
    const auto resident = [&textures](TileId id) { return textures.count(id.key()) > 0; };
    auto upload = [&](TileId id, const AssetPtr& asset) {
        SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, asset->width, asset->height);
        if (!tex) return;
        SDL_UpdateTexture(tex, nullptr, asset->bytes.data(), asset->width * 4);
        textures[id.key()] = tex;
    };

    Camera cam = clampCamera({(WorldW - ScreenW) * 0.5f, (WorldH - ScreenH) * 0.5f});
    float velX = 0, velY = 0, lookVX = 0, lookVY = 0;
    bool autopan = o.autopan, syncMode = o.sync, predictive = o.predictive, running = true;
    double simTime = 0, worstOverall = 0, worstRecent = 0, sinceWorstReset = 0, fpsAccum = 0, fps = 0, syncStallMsTotal = 0;
    int fpsFrames = 0, frame = 0;
    std::vector<float> graph(240, 0.0f);
    std::size_t graphPos = 0;
    auto last = FrameClock::now();
    double lastWorkMs = 16.7; // capture mode reports work time, excluding its pacing delay

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) running = false;
            if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                if (e.key.keysym.sym == SDLK_ESCAPE) running = false;
                if (e.key.keysym.sym == SDLK_SPACE) autopan = !autopan;
                if (e.key.keysym.sym == SDLK_m) syncMode = !syncMode;
                if (e.key.keysym.sym == SDLK_p) predictive = !predictive;
            }
        }

        const auto now = FrameClock::now();
        double frameMs = std::chrono::duration<double, std::milli>(now - last).count();
        last = now;
        if (frame == 0) frameMs = 16.7;
        if (o.frames > 0) frameMs = lastWorkMs;
        const double dt = o.frames > 0 ? 1.0 / 60.0 : std::min(frameMs / 1000.0, 0.1); // fixed step when capturing
        simTime += dt;

        // ---- camera ----
        const Camera before = cam;
        if (autopan) {
            cam = autopilot(simTime);
        } else {
            const Uint8* k = SDL_GetKeyboardState(nullptr);
            const float ax = static_cast<float>((k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) - (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]));
            const float ay = static_cast<float>((k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S]) - (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W]));
            const float accel = 3000.0f, maxV = 900.0f, drag = 6.0f;
            velX = std::clamp(velX + (ax * accel - velX * drag) * static_cast<float>(dt), -maxV, maxV);
            velY = std::clamp(velY + (ay * accel - velY * drag) * static_cast<float>(dt), -maxV, maxV);
            cam.x += velX * static_cast<float>(dt);
            cam.y += velY * static_cast<float>(dt);
        }
        cam = clampCamera(cam);
        const float inv = dt > 0 ? 1.0f / static_cast<float>(dt) : 0.0f;
        lookVX += ((cam.x - before.x) * inv - lookVX) * 0.1f; // smoothed velocity (px/s) drives prediction
        lookVY += ((cam.y - before.y) * inv - lookVY) * 0.1f;
        const TileRect view = viewportTiles(cam);

        // ---- load tiles ----
        double syncStallMs = 0;
        std::size_t planned = 0;
        if (syncMode) { // naive loader: read + decode + upload right here, blocking the frame
            for (int ty = view.y0; ty <= view.y1; ++ty)
                for (int tx = view.x0; tx <= view.x1; ++tx) {
                    const TileId id{tx, ty};
                    if (resident(id)) continue;
                    const auto t0 = FrameClock::now();
                    upload(id, decodeAsset(tilePath(id), source->read(tilePath(id))));
                    syncStallMs += std::chrono::duration<double, std::milli>(FrameClock::now() - t0).count();
                }
        } else {
            PlanSettings settings;
            settings.predictive = predictive;
            const auto plan = planTiles(cam, lookVX, lookVY, settings);
            planned = plan.size();
            streamer->update(plan, resident);                                   // request, promote, cancel
            for (ReadyTile& t : streamer->collect(1000)) uploads.add(std::move(t)); // completions run on THIS thread
            for (const ReadyTile& t : uploads.take(cam, MaxUploadsPerFrame, resident)) upload(t.id, t.asset);
        }
        syncStallMsTotal += syncStallMs;

        // ---- release GPU textures that are well off screen ----
        {
            const TileRect keep = keepRange(cam);
            for (auto it = textures.begin(); it != textures.end();) {
                if (!keep.contains(tileFromKey(it->first))) {
                    SDL_DestroyTexture(it->second);
                    it = textures.erase(it);
                } else {
                    ++it;
                }
            }
        }

        // ---- draw world ----
        SDL_SetRenderDrawColor(renderer, 18, 22, 38, 255);
        SDL_RenderClear(renderer);
        int loadedVisible = 0, visible = 0;
        for (int ty = view.y0; ty <= view.y1; ++ty) {
            for (int tx = view.x0; tx <= view.x1; ++tx) {
                ++visible;
                const SDL_Rect dst{static_cast<int>(std::lround(tx * TileSize - cam.x)), static_cast<int>(std::lround(ty * TileSize - cam.y)), TileSize, TileSize};
                const auto it = textures.find(TileId{tx, ty}.key());
                if (it != textures.end()) {
                    SDL_RenderCopy(renderer, it->second, nullptr, &dst);
                    ++loadedVisible;
                } else { // placeholder: animated bars prove the render loop is alive while data streams in
                    fill(renderer, dst.x, dst.y, TileSize, TileSize, 30, 34, 48);
                    const int off = static_cast<int>(simTime * 60.0) % 24;
                    for (int by = -24; by < TileSize; by += 24) {
                        const int y0 = std::max(0, by + off), y1 = std::min(TileSize, by + off + 6);
                        if (y1 > y0) fill(renderer, dst.x, dst.y + y0, TileSize, y1 - y0, 46, 52, 72);
                    }
                }
            }
        }

        // ---- overlay ----
        const Stats st = manager->stats();
        graph[graphPos++ % graph.size()] = static_cast<float>(frameMs);
        worstRecent = std::max(worstRecent, frameMs);
        if (frame > 10) worstOverall = std::max(worstOverall, frameMs);
        sinceWorstReset += frameMs;
        if (sinceWorstReset > 1000.0) { sinceWorstReset = 0; worstRecent = 0; }
        fpsAccum += o.frames > 0 ? std::max(frameMs, 1000.0 / 60.0) : frameMs; // capture mode is paced at 60 Hz
        if (++fpsFrames == 30) { fps = 1000.0 * fpsFrames / fpsAccum; fpsAccum = 0; fpsFrames = 0; }
        std::vector<float> recent;
        for (float v : graph) if (v > 0.0f) recent.push_back(v);

        const SDL_Color white{240, 244, 252, 255}, dim{160, 170, 190, 255}, accent{255, 214, 80, 255}, bad{255, 110, 110, 255}, good{110, 230, 140, 255};
        fill(renderer, 8, 8, 476, 262, 8, 10, 18, 215);
        text(renderer, syncMode ? "MODE: SYNC LOADING (BLOCKS FRAME)" : predictive ? "MODE: ASYNC + PREDICTIVE" : "MODE: ASYNC, VISIBLE ONLY", 16, 16, 2, syncMode ? bad : good);
        text(renderer, fmt("FPS %.0f   FRAME %.1f MS   WORST(1S) %.1f", fps, frameMs, worstRecent), 16, 36, 2, white);
        const int gx = 16, gy = 58, gh = 56;
        fill(renderer, gx, gy, 240, gh, 20, 24, 36, 255);
        for (std::size_t i = 0; i < graph.size(); ++i) {
            const float v = graph[(graphPos + i) % graph.size()];
            const int h = std::clamp(static_cast<int>(v / 50.0f * gh), 1, gh);
            if (v > 0.0f) fill(renderer, gx + static_cast<int>(i), gy + gh - h, 1, h, v > 20.0f ? 255 : 90, v > 20.0f ? 90 : 210, 90);
        }
        fill(renderer, gx, gy + gh - static_cast<int>(16.7f / 50.0f * gh), 240, 1, 255, 255, 255, 120);
        text(renderer, "60 FPS LINE", gx + 246, gy + gh - 8, 1, dim);
        text(renderer, "GRAPH 0-50 MS", gx + 246, gy, 1, dim);
        if (recent.size() >= 20) text(renderer, fmt("P50 %.1f  P95 %.1f  P99 %.1f MS (LAST 240)", percentile(recent, 0.50), percentile(recent, 0.95), percentile(recent, 0.99)), 16, 120, 2, white);

        int y = 142;
        auto line = [&](const std::string& s, SDL_Color c = {240, 244, 252, 255}) { text(renderer, s, 16, y, 2, c); y += 18; };
        if (syncMode) {
            line(fmt("MAIN THREAD STALLED %.1f MS THIS FRAME", syncStallMs), syncStallMs > 1.0 ? bad : dim);
            line(fmt("TOTAL STALL %.0f MS", syncStallMsTotal), dim);
            line("NO CACHE, NO WORKERS IN THIS MODE", dim);
        } else {
            line(fmt("WORKERS %.0f/%.0f BUSY   QUEUED %.0f", st.activeLoads, st.workers, st.queuedLoads), accent);
            line(fmt("CACHE %.1f/%.1f MB   GPU %.1f MB", st.cachedBytes / 1048576.0, st.cacheCapacity / 1048576.0, textures.size() * DecodedTileBytes / 1048576.0));
            line(fmt("LOAD LATENCY MEAN %.0f  P95 %.0f MS", st.meanLatencyMs, st.p95LatencyMs));
            line(fmt("LOADS %.0f  HITS %.0f  EVICT %.0f", static_cast<double>(st.loadsStarted), static_cast<double>(st.cacheHits), static_cast<double>(st.evictions)));
            line(fmt("CANCELLED %.0f  PROMOTED %.0f  PLAN %.0f", static_cast<double>(st.cancelled), static_cast<double>(st.boosts), static_cast<double>(planned)));
        }
        line(fmt("VISIBLE %.0f  LOADED %.0f  UPLOAD QUEUE %.0f", visible, loadedVisible, static_cast<double>(uploads.size())), loadedVisible < visible ? accent : white);
        line(fmt("EXTRA READ LATENCY %.0f MS PER FILE", o.latencyMs), dim);
        text(renderer, "ARROWS/WASD MOVE  SPACE AUTOPILOT  M SYNC  P PREDICT  ESC", 8, ScreenH - 16, 1, white);

        SDL_RenderPresent(renderer);
        ++frame;
        lastWorkMs = std::chrono::duration<double, std::milli>(FrameClock::now() - now).count();

        if (o.frames > 0) {
            SDL_Delay(16); // headless capture: pace to ~60 Hz so worker threads progress in real time
            if (frame >= o.frames) {
                if (!o.screenshot.empty()) {
                    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(0, ScreenW, ScreenH, 32, SDL_PIXELFORMAT_RGBA32);
                    if (surf && SDL_RenderReadPixels(renderer, nullptr, SDL_PIXELFORMAT_RGBA32, surf->pixels, surf->pitch) == 0) SDL_SaveBMP(surf, o.screenshot.c_str());
                    if (surf) SDL_FreeSurface(surf);
                }
                std::printf("mode=%s frames=%d worst_frame_ms=%.1f failures=%llu loads=%llu hits=%llu evictions=%llu cancelled=%llu promoted=%llu latency_mean_ms=%.1f p95_ms=%.1f sync_stall_total_ms=%.0f\n",
                            syncMode ? "sync" : predictive ? "predictive" : "reactive", frame, worstOverall, static_cast<unsigned long long>(streamer->failures()),
                            static_cast<unsigned long long>(st.loadsStarted), static_cast<unsigned long long>(st.cacheHits), static_cast<unsigned long long>(st.evictions),
                            static_cast<unsigned long long>(st.cancelled), static_cast<unsigned long long>(st.boosts), st.meanLatencyMs, st.p95LatencyMs, syncStallMsTotal);
                running = false;
            }
        }
    }

    streamer.reset(); // the streamer's callbacks reference it: drop it before the manager
    manager.reset();  // join workers before tearing down SDL
    for (auto& kv : textures) SDL_DestroyTexture(kv.second);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
