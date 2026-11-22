#pragma once

#include "streamer/AssetSource.hpp"

namespace streamer {

// Decoded image, always 8-bit RGBA, row-major, top row first.
struct Image {
    int width = 0;
    int height = 0;
    Bytes rgba;
};

// Self-contained PNG decoder (own inflate, no zlib dependency). Supports non-interlaced 8-bit
// grayscale, gray+alpha, RGB, RGBA and palette images. Verifies chunk CRCs and the zlib Adler-32,
// rejects images over 16384 px per side or 2^25 pixels, and never writes outside its buffers on
// malformed input. Throws std::runtime_error on invalid or unsupported files.
Image decodePng(const Bytes& file);

// Filter applied by encodePng: 0-4 use that PNG filter on every row; 5 cycles 0..4 by row so the
// decoder's unfiltering is exercised.
constexpr int FilterCycle = 5;

// Writes a valid PNG using uncompressed ("stored") deflate blocks. For generating test and demo
// assets; not a compressor.
Bytes encodePng(const Image& image, int filter = FilterCycle);

} // namespace streamer
