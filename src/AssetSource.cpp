#include "streamer/AssetSource.hpp"

#include "streamer/Png.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <thread>

namespace streamer {
namespace {
std::uint64_t fnv1a(const std::uint8_t* data, std::size_t n, std::uint64_t h = 14695981039346656037ULL) {
    for (std::size_t i = 0; i < n; ++i) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}
} // namespace

AssetType typeFromPath(const std::string& path) {
    const std::size_t dot = path.rfind('.');
    if (dot == std::string::npos) return AssetType::Data;
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (ext == "png" || ext == "jpg" || ext == "tex") return AssetType::Texture;
    if (ext == "wav" || ext == "ogg" || ext == "mp3") return AssetType::Audio;
    return AssetType::Data;
}

std::uint64_t checksum(const Bytes& bytes) { return fnv1a(bytes.data(), bytes.size()); }

std::shared_ptr<const Asset> decodeAsset(const std::string& path, Bytes bytes) {
    auto asset = std::make_shared<Asset>();
    asset->path = path;
    asset->type = typeFromPath(path);
    const std::size_t dot = path.rfind('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    if (ext == "png") {
        Image image = decodePng(bytes);
        asset->width = image.width;
        asset->height = image.height;
        bytes = std::move(image.rgba);
    }
    asset->checksum = checksum(bytes);
    asset->bytes = std::move(bytes);
    return asset;
}

FileSource::FileSource(std::string root) : root_(std::move(root)) {}

Bytes FileSource::read(const std::string& path) {
    namespace fs = std::filesystem;
    const fs::path relative = fs::path(path).lexically_normal();
    // On Windows "/x" and "C:x" are not is_absolute() but still escape the root, so test the parts.
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || relative.has_root_directory() ||
        *relative.begin() == "..") {
        throw std::invalid_argument("asset path escapes the asset root: " + path);
    }
    std::ifstream file(fs::path(root_) / relative, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open asset: " + path);
    return Bytes(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

Bytes LatencySource::read(const std::string& path) {
    if (latency_.count() > 0) std::this_thread::sleep_for(latency_);
    return inner_->read(path);
}

Bytes SyntheticSource::generate(const std::string& path, std::size_t size) {
    std::uint64_t state = fnv1a(reinterpret_cast<const std::uint8_t*>(path.data()), path.size()) | 1ULL;
    Bytes out(size);
    for (std::size_t i = 0; i < size; ++i) {
        state ^= state << 13; // xorshift64: cheap, deterministic, does real per-byte CPU work
        state ^= state >> 7;
        state ^= state << 17;
        out[i] = static_cast<std::uint8_t>(state >> 24);
    }
    return out;
}

Bytes SyntheticSource::read(const std::string& path) {
    if (latency_.count() > 0) std::this_thread::sleep_for(latency_);
    return generate(path, assetBytes_);
}

} // namespace streamer
