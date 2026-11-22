#include "streamer/TileWorld.hpp"

#include "streamer/BitmapFont.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace streamer::tiles {
namespace {

std::uint32_t hash2(int x, int y) {
    std::uint32_t h = static_cast<std::uint32_t>(x) * 374761393U + static_cast<std::uint32_t>(y) * 668265263U;
    h = (h ^ (h >> 13)) * 1274126177U;
    return h ^ (h >> 16);
}

float valueNoise(float x, float y) {
    const int xi = static_cast<int>(std::floor(x)), yi = static_cast<int>(std::floor(y));
    const float fx = x - static_cast<float>(xi), fy = y - static_cast<float>(yi);
    const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    auto v = [](int a, int b) { return static_cast<float>(hash2(a, b) & 0xFFFF) / 65535.0f; };
    const float top = v(xi, yi) + (v(xi + 1, yi) - v(xi, yi)) * sx;
    const float bot = v(xi, yi + 1) + (v(xi + 1, yi + 1) - v(xi, yi + 1)) * sx;
    return top + (bot - top) * sy;
}

void terrainColor(int wx, int wy, std::uint8_t* out) {
    const float h = 0.6f * valueNoise(static_cast<float>(wx) / 160.0f, static_cast<float>(wy) / 160.0f) +
                    0.3f * valueNoise(static_cast<float>(wx) / 56.0f, static_cast<float>(wy) / 56.0f) +
                    0.1f * valueNoise(static_cast<float>(wx) / 14.0f, static_cast<float>(wy) / 14.0f);
    const float grain = static_cast<float>(hash2(wx, wy) & 15) - 7.0f;
    int r, g, b;
    if (h < 0.40f) { r = 28; g = 76 + static_cast<int>((h - 0.2f) * 100); b = 150 + static_cast<int>(h * 120); }      // water
    else if (h < 0.45f) { r = 214; g = 200; b = 140; }                                                                 // sand
    else if (h < 0.60f) { r = 70; g = 140 - static_cast<int>((h - 0.45f) * 160); b = 64; }                             // grass
    else if (h < 0.72f) { r = 36; g = 98; b = 56; }                                                                    // forest
    else if (h < 0.84f) { r = 118; g = 112; b = 108; }                                                                 // rock
    else { r = 240; g = 242; b = 248; }                                                                                // snow
    out[0] = static_cast<std::uint8_t>(std::clamp(r + static_cast<int>(grain), 0, 255));
    out[1] = static_cast<std::uint8_t>(std::clamp(g + static_cast<int>(grain), 0, 255));
    out[2] = static_cast<std::uint8_t>(std::clamp(b + static_cast<int>(grain), 0, 255));
    out[3] = 255;
}

} // namespace

std::string tilePath(TileId id) { return "tile_" + std::to_string(id.x) + "_" + std::to_string(id.y) + ".png"; }

Image makeTile(TileId id) {
    Image img;
    img.width = img.height = TileSize;
    img.rgba.resize(DecodedTileBytes);
    for (int y = 0; y < TileSize; ++y) {
        for (int x = 0; x < TileSize; ++x) {
            std::uint8_t* px = &img.rgba[(static_cast<std::size_t>(y) * TileSize + x) * 4];
            terrainColor(id.x * TileSize + x, id.y * TileSize + y, px);
            if (x == 0 || y == 0) { // faint tile border so streaming is visible
                px[0] = static_cast<std::uint8_t>(px[0] * 3 / 5);
                px[1] = static_cast<std::uint8_t>(px[1] * 3 / 5);
                px[2] = static_cast<std::uint8_t>(px[2] * 3 / 5);
            }
        }
    }
    const std::string label = std::to_string(id.x) + "," + std::to_string(id.y);
    auto stamp = [&](int ox, int oy, std::uint8_t c) {
        forEachTextPixel(label.c_str(), ox, oy, 2, [&](int x, int y, int w, int h) {
            for (int j = y; j < y + h; ++j) {
                for (int i = x; i < x + w; ++i) {
                    if (i < 0 || j < 0 || i >= TileSize || j >= TileSize) continue;
                    std::uint8_t* px = &img.rgba[(static_cast<std::size_t>(j) * TileSize + i) * 4];
                    px[0] = px[1] = px[2] = c;
                }
            }
        });
    };
    stamp(7, 7, 0);
    stamp(6, 6, 255);
    return img;
}

void ensureAssets(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    if (fs::exists(dir / tilePath({0, 0})) && fs::exists(dir / tilePath({WorldTilesX - 1, WorldTilesY - 1}))) return;
    std::printf("Generating %d PNG tiles in %s ...\n", WorldTilesX * WorldTilesY, dir.string().c_str());
    fs::create_directories(dir);
    for (int ty = 0; ty < WorldTilesY; ++ty) {
        for (int tx = 0; tx < WorldTilesX; ++tx) {
            const Bytes png = encodePng(makeTile({tx, ty}));
            std::ofstream f(dir / tilePath({tx, ty}), std::ios::binary);
            f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        }
    }
}

Camera clampCamera(Camera c) {
    c.x = std::clamp(c.x, 0.0f, static_cast<float>(WorldW - ScreenW));
    c.y = std::clamp(c.y, 0.0f, static_cast<float>(WorldH - ScreenH));
    return c;
}

Camera autopilot(double t) {
    const float rx = (WorldW - ScreenW) * 0.5f, ry = (WorldH - ScreenH) * 0.5f;
    return {rx + rx * 0.97f * static_cast<float>(std::sin(t * 0.23)), ry + ry * 0.97f * static_cast<float>(std::sin(t * 0.31 + 1.0))};
}

TileRect viewportTiles(Camera cam, int pad) {
    cam = clampCamera(cam);
    return {std::max(0, static_cast<int>(std::floor(cam.x / TileSize)) - pad),
            std::max(0, static_cast<int>(std::floor(cam.y / TileSize)) - pad),
            std::min(WorldTilesX - 1, static_cast<int>(std::floor((cam.x + ScreenW - 1) / TileSize)) + pad),
            std::min(WorldTilesY - 1, static_cast<int>(std::floor((cam.y + ScreenH - 1) / TileSize)) + pad)};
}

std::vector<PlannedTile> planTiles(Camera cam, float vx, float vy, const PlanSettings& s) {
    std::unordered_map<int, int> best; // tile key -> highest priority
    auto offer = [&best](TileId t, int priority) {
        auto it = best.find(t.key());
        if (it == best.end() || it->second < priority) best[t.key()] = priority;
    };

    cam = clampCamera(cam);
    const TileRect view = viewportTiles(cam);
    const float cx = cam.x + ScreenW * 0.5f, cy = cam.y + ScreenH * 0.5f;
    for (int ty = view.y0; ty <= view.y1; ++ty) {
        for (int tx = view.x0; tx <= view.x1; ++tx) {
            const float dx = (tx + 0.5f) * TileSize - cx, dy = (ty + 0.5f) * TileSize - cy;
            const int nearness = std::max(0, 99 - static_cast<int>(std::sqrt(dx * dx + dy * dy) / 12.0f));
            offer({tx, ty}, band::Visible + nearness);
        }
    }
    if (s.predictive) {
        // Project the viewport forward along the velocity; tiles needed sooner rank higher.
        constexpr int Samples = 4;
        for (int i = 1; i <= Samples; ++i) {
            const float tau = s.horizonSeconds * static_cast<float>(i) / Samples;
            const TileRect ahead = viewportTiles({cam.x + vx * tau, cam.y + vy * tau});
            const int priority = band::Predicted + (Samples - i) * 50;
            for (int ty = ahead.y0; ty <= ahead.y1; ++ty)
                for (int tx = ahead.x0; tx <= ahead.x1; ++tx) offer({tx, ty}, priority);
        }
        const TileRect ring = viewportTiles(cam, s.marginTiles);
        for (int ty = ring.y0; ty <= ring.y1; ++ty)
            for (int tx = ring.x0; tx <= ring.x1; ++tx) offer({tx, ty}, band::Margin);
    }

    std::vector<PlannedTile> out;
    out.reserve(best.size());
    for (const auto& [key, pr] : best) out.push_back({tileFromKey(key), pr});
    std::sort(out.begin(), out.end(), [](const PlannedTile& a, const PlannedTile& b) { return a.priority != b.priority ? a.priority > b.priority : a.id.key() < b.id.key(); });
    if (out.size() > s.maxTiles) out.resize(s.maxTiles);
    return out;
}

void TileStreamer::update(const std::vector<PlannedTile>& plan, const std::function<bool(TileId)>& isResident) {
    ++frame_;
    std::unordered_map<int, int> wanted;
    for (const PlannedTile& p : plan) {
        if (!isResident(p.id)) wanted[p.id.key()] = p.priority;
    }

    // Cancel requests the plan no longer wants. cancel() only succeeds while the load is still queued.
    for (auto it = requested_.begin(); it != requested_.end();) {
        if (wanted.count(it->first)) { ++it; continue; }
        ++cancelsIssued_;
        if (manager_.cancel(tilePath(tileFromKey(it->first)))) ++cancelsWon_;
        it = requested_.erase(it);
    }

    // Request what is new; promote what became more urgent.
    for (const PlannedTile& p : plan) {
        const int key = p.id.key();
        if (!wanted.count(key)) continue;
        const auto found = requested_.find(key);
        if (found != requested_.end()) {
            if (p.priority > found->second.priority) { // joins the in-flight load and promotes it
                manager_.request(tilePath(p.id), p.priority);
                found->second.priority = p.priority;
            }
            continue;
        }
        const auto failed = failedAt_.find(key);
        if (failed != failedAt_.end() && frame_ - failed->second < 120) continue; // back off after a failure
        // A prefetched tile that just arrived is warm in the cache; do not re-request it every frame
        // while it waits to come into range. Visible tiles are always requested (a cache hit is cheap).
        const auto warm = deliveredAt_.find(key);
        if (p.priority < band::Visible && warm != deliveredAt_.end() && frame_ - warm->second < 90) continue;
        const std::uint64_t serial = nextSerial_++;
        requested_[key] = {p.priority, serial};
        ++requestsIssued_;
        manager_.request(tilePath(p.id), p.priority, [this, key, serial](const Result& r) {
            const auto cur = requested_.find(key);
            const bool current = cur != requested_.end() && cur->second.serial == serial;
            if (r.asset) {
                deliveredAt_[key] = frame_;
                ready_.push_back({tileFromKey(key), r.asset});
                if (current) requested_.erase(cur);
                return;
            }
            try {
                std::rethrow_exception(r.error);
            } catch (const LoadCancelled&) {
                // We cancelled it (and already dropped the record); a re-request has a newer serial.
            } catch (...) {
                ++failures_;
                failedAt_[key] = frame_;
                if (current) requested_.erase(cur);
            }
        });
    }
}

std::vector<ReadyTile> TileStreamer::collect(std::size_t maxTiles) {
    manager_.pumpCompletions();
    std::vector<ReadyTile> out;
    while (!ready_.empty() && out.size() < maxTiles) {
        out.push_back(std::move(ready_.front()));
        ready_.pop_front();
    }
    return out;
}

std::vector<ReadyTile> UploadQueue::take(Camera cam, std::size_t budget, const std::function<bool(TileId)>& isResident) {
    const TileRect range = uploadRange(cam);
    std::vector<ReadyTile> out;
    std::deque<ReadyTile> keep;
    for (ReadyTile& t : queue_) {
        if (!range.contains(t.id) || isResident(t.id)) continue; // not needed (any more): drop
        if (out.size() < budget) out.push_back(std::move(t));
        else keep.push_back(std::move(t));
    }
    queue_ = std::move(keep);
    return out;
}

} // namespace streamer::tiles
