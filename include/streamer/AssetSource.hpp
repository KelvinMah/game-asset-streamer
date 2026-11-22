#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace streamer {

using Bytes = std::vector<std::uint8_t>;

enum class AssetType { Texture, Audio, Data };

// Classifies by file extension (case-insensitive): .png/.jpg/.tex -> Texture, .wav/.ogg/.mp3 -> Audio.
AssetType typeFromPath(const std::string& path);

// 64-bit FNV-1a over the payload; used as the integrity check in tests and benchmarks.
std::uint64_t checksum(const Bytes& bytes);

// A decoded, immutable asset. Shared by pointer between the cache and every consumer.
struct Asset {
    std::string path;
    AssetType type = AssetType::Data;
    Bytes bytes;             // decoded payload: RGBA8 pixels for .png, raw file bytes otherwise
    int width = 0;           // pixel size for decoded images, else 0
    int height = 0;
    std::uint64_t checksum = 0;
    std::size_t cost() const { return bytes.size(); } // bytes charged against the cache budget
};

// Turns raw bytes into an Asset: classifies it, decodes .png files to RGBA pixels (throws on a bad
// image), and computes the checksum. Runs on worker threads.
std::shared_ptr<const Asset> decodeAsset(const std::string& path, Bytes bytes);

// Where raw bytes come from. read() is called concurrently from worker threads, so implementations
// must be thread-safe. It throws on failure.
class AssetSource {
public:
    virtual ~AssetSource() = default;
    virtual Bytes read(const std::string& path) = 0;
};

// Reads files under a root directory. Absolute paths and ".." escapes are rejected.
class FileSource : public AssetSource {
public:
    explicit FileSource(std::string root);
    Bytes read(const std::string& path) override;

private:
    std::string root_;
};

// Wraps another source and adds a fixed delay per read, to model slow storage (spinning disk,
// network mount, console optical drive) on top of real file reads.
class LatencySource : public AssetSource {
public:
    LatencySource(std::shared_ptr<AssetSource> inner, std::chrono::microseconds latency)
        : inner_(std::move(inner)), latency_(latency) {}
    Bytes read(const std::string& path) override;

private:
    std::shared_ptr<AssetSource> inner_;
    std::chrono::microseconds latency_;
};

// Deterministic generated data with optional simulated storage latency. Used by the benchmark and
// demo so they need no asset files; `generate` lets tests recompute the expected payload.
class SyntheticSource : public AssetSource {
public:
    SyntheticSource(std::size_t assetBytes, std::chrono::microseconds latency)
        : assetBytes_(assetBytes), latency_(latency) {}
    Bytes read(const std::string& path) override;
    static Bytes generate(const std::string& path, std::size_t size);

private:
    std::size_t assetBytes_;
    std::chrono::microseconds latency_;
};

} // namespace streamer
