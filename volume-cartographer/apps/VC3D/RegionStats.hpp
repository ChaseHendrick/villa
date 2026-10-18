#pragma once

// Statistics of the voxels inside a region, read chunk by chunk from a VC3D
// volume. Free of Qt. Every number is computed from an integer histogram
// merged by addition, so thread count and chunk order cannot change any
// result (docs/regions.md has the exact definitions and a Python check).

#include "RegionModel.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class Volume;

namespace vc3d::regions
{

static_assert(__SIZEOF_INT128__ == 16, "region statistics need a 128-bit integer type");

// An inclusive index range at one level of a volume, z y x order.
struct LevelRange {
    int level = 0;
    cv::Vec3i minZyx{0, 0, 0};
    cv::Vec3i maxZyx{-1, -1, -1};
    bool empty() const;
    std::uint64_t count() const;
};

// Level-L indices i whose viewer point i * 2^L maps to a region point inside
// the box, clipped to the level's shape (z y x).
LevelRange boxIndexRange(const Box& box, const FrameMap& regionToVolume, int level, const std::array<int, 3>& levelShapeZyx);

// Statistics and crop exports read at most this many bytes of voxels per
// region.
inline constexpr std::uint64_t kMaxReadBytes = std::uint64_t{8} << 30;

struct Progress {
    std::function<void(double)> report;
    const std::atomic<bool>* cancel = nullptr;
    bool canceled() const { return cancel && cancel->load(std::memory_order_relaxed); }
};

// One chunk's part of a range: `data` points at the chunk's voxels (chunk
// layout, `chunkShapeZyx`), or is null when the chunk is all fill value.
// `fromZyx` and `toZyx` are the inclusive in-range indices, level coordinates.
struct ChunkView {
    int cz = 0;
    int cy = 0;
    int cx = 0;
    const std::byte* data = nullptr;
    bool allFill = false;
    std::array<int, 3> chunkShapeZyx{0, 0, 0};
    std::array<int, 3> chunkOriginZyx{0, 0, 0};
    cv::Vec3i fromZyx{0, 0, 0};
    cv::Vec3i toZyx{0, 0, 0};
};
using ChunkVisitor = std::function<void(int worker, const ChunkView& chunk)>;

// Visits every chunk of `level` meeting the range once, reading with
// IChunkedArray::getChunkBlocking on `threads` dedicated workers after a
// background prefetch. Returns false on a read error or a cancel (check
// progress.canceled()); `error` is set on a read error.
bool forEachChunk(
    Volume& volume,
    int level,
    const LevelRange& range,
    int threads,
    const Progress& progress,
    const ChunkVisitor& visit,
    std::string* error,
    std::uint64_t* chunksRead = nullptr,
    std::uint64_t* fillChunks = nullptr);

struct StatsRequest {
    int level = -1;                          // -1: the finest stored level within voxelBudget
    std::uint64_t voxelBudget = 33'554'432;  // 32 Mi voxels
    std::vector<double> percentiles{0.5, 1, 5, 25, 50, 75, 95, 99, 99.5};
    std::optional<double> threshold;
    int threads = 2;
};

struct Moments {
    std::uint64_t n = 0;
    std::uint64_t s1 = 0;
    unsigned __int128 s2 = 0;
};

Moments momentsOf(std::span<const std::uint64_t> histogram);
// numpy percentile(..., method="inverted_cdf") on the histogram's values:
// p = 0 gives the minimum, otherwise the smallest value whose cumulative
// count reaches ceil(round(p * 10^4) * n / 10^6).
std::uint32_t percentileOf(std::span<const std::uint64_t> histogram, std::uint64_t n, double percent);
// Exact decimal text of a 128-bit count.
std::string toDecimal(unsigned __int128 value);
// True when a percentile is in [0, 100] with at most 4 decimals.
bool isValidPercentile(double percent);

struct VolumeStats {
    std::string volumeId;
    std::string dtype;  // "uint8" or "uint16"
    int level = 0;
    LevelRange range;
    std::uint64_t count = 0;
    std::uint32_t min = 0;
    std::uint32_t max = 0;
    std::uint64_t sum = 0;
    unsigned __int128 sumSquares = 0;
    double mean = 0;
    double stddev = 0;
    double volumeMm3 = 0;
    double levelVoxelSizeUm = 0;
    std::vector<std::pair<double, std::uint32_t>> percentiles;
    std::array<std::uint64_t, 256> histogram{};
    int binWidth = 1;
    std::optional<double> threshold;
    std::uint64_t countAbove = 0;
    double fractionAbove = 0;
    std::uint64_t chunksRead = 0;
    std::uint64_t fillChunks = 0;
    double seconds = 0;
    bool canceled = false;
};

// The stored levels and their index-range sizes for a box, finest first.
std::vector<std::pair<int, std::uint64_t>> levelVoxelCounts(Volume& volume, const Box& box, const FrameMap& map);
// The finest stored level whose range fits the budget, else the coarsest.
int chooseLevel(Volume& volume, const Box& box, const FrameMap& map, std::uint64_t voxelBudget);

// Statistics of the box on one volume. nullopt with `error` set on a failure;
// a cancel returns stats with canceled set and partial numbers.
std::optional<VolumeStats> computeStats(
    Volume& volume, const std::string& volumeId, const Box& box, const FrameMap& map, const StatsRequest& request, const Progress& progress, std::string* error);

// The stats file document (schema vc3d.region_stats, version 1). `created`
// is an ISO 8601 UTC time.
Json statsJson(const VolumeStats& stats, const Region& region, const FrameMap& map, const std::string& location, const std::string& created);

// "<sanitized volume id>-L<level>.json"
std::string statsFileName(const std::string& volumeId, int level);

}  // namespace vc3d::regions
