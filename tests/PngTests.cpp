#include "streamer/Png.hpp"

#include <gtest/gtest.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace streamer;

namespace {

Bytes readFile(const std::string& name) {
    std::ifstream f(std::string(STREAMER_TEST_DATA) + "/" + name, std::ios::binary);
    return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

Image patternImage(int w, int h) {
    Image img;
    img.width = w;
    img.height = h;
    img.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (std::size_t i = 0; i < img.rgba.size(); ++i) img.rgba[i] = static_cast<std::uint8_t>((i * 7 + i / 13) & 255);
    return img;
}

} // namespace

// The fixtures cover every color type and deflate block type, with expected pixels produced by
// Pillow/libpng (an independent decoder); see tests/data/make_fixtures.py.
TEST(Png, DecodesFixturesIdenticallyToLibpng) {
    const char* names[] = {"rgb_cycle_fixed",  "rgba_paeth_stored", "gray_stored",       "graya_up_dynamic",
                           "palette_avg",      "one_pixel",         "rgb_wide_dynamic",  "rgba_tile_dynamic"};
    for (const char* name : names) {
        const Bytes file = readFile(std::string(name) + ".png");
        const Bytes expected = readFile(std::string(name) + ".rgba");
        ASSERT_FALSE(file.empty());
        const Image img = decodePng(file);
        EXPECT_EQ(img.rgba.size(), expected.size()) << name;
        EXPECT_TRUE(img.rgba == expected) << "pixel mismatch in " << name;
        EXPECT_EQ(static_cast<std::size_t>(img.width) * img.height * 4, expected.size()) << name;
    }
}

TEST(Png, EncodeDecodeRoundTripsForEveryFilter) {
    for (const auto& [w, h] : {std::pair<int, int>{1, 1}, {7, 3}, {64, 64}, {130, 2}, {3, 90}}) {
        const Image src = patternImage(w, h);
        for (int filter = 0; filter <= FilterCycle; ++filter) {
            const Image back = decodePng(encodePng(src, filter));
            EXPECT_EQ(back.width, w);
            EXPECT_EQ(back.height, h);
            EXPECT_TRUE(back.rgba == src.rgba) << w << "x" << h << " filter " << filter;
        }
    }
}

TEST(Png, LargeImageUsesMultipleStoredBlocks) {
    const Image src = patternImage(300, 300); // 360 KB of raw data > one 64 KiB stored block
    EXPECT_TRUE(decodePng(encodePng(src)).rgba == src.rgba);
}

TEST(Png, RejectsNonPngAndTruncatedFiles) {
    EXPECT_THROW(decodePng(Bytes{}), std::runtime_error);
    EXPECT_THROW(decodePng(Bytes(100, 0x41)), std::runtime_error);
    const Bytes good = encodePng(patternImage(16, 16));
    for (std::size_t cut : {std::size_t(8), std::size_t(20), good.size() / 2, good.size() - 1}) {
        EXPECT_THROW(decodePng(Bytes(good.begin(), good.begin() + cut)), std::runtime_error);
    }
}

TEST(Png, DetectsCorruptedData) {
    Bytes good = encodePng(patternImage(16, 16));
    good[good.size() / 2] ^= 0x40; // inside IDAT: chunk CRC no longer matches
    EXPECT_THROW(decodePng(good), std::runtime_error);
}

TEST(Png, RejectsOversizedAndUnsupportedHeaders) {
    auto withHeader = [](std::uint32_t w, std::uint32_t h, std::uint8_t depth, std::uint8_t color, std::uint8_t interlace) {
        Bytes file = encodePng(patternImage(2, 2));
        auto put = [&](std::size_t at, std::uint32_t v) {
            for (int i = 0; i < 4; ++i) file[at + i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
        };
        put(16, w);
        put(20, h);
        file[24] = depth;
        file[25] = color;
        file[28] = interlace;
        // Recompute the IHDR CRC so only the semantic check can reject it.
        std::uint32_t crc = 0xFFFFFFFFU;
        for (std::size_t i = 12; i < 29; ++i) {
            crc ^= file[i];
            for (int k = 0; k < 8; ++k) crc = (crc & 1U) ? 0xEDB88320U ^ (crc >> 1) : crc >> 1;
        }
        put(29, ~crc);
        return file;
    };
    EXPECT_NO_THROW((void)decodePng(withHeader(2, 2, 8, 6, 0))); // sanity: helper keeps a valid file valid
    EXPECT_THROW(decodePng(withHeader(100000, 100000, 8, 6, 0)), std::runtime_error); // decompression bomb size
    EXPECT_THROW(decodePng(withHeader(2, 2, 16, 6, 0)), std::runtime_error);          // 16-bit
    EXPECT_THROW(decodePng(withHeader(2, 2, 8, 6, 1)), std::runtime_error);           // interlaced
    EXPECT_THROW(decodePng(withHeader(2, 2, 8, 5, 0)), std::runtime_error);           // invalid color type
}

// Robustness: arbitrary single-byte damage must end in an exception or a decoded image, never a
// crash or out-of-bounds access (this test is meant to be run under AddressSanitizer/UBSan).
TEST(Png, SurvivesEveryTruncationAndManyBitFlips) {
    const Bytes fixture = readFile("rgba_tile_dynamic.png"); // real deflate stream with dynamic Huffman codes
    ASSERT_FALSE(fixture.empty());
    for (std::size_t cut = 0; cut < fixture.size(); ++cut) {
        try { decodePng(Bytes(fixture.begin(), fixture.begin() + cut)); } catch (const std::runtime_error&) {}
    }
    std::uint32_t rng = 12345;
    for (int i = 0; i < 3000; ++i) {
        rng = rng * 1664525U + 1013904223U;
        Bytes damaged = fixture;
        damaged[(rng >> 8) % damaged.size()] ^= static_cast<std::uint8_t>(1U << (rng % 8));
        try { decodePng(damaged); } catch (const std::runtime_error&) {}
    }
    SUCCEED();
}
