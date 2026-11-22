#include "streamer/TileWorld.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <thread>
#include <unordered_set>

using namespace streamer;
using namespace streamer::tiles;
using namespace std::chrono;

namespace {

// Serves a tiny valid PNG per tile whose first pixel encodes the tile id, so tests can verify that
// every tile that comes back is the tile that was asked for. Optional latency and per-tile failure.
class TileSource : public AssetSource {
public:
    explicit TileSource(microseconds delay = microseconds(0), int failX = -1, int failY = -1) : delay_(delay), failX_(failX), failY_(failY) {}
    Bytes read(const std::string& path) override {
        int x = -1, y = -1;
        if (std::sscanf(path.c_str(), "tile_%d_%d.png", &x, &y) != 2) throw std::runtime_error("bad tile path " + path);
        ++reads;
        {
            std::lock_guard<std::mutex> lock(m_);
            readPaths.insert(path);
            ++readCount[path];
        }
        if (delay_.count() > 0) std::this_thread::sleep_for(delay_);
        if (x == failX_ && y == failY_) throw std::runtime_error("disk error");
        Image img;
        img.width = img.height = 8;
        img.rgba.assign(8 * 8 * 4, 0);
        for (int i = 0; i < 8 * 8; ++i) {
            img.rgba[i * 4] = static_cast<std::uint8_t>(x);
            img.rgba[i * 4 + 1] = static_cast<std::uint8_t>(y);
            img.rgba[i * 4 + 3] = 255;
        }
        return encodePng(img);
    }
    std::atomic<int> reads{0};
    std::mutex m_;
    std::set<std::string> readPaths;
    std::map<std::string, int> readCount;

private:
    microseconds delay_;
    int failX_, failY_;
};

bool correctTile(const ReadyTile& t) {
    return t.asset && t.asset->width == 8 && t.asset->bytes[0] == static_cast<std::uint8_t>(t.id.x) && t.asset->bytes[1] == static_cast<std::uint8_t>(t.id.y);
}

std::vector<PlannedTile> rectPlan(int x0, int y0, int x1, int y1, int priority) {
    std::vector<PlannedTile> plan;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) plan.push_back({{x, y}, priority});
    return plan;
}

const auto neverResident = [](TileId) { return false; };

void waitForActive(AssetManager& m, std::size_t n) {
    for (int spin = 0; spin < 5000 && m.stats().activeLoads < n; ++spin) std::this_thread::sleep_for(milliseconds(1));
    ASSERT_EQ(m.stats().activeLoads, n);
}

} // namespace

// ---- planner ---------------------------------------------------------------------------------------

TEST(Planner, StationaryCameraPlansTheViewportAndAMarginRingOnly) {
    const Camera cam{1000, 800};
    const auto plan = planTiles(cam, 0, 0, PlanSettings{});
    const TileRect view = viewportTiles(cam), ring = viewportTiles(cam, 1);
    for (const PlannedTile& p : plan) {
        EXPECT_TRUE(ring.contains(p.id));
        if (view.contains(p.id)) EXPECT_GE(p.priority, band::Visible);
        else EXPECT_EQ(p.priority, band::Margin);
    }
    const int visibleCount = (view.x1 - view.x0 + 1) * (view.y1 - view.y0 + 1);
    const int ringCount = (ring.x1 - ring.x0 + 1) * (ring.y1 - ring.y0 + 1);
    EXPECT_EQ(static_cast<int>(plan.size()), ringCount);
    EXPECT_GT(ringCount, visibleCount);
}

TEST(Planner, MovingRightPrioritisesTilesAheadOverTilesBehind) {
    const Camera cam{1500, 800};
    const auto plan = planTiles(cam, 700, 0, PlanSettings{});
    const TileRect view = viewportTiles(cam);
    int aheadBest = 0, behindBest = 0;
    bool anyAhead = false;
    for (const PlannedTile& p : plan) {
        if (view.contains(p.id)) continue;
        if (p.id.x > view.x1) { anyAhead = true; aheadBest = std::max(aheadBest, p.priority); }
        if (p.id.x < view.x0) behindBest = std::max(behindBest, p.priority);
    }
    EXPECT_TRUE(anyAhead);
    EXPECT_GT(aheadBest, band::Predicted - 1);   // ahead tiles are in the predicted band
    EXPECT_EQ(behindBest, band::Margin);         // behind tiles get only the margin priority
    EXPECT_GT(aheadBest, behindBest);
}

TEST(Planner, VisibleTilesAlwaysOutrankPredictedOnes) {
    const Camera cam{1200, 700};
    int minVisible = 1 << 30, maxOther = 0;
    const TileRect view = viewportTiles(cam);
    for (const PlannedTile& p : planTiles(cam, -500, 400, PlanSettings{})) {
        if (view.contains(p.id)) minVisible = std::min(minVisible, p.priority);
        else maxOther = std::max(maxOther, p.priority);
    }
    EXPECT_GT(minVisible, maxOther);
}

TEST(Planner, SoonerTilesGetHigherPriorityAndNearCentreBeatsEdge) {
    const auto plan = planTiles({1000, 800}, 800, 0, PlanSettings{});
    std::unordered_map<int, int> pr;
    for (const auto& p : plan) pr[p.id.key()] = p.priority;
    const TileRect view = viewportTiles({1000, 800});
    auto at = [&pr](int x, int y) { return pr[TileId{x, y}.key()]; };
    // A tile just beyond the right edge is needed before one three tiles further along.
    const int nearAhead = at(view.x1 + 1, view.y0 + 1), farAhead = at(view.x1 + 3, view.y0 + 1);
    EXPECT_GE(nearAhead, farAhead);
    // Within the viewport, the centre tile outranks a corner tile.
    const int centre = at((view.x0 + view.x1) / 2, (view.y0 + view.y1) / 2), corner = at(view.x0, view.y0);
    EXPECT_GT(centre, corner);
}

TEST(Planner, StaysInsideTheWorldWithNoDuplicates) {
    for (const Camera cam : {Camera{0, 0}, Camera{static_cast<float>(WorldW), static_cast<float>(WorldH)}, Camera{-50, 9999}}) {
        for (const auto& v : {std::pair<float, float>{-2000, -2000}, {2000, 2000}, {0, 0}}) {
            std::unordered_set<int> seen;
            for (const PlannedTile& p : planTiles(cam, v.first, v.second, PlanSettings{})) {
                EXPECT_GE(p.id.x, 0);
                EXPECT_LT(p.id.x, WorldTilesX);
                EXPECT_GE(p.id.y, 0);
                EXPECT_LT(p.id.y, WorldTilesY);
                EXPECT_TRUE(seen.insert(p.id.key()).second);
            }
        }
    }
}

TEST(Planner, NonPredictiveModeRequestsOnlyVisibleTiles) {
    PlanSettings s;
    s.predictive = false;
    const Camera cam{1500, 800};
    const TileRect view = viewportTiles(cam);
    const auto plan = planTiles(cam, 900, 0, s);
    EXPECT_EQ(static_cast<int>(plan.size()), (view.x1 - view.x0 + 1) * (view.y1 - view.y0 + 1));
    for (const auto& p : plan) EXPECT_TRUE(view.contains(p.id));
}

TEST(Planner, IsDeterministic) {
    const auto a = planTiles({900, 600}, 321, -123, PlanSettings{});
    const auto b = planTiles({900, 600}, 321, -123, PlanSettings{});
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_TRUE(a[i].id == b[i].id);
        EXPECT_EQ(a[i].priority, b[i].priority);
    }
}

// ---- streamer ---------------------------------------------------------------------------------------

TEST(TileStreamer, LoadsPlannedTilesAndTheyAreTheRightOnes) {
    auto source = std::make_shared<TileSource>();
    AssetManager manager(source, {2, 1 << 20});
    TileStreamer streamer(manager);
    streamer.update(rectPlan(2, 3, 5, 4, 1000), neverResident);
    std::set<int> got;
    for (int spin = 0; spin < 2000 && got.size() < 8; ++spin) {
        for (const ReadyTile& t : streamer.collect(100)) {
            EXPECT_TRUE(correctTile(t));
            got.insert(t.id.key());
        }
        std::this_thread::sleep_for(milliseconds(1));
    }
    EXPECT_EQ(got.size(), 8U);
    EXPECT_EQ(streamer.requestedCount(), 0U);
}

TEST(TileStreamer, ResidentTilesAreNeverRequested) {
    auto source = std::make_shared<TileSource>();
    AssetManager manager(source, {2, 1 << 20});
    TileStreamer streamer(manager);
    streamer.update(rectPlan(0, 0, 3, 0, 1000), [](TileId t) { return t.x < 2; });
    EXPECT_EQ(streamer.requestsIssued(), 2U);
}

TEST(TileStreamer, CancelsQueuedRequestsThatLeftThePlan) {
    auto source = std::make_shared<TileSource>(milliseconds(40));
    AssetManager manager(source, {1, 0});
    TileStreamer streamer(manager);
    streamer.update(rectPlan(0, 0, 9, 0, 1000), neverResident); // ten tiles, one slow worker
    waitForActive(manager, 1);
    streamer.update(rectPlan(20, 10, 21, 10, 1000), neverResident); // camera jumped far away
    EXPECT_GE(streamer.cancelsWon(), 8U); // all but the tile already being read were still queued
    EXPECT_EQ(streamer.cancelsIssued(), 10U);

    std::set<int> got;
    for (int spin = 0; spin < 3000 && got.size() < 2; ++spin) {
        for (const ReadyTile& t : streamer.collect(100))
            if (t.id.x >= 20) got.insert(t.id.key());
        std::this_thread::sleep_for(milliseconds(1));
    }
    EXPECT_EQ(got.size(), 2U);
    EXPECT_LE(source->reads.load(), 4); // far fewer than the 12 tiles requested overall
}

TEST(TileStreamer, RaisedPriorityPromotesTheQueuedLoad) {
    auto source = std::make_shared<TileSource>(milliseconds(30));
    AssetManager manager(source, {1, 0});
    TileStreamer streamer(manager);
    streamer.update(rectPlan(0, 0, 5, 0, 100), neverResident);
    waitForActive(manager, 1);
    streamer.update({{{5, 0}, 2000}}, [](TileId t) { return t.x != 5; }); // only (5,0) remains wanted, now urgent
    EXPECT_EQ(manager.stats().boosts, 1U);
}

TEST(TileStreamer, ReRequestAfterCancelDeliversExactlyOneTile) {
    auto source = std::make_shared<TileSource>(milliseconds(40));
    AssetManager manager(source, {1, 0});
    TileStreamer streamer(manager);
    streamer.update(rectPlan(0, 0, 0, 0, 1000), neverResident); // occupies the only worker
    waitForActive(manager, 1);
    streamer.update(rectPlan(0, 0, 1, 0, 500), neverResident);       // (1,0) queues behind it
    streamer.update(rectPlan(0, 0, 0, 0, 1000), neverResident);      // (1,0) dropped: cancelled while queued
    streamer.update(rectPlan(0, 0, 1, 0, 500), neverResident);       // and wanted again: a fresh request
    int tile10 = 0, tile00 = 0;
    for (int spin = 0; spin < 3000 && tile10 == 0; ++spin) {
        for (const ReadyTile& t : streamer.collect(100)) {
            EXPECT_TRUE(correctTile(t));
            (t.id.x == 1 ? tile10 : tile00)++;
        }
        std::this_thread::sleep_for(milliseconds(1));
    }
    for (const ReadyTile& t : streamer.collect(100)) (t.id.x == 1 ? tile10 : tile00)++;
    EXPECT_EQ(tile10, 1); // the stale callback of the cancelled request did not clobber or duplicate it
    EXPECT_EQ(tile00, 1);
    EXPECT_EQ(streamer.requestedCount(), 0U);
}

TEST(TileStreamer, FailedTilesBackOffInsteadOfBeingHammered) {
    auto source = std::make_shared<TileSource>(microseconds(0), 3, 3);
    AssetManager manager(source, {1, 1 << 20});
    TileStreamer streamer(manager);
    const auto plan = rectPlan(3, 3, 3, 3, 1000);
    for (int frame = 0; frame < 100; ++frame) {
        streamer.update(plan, neverResident);
        streamer.collect(10);
        std::this_thread::sleep_for(milliseconds(1));
    }
    EXPECT_EQ(source->reads.load(), 1); // one attempt, then backoff (retry only after 120 frames)
    EXPECT_EQ(streamer.failures(), 1U);
    EXPECT_EQ(streamer.requestedCount(), 0U);
}

TEST(TileStreamer, FrantickCameraMovementStaysConsistent) {
    // A camera that teleports, reverses and sprints for thousands of frames against real worker threads.
    auto source = std::make_shared<TileSource>(microseconds(150));
    AssetManager manager(source, {3, 40 * 8 * 8 * 4});
    TileStreamer streamer(manager);
    std::unordered_set<int> resident;
    std::uint32_t rng = 2463534242u;
    auto next = [&rng] { rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng; };
    Camera cam{1000, 600};
    float vx = 0, vy = 0;
    std::size_t maxRequested = 0;
    int wrong = 0, delivered = 0;

    for (int frame = 0; frame < 4000; ++frame) {
        const std::uint32_t r = next();
        if (r % 97 == 0) cam = {static_cast<float>(next() % WorldW), static_cast<float>(next() % WorldH)}; // teleport
        if (r % 31 == 0) { vx = static_cast<float>(static_cast<int>(next() % 3000) - 1500); vy = static_cast<float>(static_cast<int>(next() % 2000) - 1000); }
        cam = clampCamera({cam.x + vx / 60.0f, cam.y + vy / 60.0f});
        const auto plan = planTiles(cam, vx, vy, PlanSettings{});
        streamer.update(plan, [&](TileId t) { return resident.count(t.key()) > 0; });
        maxRequested = std::max(maxRequested, streamer.requestedCount());
        EXPECT_LE(streamer.requestedCount(), plan.size()); // requests never outlive the plan
        for (const ReadyTile& t : streamer.collect(4)) {
            ++delivered;
            if (!correctTile(t)) ++wrong;
            resident.insert(t.id.key());
        }
        // evict far-away "GPU" tiles
        const TileRect keep = viewportTiles(cam, 2);
        for (auto it = resident.begin(); it != resident.end();) it = keep.contains(tileFromKey(*it)) ? std::next(it) : resident.erase(it);
    }

    // Stop wanting anything and let everything settle.
    for (int i = 0; i < 3000; ++i) {
        streamer.update({}, neverResident);
        streamer.collect(1000);
        const Stats s = manager.stats();
        if (s.activeLoads == 0 && s.queuedLoads == 0) break;
        std::this_thread::sleep_for(milliseconds(1));
    }
    streamer.collect(1000);
    const Stats s = manager.stats();
    EXPECT_EQ(wrong, 0);
    EXPECT_GT(delivered, 100);
    EXPECT_EQ(s.loadsStarted, s.loadsCompleted + s.loadsFailed + s.cancelled);
    EXPECT_EQ(s.loadsFailed, 0U);
    EXPECT_EQ(s.activeLoads, 0U);
    EXPECT_EQ(s.queuedLoads, 0U);
    EXPECT_EQ(streamer.requestedCount(), 0U);
    EXPECT_GT(s.cancelled, 0U); // fast movement really did make obsolete requests that were cancelled
}

// ---- plan size, warm tiles and upload policy (regressions found by the frame benchmark) -------------

TEST(Planner, PlanNeverExceedsTheTileBudgetAndKeepsTheMostUrgentTiles) {
    PlanSettings s;
    s.maxTiles = 60;
    const Camera cam{1500, 900};
    const auto plan = planTiles(cam, 2500, 1500, s); // very fast camera: the raw prediction is huge
    EXPECT_LE(plan.size(), 60U);
    const TileRect view = viewportTiles(cam);
    int visibleKept = 0;
    for (const auto& p : plan) visibleKept += view.contains(p.id);
    EXPECT_EQ(visibleKept, (view.x1 - view.x0 + 1) * (view.y1 - view.y0 + 1)); // every visible tile survived the cut
    for (std::size_t i = 1; i < plan.size(); ++i) EXPECT_GE(plan[i - 1].priority, plan[i].priority); // cut by priority
}

TEST(Planner, DefaultBudgetFitsComfortablyInTheDemoCache) {
    EXPECT_LE(PlanSettings{}.maxTiles, 90U); // the demo and benchmark cache holds 90 tiles
}

TEST(TileStreamer, WarmPrefetchedTilesAreNotReRequestedEveryFrame) {
    auto source = std::make_shared<TileSource>();
    AssetManager manager(source, {2, 1 << 20});
    TileStreamer streamer(manager);
    const std::vector<PlannedTile> prefetch = {{{4, 4}, band::Predicted + 100}};
    streamer.update(prefetch, neverResident);
    for (int spin = 0; spin < 2000 && streamer.readyCount() == 0; ++spin) {
        streamer.collect(0); // run completions without taking tiles
        std::this_thread::sleep_for(milliseconds(1));
    }
    ASSERT_EQ(streamer.readyCount(), 1U);
    streamer.collect(10); // the client consumed it but chose not to upload (still out of range)
    for (int frame = 0; frame < 60; ++frame) streamer.update(prefetch, neverResident);
    EXPECT_EQ(streamer.requestsIssued(), 1U); // no per-frame churn
    streamer.update({{{4, 4}, band::Visible + 50}}, neverResident); // but once it is visible it is requested immediately
    EXPECT_EQ(streamer.requestsIssued(), 2U);
}

TEST(UploadQueue, EnforcesBudgetRangeAndResidency) {
    const Camera cam{1000, 600};
    const TileRect range = uploadRange(cam);
    auto mk = [](int x, int y) { return ReadyTile{{x, y}, nullptr}; };
    UploadQueue q;
    q.add(mk(range.x0, range.y0));
    q.add(mk(range.x0 + 1, range.y0));
    q.add(mk(range.x0 + 2, range.y0));
    q.add(mk(range.x1 + 5, range.y1));       // outside the upload range: must be dropped
    q.add(mk(range.x0, range.y0 + 1));       // already resident: must be dropped
    const auto resident = [&](TileId t) { return t.x == range.x0 && t.y == range.y0 + 1; };
    const auto first = q.take(cam, 2, resident);
    ASSERT_EQ(first.size(), 2U);
    EXPECT_EQ(first[0].id.x, range.x0);
    EXPECT_EQ(first[1].id.x, range.x0 + 1);
    EXPECT_EQ(q.size(), 1U); // one in-range tile is waiting for the next frame
    EXPECT_EQ(q.take(cam, 2, resident).size(), 1U);
    EXPECT_EQ(q.size(), 0U);
}

TEST(UploadQueue, UploadRangeIsInsideKeepRangeSoNothingFlaps) {
    for (const Camera cam : {Camera{0, 0}, Camera{1234, 777}, Camera{3000, 2000}}) {
        const TileRect up = uploadRange(cam), keep = keepRange(cam);
        EXPECT_TRUE(keep.contains({up.x0, up.y0}));
        EXPECT_TRUE(keep.contains({up.x1, up.y1}));
    }
}
