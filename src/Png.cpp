#include "streamer/Png.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <string>
#include <stdexcept>

namespace streamer {
namespace {

[[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string("png: ") + what); }

// ---- checksums --------------------------------------------------------------------------------

std::uint32_t crc32(const std::uint8_t* data, std::size_t n, std::uint32_t crc = 0) {
    static const auto table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1U) ? 0xEDB88320U ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (std::size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFFU] ^ (crc >> 8);
    return ~crc;
}

std::uint32_t adler32(const std::uint8_t* data, std::size_t n) {
    std::uint32_t a = 1, b = 0;
    for (std::size_t i = 0; i < n; ++i) {
        a = (a + data[i]) % 65521U;
        b = (b + a) % 65521U;
    }
    return (b << 16) | a;
}

std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

// ---- inflate (RFC 1951), structured after zlib's reference "puff" ------------------------------

class BitReader {
public:
    BitReader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    std::uint32_t bits(int need) { // need <= 16
        while (count_ < need) {
            if (pos_ >= size_) fail("unexpected end of compressed data");
            buffer_ |= std::uint32_t(data_[pos_++]) << count_;
            count_ += 8;
        }
        const std::uint32_t v = buffer_ & ((1U << need) - 1U);
        buffer_ >>= need;
        count_ -= need;
        return v;
    }

    void alignToByte() { // bits() only ever buffers part of one byte, so this drops < 8 bits
        buffer_ = 0;
        count_ = 0;
    }

    const std::uint8_t* takeBytes(std::size_t n) {
        if (n > size_ - pos_) fail("unexpected end of compressed data");
        const std::uint8_t* p = data_ + pos_;
        pos_ += n;
        return p;
    }

    std::size_t position() const { return pos_; }

private:
    const std::uint8_t* data_;
    std::size_t size_;
    std::size_t pos_ = 0;
    std::uint32_t buffer_ = 0;
    int count_ = 0;
};

struct Huffman {
    std::uint16_t count[16] = {};
    std::uint16_t symbol[288] = {};
};

void buildHuffman(Huffman& h, const std::uint8_t* lengths, int n) {
    for (auto& c : h.count) c = 0;
    for (int s = 0; s < n; ++s) ++h.count[lengths[s]];
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) fail("over-subscribed Huffman code");
    }
    std::uint16_t offsets[16] = {};
    for (int len = 1; len < 15; ++len) offsets[len + 1] = static_cast<std::uint16_t>(offsets[len] + h.count[len]);
    for (int s = 0; s < n; ++s) {
        if (lengths[s] != 0) h.symbol[offsets[lengths[s]]++] = static_cast<std::uint16_t>(s);
    }
}

int decodeSymbol(BitReader& in, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= static_cast<int>(in.bits(1));
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    fail("invalid Huffman code");
}

constexpr std::uint16_t LenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t LenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t DistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::uint8_t DistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// Output is written into a buffer of exactly the size the PNG header promises; producing more or
// less than that is an error, which also bounds memory against decompression bombs.
struct Output {
    std::uint8_t* data;
    std::size_t size;
    std::size_t pos = 0;
};

void inflateCodes(BitReader& in, Output& out, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        int sym = decodeSymbol(in, lit);
        if (sym < 256) {
            if (out.pos >= out.size) fail("decompressed data larger than image");
            out.data[out.pos++] = static_cast<std::uint8_t>(sym);
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) fail("invalid length symbol");
            const std::size_t len = LenBase[sym] + in.bits(LenExtra[sym]);
            const int dsym = decodeSymbol(in, dist);
            if (dsym >= 30) fail("invalid distance symbol");
            const std::size_t d = DistBase[dsym] + in.bits(DistExtra[dsym]);
            if (d > out.pos) fail("distance reaches before start of output");
            if (len > out.size - out.pos) fail("decompressed data larger than image");
            for (std::size_t i = 0; i < len; ++i, ++out.pos) out.data[out.pos] = out.data[out.pos - d];
        }
    }
}

void inflateStored(BitReader& in, Output& out) {
    in.alignToByte();
    const std::uint8_t* header = in.takeBytes(4);
    const std::size_t len = header[0] | (header[1] << 8);
    const std::size_t nlen = header[2] | (header[3] << 8);
    if (len != (~nlen & 0xFFFFU)) fail("stored block length check failed");
    if (len > out.size - out.pos) fail("decompressed data larger than image");
    const std::uint8_t* src = in.takeBytes(len);
    for (std::size_t i = 0; i < len; ++i) out.data[out.pos++] = src[i];
}

void inflateFixed(BitReader& in, Output& out) {
    static const std::pair<Huffman, Huffman> tables = [] {
        std::uint8_t lengths[288];
        int s = 0;
        for (; s < 144; ++s) lengths[s] = 8;
        for (; s < 256; ++s) lengths[s] = 9;
        for (; s < 280; ++s) lengths[s] = 7;
        for (; s < 288; ++s) lengths[s] = 8;
        std::pair<Huffman, Huffman> t;
        buildHuffman(t.first, lengths, 288);
        std::uint8_t dl[30];
        for (auto& v : dl) v = 5;
        buildHuffman(t.second, dl, 30);
        return t;
    }();
    inflateCodes(in, out, tables.first, tables.second);
}

void inflateDynamic(BitReader& in, Output& out) {
    static constexpr std::uint8_t Order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    const int nlen = static_cast<int>(in.bits(5)) + 257;
    const int ndist = static_cast<int>(in.bits(5)) + 1;
    const int ncode = static_cast<int>(in.bits(4)) + 4;
    if (nlen > 286 || ndist > 30) fail("too many length or distance codes");

    std::uint8_t lengths[320] = {};
    for (int i = 0; i < ncode; ++i) lengths[Order[i]] = static_cast<std::uint8_t>(in.bits(3));
    Huffman codeLengthCode;
    buildHuffman(codeLengthCode, lengths, 19);

    std::uint8_t all[320] = {};
    int index = 0;
    while (index < nlen + ndist) {
        const int sym = decodeSymbol(in, codeLengthCode);
        if (sym < 16) {
            all[index++] = static_cast<std::uint8_t>(sym);
            continue;
        }
        std::uint8_t value = 0;
        int repeat = 0;
        if (sym == 16) {
            if (index == 0) fail("repeat with no previous length");
            value = all[index - 1];
            repeat = 3 + static_cast<int>(in.bits(2));
        } else if (sym == 17) {
            repeat = 3 + static_cast<int>(in.bits(3));
        } else {
            repeat = 11 + static_cast<int>(in.bits(7));
        }
        if (index + repeat > nlen + ndist) fail("code length repeat overflows");
        while (repeat--) all[index++] = value;
    }
    if (all[256] == 0) fail("missing end-of-block code");

    Huffman lit, dist;
    buildHuffman(lit, all, nlen);
    buildHuffman(dist, all + nlen, ndist);
    inflateCodes(in, out, lit, dist);
}

// zlib stream (RFC 1950) -> exactly `expected` bytes.
Bytes zlibInflate(const Bytes& z, std::size_t expected) {
    if (z.size() < 6) fail("zlib stream too short");
    if ((z[0] & 0x0F) != 8 || ((z[0] << 8) | z[1]) % 31 != 0 || (z[1] & 0x20)) fail("bad zlib header");
    Bytes out(expected);
    Output output{out.data(), out.size()};
    BitReader in(z.data() + 2, z.size() - 2);
    for (;;) {
        const std::uint32_t final = in.bits(1);
        switch (in.bits(2)) {
        case 0: inflateStored(in, output); break;
        case 1: inflateFixed(in, output); break;
        case 2: inflateDynamic(in, output); break;
        default: fail("invalid deflate block type");
        }
        if (final) break;
    }
    if (output.pos != expected) fail("decompressed data smaller than image");
    in.alignToByte();
    const std::size_t trailer = 2 + in.position();
    if (z.size() < trailer + 4 || be32(z.data() + trailer) != adler32(out.data(), out.size())) fail("zlib checksum mismatch");
    return out;
}

// ---- PNG ----------------------------------------------------------------------------------------

constexpr std::uint8_t Signature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
constexpr int MaxSide = 16384;
constexpr std::uint64_t MaxPixels = 1ULL << 25;

int paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

int channelsFor(int colorType) {
    switch (colorType) {
    case 0: return 1; // gray
    case 2: return 3; // RGB
    case 3: return 1; // palette index
    case 4: return 2; // gray + alpha
    case 6: return 4; // RGBA
    default: fail("unsupported color type");
    }
}

void putChunk(Bytes& out, const char type[4], const Bytes& data) {
    auto put32 = [&out](std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>(v >> s));
    };
    put32(static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(crc32(out.data() + start, out.size() - start));
}

} // namespace

Image decodePng(const Bytes& file) {
    if (file.size() < 8 + 25 || !std::equal(Signature, Signature + 8, file.begin())) fail("not a PNG file");

    int width = 0, height = 0, colorType = -1;
    Bytes palette, idat;
    bool sawHeader = false, sawEnd = false;

    std::size_t pos = 8;
    while (pos < file.size() && !sawEnd) {
        if (file.size() - pos < 12) fail("truncated chunk");
        const std::uint32_t length = be32(&file[pos]);
        if (length > file.size() - pos - 12) fail("chunk length exceeds file");
        const std::uint8_t* type = &file[pos + 4];
        const std::uint8_t* data = &file[pos + 8];
        if (crc32(type, 4 + length) != be32(data + length)) fail("chunk CRC mismatch");
        const std::string name(reinterpret_cast<const char*>(type), 4);
        pos += 12 + length;

        if (!sawHeader) {
            if (name != "IHDR" || length != 13) fail("first chunk must be IHDR");
            const std::uint32_t w = be32(data), h = be32(data + 4);
            if (w == 0 || h == 0 || w > MaxSide || h > MaxSide || std::uint64_t(w) * h > MaxPixels) fail("image dimensions out of range");
            if (data[8] != 8) fail("only 8-bit images are supported");
            colorType = data[9];
            channelsFor(colorType);
            if (data[10] != 0 || data[11] != 0) fail("unknown compression or filter method");
            if (data[12] != 0) fail("interlaced images are not supported");
            width = static_cast<int>(w);
            height = static_cast<int>(h);
            sawHeader = true;
        } else if (name == "PLTE") {
            if (length % 3 != 0 || length > 768) fail("bad palette");
            palette.assign(data, data + length);
        } else if (name == "IDAT") {
            idat.insert(idat.end(), data, data + length);
        } else if (name == "IEND") {
            sawEnd = true;
        } else if ((type[0] & 0x20) == 0 && name != "IHDR") {
            fail("unknown critical chunk");
        }
    }
    if (!sawHeader || !sawEnd) fail("missing IHDR or IEND");
    if (colorType == 3 && palette.empty()) fail("palette image without PLTE");

    const int bpp = channelsFor(colorType);
    const std::size_t stride = static_cast<std::size_t>(width) * bpp;
    Bytes raw = zlibInflate(idat, (stride + 1) * static_cast<std::size_t>(height));

    // Undo the per-row filters in place.
    for (int y = 0; y < height; ++y) {
        std::uint8_t* row = raw.data() + static_cast<std::size_t>(y) * (stride + 1);
        const int filter = row[0];
        std::uint8_t* cur = row + 1;
        const std::uint8_t* up = y > 0 ? row - stride : nullptr; // previous row's pixels (already unfiltered)
        if (filter > 4) fail("invalid row filter");
        for (std::size_t i = 0; i < stride; ++i) {
            const int a = i >= static_cast<std::size_t>(bpp) ? cur[i - bpp] : 0;
            const int b = up ? up[i] : 0;
            const int c = (up && i >= static_cast<std::size_t>(bpp)) ? up[i - bpp] : 0;
            int pred = 0;
            switch (filter) {
            case 1: pred = a; break;
            case 2: pred = b; break;
            case 3: pred = (a + b) / 2; break;
            case 4: pred = paeth(a, b, c); break;
            default: break;
            }
            cur[i] = static_cast<std::uint8_t>(cur[i] + pred);
        }
    }

    Image image;
    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    for (int y = 0; y < height; ++y) {
        const std::uint8_t* src = raw.data() + static_cast<std::size_t>(y) * (stride + 1) + 1;
        std::uint8_t* dst = image.rgba.data() + static_cast<std::size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x, dst += 4) {
            const std::uint8_t* s = src + static_cast<std::size_t>(x) * bpp;
            switch (colorType) {
            case 0: dst[0] = dst[1] = dst[2] = s[0]; dst[3] = 255; break;
            case 2: dst[0] = s[0]; dst[1] = s[1]; dst[2] = s[2]; dst[3] = 255; break;
            case 3: {
                const std::size_t idx = s[0];
                if (idx * 3 + 2 >= palette.size()) fail("palette index out of range");
                dst[0] = palette[idx * 3]; dst[1] = palette[idx * 3 + 1]; dst[2] = palette[idx * 3 + 2]; dst[3] = 255;
                break;
            }
            case 4: dst[0] = dst[1] = dst[2] = s[0]; dst[3] = s[1]; break;
            default: dst[0] = s[0]; dst[1] = s[1]; dst[2] = s[2]; dst[3] = s[3]; break;
            }
        }
    }
    return image;
}

Bytes encodePng(const Image& image, int filter) {
    if (image.width <= 0 || image.height <= 0 || image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4) {
        throw std::invalid_argument("encodePng: bad image");
    }
    const std::size_t stride = static_cast<std::size_t>(image.width) * 4;
    Bytes raw;
    raw.reserve((stride + 1) * image.height);
    for (int y = 0; y < image.height; ++y) {
        const int f = filter == FilterCycle ? y % 5 : filter;
        const std::uint8_t* cur = image.rgba.data() + y * stride;
        const std::uint8_t* up = y > 0 ? cur - stride : nullptr;
        raw.push_back(static_cast<std::uint8_t>(f));
        for (std::size_t i = 0; i < stride; ++i) {
            const int a = i >= 4 ? cur[i - 4] : 0;
            const int b = up ? up[i] : 0;
            const int c = (up && i >= 4) ? up[i - 4] : 0;
            int pred = 0;
            switch (f) {
            case 1: pred = a; break;
            case 2: pred = b; break;
            case 3: pred = (a + b) / 2; break;
            case 4: pred = paeth(a, b, c); break;
            default: break;
            }
            raw.push_back(static_cast<std::uint8_t>(cur[i] - pred));
        }
    }

    Bytes z = {0x78, 0x01};
    for (std::size_t off = 0; off < raw.size() || off == 0;) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - off);
        const bool last = off + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n & 0xFF));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n & 0xFF));
        z.push_back(static_cast<std::uint8_t>((~n >> 8) & 0xFF));
        z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
        off += n;
        if (last) break;
    }
    const std::uint32_t ad = adler32(raw.data(), raw.size());
    for (int s = 24; s >= 0; s -= 8) z.push_back(static_cast<std::uint8_t>(ad >> s));

    Bytes header(13);
    for (int i = 0; i < 4; ++i) {
        header[i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(image.width) >> (24 - 8 * i));
        header[4 + i] = static_cast<std::uint8_t>(static_cast<std::uint32_t>(image.height) >> (24 - 8 * i));
    }
    header[8] = 8;
    header[9] = 6; // RGBA
    Bytes out(Signature, Signature + 8);
    putChunk(out, "IHDR", header);
    putChunk(out, "IDAT", z);
    putChunk(out, "IEND", {});
    return out;
}

} // namespace streamer
