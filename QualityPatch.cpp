#include "QualityPatch.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using Bytes = std::vector<unsigned char>;

void require(bool valid, const char* message) {
    if (!valid) throw std::runtime_error(message);
}

uint64_t number(const Bytes& bytes, size_t pos, size_t width = 4) {
    require(pos <= bytes.size() && width <= bytes.size() - pos, "Truncated MP4 field");
    uint64_t value = 0;
    for (size_t i = 0; i < width; ++i) value = (value << 8) | bytes[pos + i];
    return value;
}

void write32(Bytes& bytes, size_t pos, uint32_t value) {
    for (int i = 3; i >= 0; --i) {
        bytes.at(pos + i) = value & 255;
        value >>= 8;
    }
}

struct Box {
    uint64_t start, payload, end;
    std::string_view type;
};

Box boxAt(const Bytes& bytes, size_t pos, size_t limit) {
    require(pos <= limit && limit - pos >= 8, "Truncated MP4 box");
    const auto size = number(bytes, pos);
    const size_t header = size == 1 ? 16 : 8;
    const auto length = size == 1 ? number(bytes, pos + 8, 8) : size ? size : limit - pos;
    require(length >= header && length <= limit - pos, "Invalid MP4 box size");
    const auto type = reinterpret_cast<const char*>(bytes.data() + pos + 4);
    return {pos, pos + header, pos + length, {type, 4}};
}

Box child(const Bytes& bytes, Box parent, std::string_view type) {
    for (auto pos = parent.payload; pos < parent.end;) {
        const auto box = boxAt(bytes, pos, parent.end);
        if (box.type == type) return box;
        pos = box.end;
    }
    return {};
}

Box required(const Bytes& bytes, Box parent, std::string_view type) {
    const auto box = child(bytes, parent, type);
    require(box.end != 0, "Required MP4 box is missing");
    return box;
}

uint64_t field(const Bytes& bytes, Box box, uint64_t offset, size_t width = 4) {
    const auto size = box.end - box.payload;
    require(offset <= size && width <= size - offset, "Truncated MP4 table");
    return number(bytes, box.payload + offset, width);
}

void readAt(std::ifstream& input, uint64_t offset, Bytes& bytes) {
    input.seekg(offset);
    require(bool(input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())),
            "Could not read source video");
}

struct Sample {
    uint64_t offset, length;
};

std::vector<Sample> lastAudioSamples(const Bytes& movie) {
    const auto root = boxAt(movie, 0, movie.size());
    Box audio{};
    bool hasVideo = false;
    for (auto pos = root.payload; pos < root.end;) {
        const auto track = boxAt(movie, pos, root.end);
        pos = track.end;
        if (track.type != "trak") continue;
        const auto media = required(movie, track, "mdia");
        const auto type = field(movie, required(movie, media, "hdlr"), 8);
        if (type == 0x76696465) hasVideo = true;
        if (type == 0x736f756e && !audio.end) audio = media;
    }
    require(hasVideo && audio.end, "MP4 must contain video and AAC audio");
    const auto table = required(movie, required(movie, audio, "minf"), "stbl");
    const auto desc = required(movie, table, "stsd");
    require(field(movie, desc, 4) == 1, "Multiple audio codecs are unsupported");
    const auto entry = boxAt(movie, desc.payload + 8, desc.end);
    const auto channels = field(movie, entry, 16, 2);
    require(entry.type == "mp4a" && channels == 2 &&
            field(movie, entry, 24) == (48000u << 16),
            "This prototype requires 48 kHz stereo AAC-LC");
    auto extensions = entry;
    extensions.payload += 28;
    const auto esds = required(movie, extensions, "esds");
    // AAC-LC, sampling frequency index 3 (48 kHz), channel configuration 2.
    bool aacLC = false;
    for (auto pos = esds.payload + 4; pos + 4 <= esds.end; ++pos) {
        if (number(movie, pos) == 0x05021190) aacLC = true;
    }
    require(aacLC, "Unsupported AAC-LC decoder configuration");

    const auto sizes = required(movie, table, "stsz");
    const auto commonSize = field(movie, sizes, 4);
    const auto sampleCount = field(movie, sizes, 8);
    require(sampleCount >= 4 && (commonSize || sampleCount <= (sizes.end - sizes.payload - 12) / 4),
            "Invalid AAC sample sizes");
    const auto runs = required(movie, table, "stsc");
    const auto runCount = field(movie, runs, 4);
    auto chunks = child(movie, table, "co64");
    const int width = chunks.end ? 8 : 4;
    if (!chunks.end) chunks = required(movie, table, "stco");
    const auto chunkCount = field(movie, chunks, 4);
    require(runCount && chunkCount && field(movie, runs, 8) == 1, "Invalid AAC chunk table");

    uint64_t run = 0, sample = 0;
    std::vector<Sample> tail;
    for (uint64_t chunk = 0; chunk < chunkCount; ++chunk) {
        if (run + 1 < runCount && field(movie, runs, 8 + 12 * (run + 1)) == chunk + 1) ++run;
        const auto count = field(movie, runs, 12 + 12 * run);
        require(count && count <= sampleCount - sample &&
                field(movie, runs, 16 + 12 * run) == 1, "Invalid AAC chunk mapping");
        auto offset = field(movie, chunks, 8 + width * chunk, width);
        for (uint64_t i = 0; i < count; ++i, ++sample) {
            const auto length = commonSize ? commonSize : field(movie, sizes, 12 + 4 * sample);
            require(length && offset <= UINT64_MAX - length, "Invalid AAC sample offset");
            if (sample >= sampleCount - 4) tail.push_back({offset, length});
            offset += length;
        }
    }
    require(sample == sampleCount && tail.size() == 4, "Incomplete AAC sample table");
    return tail;
}

Bytes commentBox() {
    return {
        0,0,0,100,'m','e','t','a',0,0,0,0,
        0,0,0,33,'h','d','l','r',0,0,0,0,0,0,0,0,'m','d','i','r',
        0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0,0,55,'i','l','s','t',
        0,0,0,47,0xa9,'c','m','t',
        0,0,0,39,'d','a','t','a',0,0,0,1,0,0,0,0,
        'P','a','t','c','h','e','d',' ','b','y',' ','C','o','m','p','r','e','s','s','b','a','s','e'
    };
}
} // namespace

void patchVideo(const std::string& source, const std::string& destination,
                const PatchLog& log, const PatchProgress& progress) {
    require(source != destination, "Source and destination must differ");
    std::ifstream input(source, std::ios::binary | std::ios::ate);
    require(bool(input), "Could not open source video");
    const auto length = input.tellg();
    require(length >= 8, "Empty or invalid video");
    const auto size = static_cast<uint64_t>(length);
    std::vector<std::pair<uint64_t, uint64_t>> media;
    uint64_t movieStart = 0, movieSize = 0;
    for (uint64_t pos = 0; pos < size;) {
        require(size - pos >= 8, "Truncated MP4 header");
        Bytes header(8);
        readAt(input, pos, header);
        auto span = number(header, 0);
        const std::string type(header.begin() + 4, header.end());
        require(span != 0 && type != "moof", "Fragmented or open-ended MP4 is unsupported");
        uint64_t headerSize = 8;
        if (span == 1) {
            require(size - pos >= 16, "Truncated extended MP4 header");
            readAt(input, pos + 8, header);
            span = number(header, 0, 8);
            headerSize = 16;
        }
        require(span >= headerSize && span <= size - pos, "Invalid top-level MP4 box");
        if (type == "mdat") media.emplace_back(pos + headerSize, pos + span);
        if (type == "moov") {
            require(!movieSize, "Multiple MP4 movie boxes are unsupported");
            movieStart = pos;
            movieSize = span;
        }
        pos += span;
    }
    require(movieSize && movieSize <= 64 * 1024 * 1024, "Invalid or oversized MP4 metadata");
    Bytes movie(movieSize);
    readAt(input, movieStart, movie);
    const auto samples = lastAudioSamples(movie);
    const auto root = boxAt(movie, 0, movie.size());
    const auto metadata = required(movie, root, "udta");
    require(root.payload == 8 && metadata.payload == metadata.start + 8,
            "Extended movie metadata headers are unsupported");
    const auto comment = commentBox();
    const std::string_view bytes(reinterpret_cast<const char*>(movie.data()), movie.size());
    require(bytes.find("Patched by Compressbase") == std::string_view::npos,
            "File already contains the CompressBase marker");
    movie.insert(movie.begin() + metadata.end, comment.begin(), comment.end());
    write32(movie, 0, static_cast<uint32_t>(movie.size()));
    write32(movie, metadata.start, static_cast<uint32_t>(metadata.end - metadata.start + comment.size()));

    struct Replacement { uint64_t offset; Bytes bytes; };
    std::vector<Replacement> edits{{movieStart + 4, {'f','r','e','e'}}};
    constexpr std::array<unsigned char, 15> pce{
        0xa0,0x98,0x00,0x40,0x01,0x28,0x00,0x64,0,0,0x03,0x50,0,0,0x38
    };
    for (const auto sample : samples) {
        const bool inMedia = std::any_of(media.begin(), media.end(), [&](auto range) {
            return sample.offset >= range.first && sample.offset <= range.second &&
                   sample.length <= range.second - sample.offset;
        });
        require(inMedia && sample.length >= pce.size() && sample.length <= 1024 * 1024,
                "AAC sample cannot be patched");
        Bytes replacement(sample.length, 0);
        std::copy(pce.begin(), pce.end(), replacement.begin());
        edits.push_back({sample.offset, std::move(replacement)});
    }
    std::sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
    uint64_t previousEnd = 0;
    for (const auto& edit : edits) {
        require(edit.offset >= previousEnd, "Overlapping MP4 edits");
        previousEnd = edit.offset + edit.bytes.size();
    }
    log("MP4 parsed: " + std::to_string(size) + " bytes");
    log("Located the final 4 AAC samples (stereo, 48 kHz)");

    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    require(bool(output), "Could not create temporary video");
    uint64_t written = 0;
    const auto total = size + movie.size();
    progress(0, total);
    const auto write = [&](const unsigned char* data, size_t count) {
        require(bool(output.write(reinterpret_cast<const char*>(data), count)),
                "Could not write temporary video");
        written += count;
        if (written < total) progress(written, total);
    };
    Bytes buffer(256 * 1024);
    const auto copyTo = [&](uint64_t end) {
        while (written < end) {
            const auto count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), end - written));
            input.seekg(written);
            require(bool(input.read(reinterpret_cast<char*>(buffer.data()), count)),
                    "Could not copy source video");
            write(buffer.data(), count);
        }
    };
    for (const auto& edit : edits) {
        copyTo(edit.offset);
        write(edit.bytes.data(), edit.bytes.size());
    }
    copyTo(size);
    log("AAC samples written; appending updated MP4 metadata");
    write(movie.data(), movie.size());
    output.close();
    require(bool(output), "Could not finish temporary video");
    progress(written, total);
    log("Output ready: " + std::to_string(written) + " bytes");
}
