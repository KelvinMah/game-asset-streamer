#pragma once

#include "streamer/AssetManager.hpp"
#include "streamer/Png.hpp"

#include <deque>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// A tile world to stream: its geometry, procedural PNG generation, a camera-motion predictor, and
// a client that turns predictions into prioritised requests and cancellations. Shared by the demo
// and the frame-time benchmark so both exercise exactly the same streaming logic.
namespace streamer::tiles {

constexpr int ScreenW = 960, ScreenH = 540;
constexpr int TileSize = 128;
constexpr int WorldTilesX = 32, WorldTilesY = 20;
constexpr int WorldW = WorldTilesX * TileSize, WorldH = WorldTilesY * TileSize;
constexpr std::size_t DecodedTileBytes = static_cast<std::size_t>(TileSize) * TileSize * 4;

struct TileId {
    int x = 0, y = 0;
    int key() const { return y * WorldTilesX + x; }
    bool operator==(const TileId& o) const { return x == o.x && y == o.y; }
};
inline TileId tileFromKey(int key) { return {key % WorldTilesX, key / WorldTilesX}; }
std::string tilePath(TileId id);

// ---- world content -----------------------------------------------------------------------------

Image makeTile(TileId id);                              // seamless terrain with the tile's coordinates stamped on it
void ensureAssets(const std::filesystem::path& dir);    // writes all PNG tiles unless they already exist

// ---- camera ------------------------------------------------------------------------------------

struct Camera {
    float x = 0, y = 0; // top-left of the viewport in world pixels
};
Camera clampCamera(Camera c);
Camera autopilot(double seconds); // deterministic scripted tour: identical path for every benchmark mode

// Inclusive tile rectangle covering the viewport at `cam`, grown by `pad` tiles and clipped to the world.
struct TileRect {
    int x0, y0, x1, y1;
    bool contains(TileId t) const { return t.x >= x0 && t.x <= x1 && t.y >= y0 && t.y <= y1; }
};
TileRect viewportTiles(Camera cam, int pad = 0);

// ---- prediction --------------------------------------------------------------------------------

// Priority bands (any int is a valid priority; larger runs first). Visible tiles always outrank
// predicted ones, which outrank the margin ring.
namespace band {
constexpr int Visible = 1000;   // + 0..99, nearer the screen centre = higher
constexpr int Predicted = 300;  // + 0..200, sooner = higher
constexpr int Margin = 100;
} // namespace band

struct PlannedTile {
    TileId id;
    int priority = 0;
};

struct PlanSettings {
    bool predictive = true;     // false: request only what is visible right now
    float horizonSeconds = 0.8f;
    int marginTiles = 1;        // always-warm ring around the viewport
    // Upper bound on tiles planned (lowest priorities dropped first). Keep it below the cache capacity:
    // a plan larger than the cache makes predicted tiles evict each other and the visible ones.
    std::size_t maxTiles = 80;
};

// Residency policy shared by clients: upload a delivered tile only inside uploadRange(cam), and release
// GPU tiles only outside keepRange(cam). keepRange contains uploadRange, so nothing is uploaded just to
// be evicted on the next frame.
inline TileRect uploadRange(Camera cam) { return viewportTiles(cam, 1); }
inline TileRect keepRange(Camera cam) { return viewportTiles(cam, 2); }

// Which tiles should be loaded or kept loading, with priorities. The viewport is projected along
// the camera velocity at several look-ahead times; a tile needed sooner gets a higher priority.
std::vector<PlannedTile> planTiles(Camera cam, float velX, float velY, const PlanSettings& settings);

// ---- client ------------------------------------------------------------------------------------

struct ReadyTile {
    TileId id;
    AssetPtr asset;
};

// Drives an AssetManager from successive plans (main thread only). Each update() requests new
// tiles, promotes ones whose priority rose, and CANCELS requests that dropped out of the plan, which
// is what keeps a fast-moving camera from drowning workers in tiles it has already flown past.
class TileStreamer {
public:
    explicit TileStreamer(AssetManager& manager) : manager_(manager) {}

    // `isResident` tells the streamer which tiles the caller already holds (they are never requested).
    void update(const std::vector<PlannedTile>& plan, const std::function<bool(TileId)>& isResident);

    // Delivers finished loads (runs the manager's completion callbacks) and returns up to maxTiles.
    std::vector<ReadyTile> collect(std::size_t maxTiles);

    std::size_t requestedCount() const { return requested_.size(); }
    std::size_t readyCount() const { return ready_.size(); }
    std::uint64_t requestsIssued() const { return requestsIssued_; }
    std::uint64_t cancelsIssued() const { return cancelsIssued_; }
    std::uint64_t cancelsWon() const { return cancelsWon_; }
    std::uint64_t failures() const { return failures_; }

private:
    struct Requested {
        int priority;
        std::uint64_t serial; // distinguishes a re-request from a stale callback of a cancelled one
    };

    AssetManager& manager_;
    std::unordered_map<int, Requested> requested_;
    std::unordered_map<int, std::uint64_t> failedAt_; // frame of last failure, for retry backoff
    std::unordered_map<int, std::uint64_t> deliveredAt_; // frame a tile last arrived (it is warm in the cache)
    std::deque<ReadyTile> ready_;
    std::uint64_t frame_ = 0, nextSerial_ = 1;
    std::uint64_t requestsIssued_ = 0, cancelsIssued_ = 0, cancelsWon_ = 0, failures_ = 0;
};

// Decoded tiles waiting for a GPU upload slot (main thread only). take() enforces the residency policy:
// tiles outside uploadRange() or already resident are discarded (the decoded data stays in the cache
// if they are needed later), and at most `budget` tiles are handed out per call so a burst of finished
// loads cannot hitch a frame. Tiles over budget stay queued for the next frame.
class UploadQueue {
public:
    void add(ReadyTile tile) { queue_.push_back(std::move(tile)); }
    std::vector<ReadyTile> take(Camera cam, std::size_t budget, const std::function<bool(TileId)>& isResident);
    std::size_t size() const { return queue_.size(); }

private:
    std::deque<ReadyTile> queue_;
};

} // namespace streamer::tiles
