#include "RegionStats.hpp"

#include "vc/core/Version.hpp"
#include "vc/core/render/ChunkCache.hpp"
#include "vc/core/render/IChunkedArray.hpp"
#include "vc/core/types/Volume.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <mutex>
#include <thread>

namespace vc3d::regions
{

namespace
{

// ceil(value * 2^shift) and floor(value * 2^shift) for value >= 0.
long long scaleCeil(long long value, int shift)
{
    if (shift >= 0) {
        return value << shift;
    }
    const long long d = 1LL << (-shift);
    return (value + d - 1) / d;
}

long long scaleFloor(long long value, int shift)
{
    if (shift >= 0) {
        return value << shift;
    }
    const long long d = 1LL << (-shift);
    return value / d;
}

std::size_t bytesPerVoxel(vc::render::ChunkDtype dtype)
{
    return dtype == vc::render::ChunkDtype::UInt16 ? 2 : 1;
}

}  // namespace

bool LevelRange::empty() const
{
    return minZyx[0] > maxZyx[0] || minZyx[1] > maxZyx[1] || minZyx[2] > maxZyx[2];
}

std::uint64_t LevelRange::count() const
{
    if (empty()) {
        return 0;
    }
    std::uint64_t n = 1;
    for (int axis = 0; axis < 3; ++axis) {
        n *= static_cast<std::uint64_t>(maxZyx[axis] - minZyx[axis]) + 1;
    }
    return n;
}

LevelRange boxIndexRange(const Box& box, const FrameMap& regionToVolume, int level, const std::array<int, 3>& levelShapeZyx)
{
    LevelRange range;
    range.level = level;
    // Region point r is level index r * k / 2^L = r * 2^shift.
    const int shift = regionToVolume.log2RegionToViewer - level;
    for (int axis = 0; axis < 3; ++axis) {
        const int zyx = 2 - axis;
        const long long lo = std::max(0LL, scaleCeil(std::max(0, box.min[axis]), shift));
        const long long hi = std::min<long long>(levelShapeZyx[zyx] - 1LL, scaleFloor(std::max(0, box.max[axis]), shift));
        range.minZyx[zyx] = static_cast<int>(std::min<long long>(lo, std::numeric_limits<int>::max()));
        range.maxZyx[zyx] = static_cast<int>(std::max<long long>(hi, -1));
    }
    return range;
}

std::optional<ShapeSpec> shapeSpec(const Region& region, std::string* error)
{
    ShapeSpec spec;
    spec.shape = region.shape;
    if (const RayArc* arc = arcOf(region)) {
        auto compiled = compileArc(*arc, region.frame, error);
        if (!compiled) {
            return std::nullopt;
        }
        spec.arc = std::make_shared<const CompiledArc>(std::move(*compiled));
    }
    return spec;
}

LevelRange shapeIndexRange(const ShapeSpec& spec, const FrameMap& regionToVolume, int level, const std::array<int, 3>& levelShapeZyx)
{
    if (const auto* box = std::get_if<Box>(&spec.shape)) {
        return boxIndexRange(*box, regionToVolume, level, levelShapeZyx);
    }
    const auto& arc = std::get<RayArc>(spec.shape);
    if (!spec.arc) {
        return LevelRange{level};
    }
    // The arc's bounding box, widened to whole voxels: every voxel inside the
    // arc has its region point inside this box.
    const auto bounds = arcBounds(arc, *spec.arc);
    Box box;
    for (int axis = 0; axis < 3; ++axis) {
        const double lo = std::floor(bounds[0][axis]);
        const double hi = std::ceil(bounds[1][axis]);
        if (!(hi >= lo) || hi < 0.0) {
            return LevelRange{level};
        }
        box.min[axis] = static_cast<int>(std::clamp(lo, 0.0, static_cast<double>(std::numeric_limits<int>::max())));
        box.max[axis] = static_cast<int>(std::clamp(hi, 0.0, static_cast<double>(std::numeric_limits<int>::max())));
    }
    return boxIndexRange(box, regionToVolume, level, levelShapeZyx);
}

bool containsIndex(const ShapeSpec& spec, const FrameMap& regionToVolume, int level, int z, int y, int x)
{
    const double s = std::ldexp(1.0, level - regionToVolume.log2RegionToViewer);
    const cv::Vec3d p(x * s, y * s, z * s);
    if (const auto* box = std::get_if<Box>(&spec.shape)) {
        return contains(*box, p);
    }
    return spec.arc && contains(std::get<RayArc>(spec.shape), *spec.arc, p);
}

bool forEachChunk(
    Volume& volume,
    int level,
    const LevelRange& range,
    int threads,
    const Progress& progress,
    const ChunkVisitor& visit,
    std::string* error,
    std::uint64_t* chunksRead,
    std::uint64_t* fillChunks,
    const std::function<bool(const cv::Vec3i& fromZyx, const cv::Vec3i& toZyx)>& keepChunk)
{
    using vc::render::ChunkKey;
    using vc::render::ChunkStatus;
    // Held for the whole job: Volume::invalidateCache() may drop the volume's
    // own reference meanwhile.
    const auto cache = volume.sharedChunkCache();
    vc::render::IChunkedArray* array = cache.get();
    if (!array) {
        if (error) {
            *error = "the volume has no chunk source";
        }
        return false;
    }
    if (range.empty()) {
        return true;
    }
    const std::array<int, 3> chunkShape = array->chunkShape(level);
    if (chunkShape[0] <= 0 || chunkShape[1] <= 0 || chunkShape[2] <= 0) {
        if (error) {
            *error = "invalid chunk shape at level " + std::to_string(level);
        }
        return false;
    }
    const std::size_t voxelBytes = bytesPerVoxel(array->dtype());
    const std::size_t chunkBytes = static_cast<std::size_t>(chunkShape[0]) * chunkShape[1] * chunkShape[2] * voxelBytes;

    std::vector<ChunkKey> keys;
    const int c0[3] = {range.minZyx[0] / chunkShape[0], range.minZyx[1] / chunkShape[1], range.minZyx[2] / chunkShape[2]};
    const int c1[3] = {range.maxZyx[0] / chunkShape[0], range.maxZyx[1] / chunkShape[1], range.maxZyx[2] / chunkShape[2]};
    keys.reserve(static_cast<std::size_t>(c1[0] - c0[0] + 1) * (c1[1] - c0[1] + 1) * (c1[2] - c0[2] + 1));
    for (int cz = c0[0]; cz <= c1[0]; ++cz) {
        for (int cy = c0[1]; cy <= c1[1]; ++cy) {
            for (int cx = c0[2]; cx <= c1[2]; ++cx) {
                ChunkKey key;
                key.level = level;
                key.iz = cz;
                key.iy = cy;
                key.ix = cx;
                if (keepChunk) {
                    const cv::Vec3i
                        from(std::max(range.minZyx[0], cz * chunkShape[0]), std::max(range.minZyx[1], cy * chunkShape[1]), std::max(range.minZyx[2], cx * chunkShape[2]));
                    const cv::Vec3i
                        to(std::min(range.maxZyx[0], (cz + 1) * chunkShape[0] - 1),
                           std::min(range.maxZyx[1], (cy + 1) * chunkShape[1] - 1),
                           std::min(range.maxZyx[2], (cx + 1) * chunkShape[2] - 1));
                    if (!keepChunk(from, to)) {
                        continue;
                    }
                }
                keys.push_back(key);
            }
        }
    }
    // Chunks are queued a window ahead of the workers rather than all at
    // once, so a canceled job leaves at most a window of downloads behind. A
    // positive offset runs after interactive viewer demand.
    const int workers = std::clamp(threads, 1, 32);
    const std::size_t window = static_cast<std::size_t>(workers) * 8;
    std::mutex prefetchMutex;
    std::size_t prefetched = 0;
    const auto prefetchTo = [&](std::size_t end) {
        std::lock_guard<std::mutex> lock(prefetchMutex);
        end = std::min(end, keys.size());
        if (end <= prefetched || (end - prefetched < window / 2 && end < keys.size()) || progress.canceled()) {
            return;
        }
        const auto first = keys.begin() + static_cast<std::ptrdiff_t>(prefetched);
        array->prefetchChunks(std::vector<ChunkKey>(first, keys.begin() + static_cast<std::ptrdiff_t>(end)), false, 8);
        prefetched = end;
    };
    prefetchTo(window);

    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> done{0};
    std::atomic<std::uint64_t> fills{0};
    std::atomic<bool> failed{false};
    std::mutex errorMutex;
    std::string firstError;
    const auto fail = [&](const std::string& message) {
        bool expected = false;
        if (failed.compare_exchange_strong(expected, true)) {
            std::lock_guard<std::mutex> lock(errorMutex);
            firstError = message;
        }
    };

    const auto work = [&](int worker) {
        while (!failed.load() && !progress.canceled()) {
            const std::size_t index = next.fetch_add(1);
            if (index >= keys.size()) {
                return;
            }
            prefetchTo(index + window);
            const ChunkKey& key = keys[index];
            vc::render::ChunkResult result;
            try {
                result = array->getChunkBlocking(level, key.iz, key.iy, key.ix);
            } catch (const std::exception& e) {
                fail(std::string("reading chunk failed: ") + e.what());
                return;
            }
            ChunkView view;
            view.cz = key.iz;
            view.cy = key.iy;
            view.cx = key.ix;
            view.chunkShapeZyx = chunkShape;
            view.chunkOriginZyx = {key.iz * chunkShape[0], key.iy * chunkShape[1], key.ix * chunkShape[2]};
            for (int axis = 0; axis < 3; ++axis) {
                view.fromZyx[axis] = std::max(range.minZyx[axis], view.chunkOriginZyx[axis]);
                view.toZyx[axis] = std::min(range.maxZyx[axis], view.chunkOriginZyx[axis] + chunkShape[axis] - 1);
            }
            if (result.status == ChunkStatus::Data && result.bytes) {
                if (result.bytes->size() < chunkBytes) {
                    fail(
                        "chunk " + std::to_string(level) + "/" + std::to_string(key.iz) + "/" + std::to_string(key.iy) + "/" +
                        std::to_string(key.ix) + " is smaller than its shape");
                    return;
                }
                view.data = result.bytes->data();
            } else if (result.status == ChunkStatus::Error) {
                fail(
                    "reading chunk " + std::to_string(level) + "/" + std::to_string(key.iz) + "/" + std::to_string(key.iy) + "/" +
                    std::to_string(key.ix) + " failed: " + (result.error.empty() ? std::string("unknown error") : result.error));
                return;
            } else {
                // AllFill and Missing read as the fill value, as zarr does.
                view.allFill = true;
                fills.fetch_add(1);
            }
            visit(worker, view);
            const std::size_t finished = done.fetch_add(1) + 1;
            if (progress.report) {
                progress.report(static_cast<double>(finished) / static_cast<double>(keys.size()));
            }
        }
    };

    if (workers == 1) {
        work(0);
    } else {
        std::vector<std::thread> pool;
        pool.reserve(static_cast<std::size_t>(workers));
        for (int w = 0; w < workers; ++w) {
            pool.emplace_back(work, w);
        }
        for (auto& t : pool) {
            t.join();
        }
    }
    if (chunksRead) {
        *chunksRead = done.load();
    }
    if (fillChunks) {
        *fillChunks = fills.load();
    }
    if (failed.load()) {
        if (error) {
            *error = firstError;
        }
        return false;
    }
    return !progress.canceled();
}

Moments momentsOf(std::span<const std::uint64_t> histogram)
{
    Moments m;
    for (std::size_t v = 0; v < histogram.size(); ++v) {
        const std::uint64_t h = histogram[v];
        if (h == 0) {
            continue;
        }
        m.n += h;
        m.s1 += static_cast<std::uint64_t>(v) * h;
        m.s2 += static_cast<unsigned __int128>(static_cast<std::uint64_t>(v) * static_cast<std::uint64_t>(v)) * h;
    }
    return m;
}

bool isValidPercentile(double percent)
{
    if (!std::isfinite(percent) || percent < 0.0 || percent > 100.0) {
        return false;
    }
    const double scaled = percent * 10000.0;
    return std::abs(scaled - std::round(scaled)) <= 1e-6;
}

std::uint32_t percentileOf(std::span<const std::uint64_t> histogram, std::uint64_t n, double percent)
{
    if (n == 0) {
        return 0;
    }
    const auto q = static_cast<unsigned __int128>(std::llround(std::clamp(percent, 0.0, 100.0) * 10000.0));
    unsigned __int128 rank = 1;
    if (q > 0) {
        rank = (q * n + 999'999) / 1'000'000;
        if (rank == 0) {
            rank = 1;
        }
    }
    unsigned __int128 cumulative = 0;
    for (std::size_t v = 0; v < histogram.size(); ++v) {
        cumulative += histogram[v];
        if (histogram[v] > 0 && cumulative >= rank) {
            return static_cast<std::uint32_t>(v);
        }
    }
    // Unreachable when n is the histogram total.
    for (std::size_t v = histogram.size(); v-- > 0;) {
        if (histogram[v] > 0) {
            return static_cast<std::uint32_t>(v);
        }
    }
    return 0;
}

std::string toDecimal(unsigned __int128 value)
{
    if (value == 0) {
        return "0";
    }
    std::string digits;
    while (value > 0) {
        digits.push_back(static_cast<char>('0' + static_cast<int>(value % 10)));
        value /= 10;
    }
    std::reverse(digits.begin(), digits.end());
    return digits;
}

std::vector<std::pair<int, std::uint64_t>> levelVoxelCounts(Volume& volume, const ShapeSpec& spec, const FrameMap& map)
{
    std::vector<std::pair<int, std::uint64_t>> counts;
    for (const int level : volume.presentScaleLevels()) {
        counts.emplace_back(level, shapeIndexRange(spec, map, level, volume.shape(level)).count());
    }
    return counts;
}

int chooseLevel(Volume& volume, const ShapeSpec& spec, const FrameMap& map, std::uint64_t voxelBudget)
{
    const auto counts = levelVoxelCounts(volume, spec, map);
    if (counts.empty()) {
        return 0;
    }
    for (const auto& [level, count] : counts) {
        if (count <= voxelBudget) {
            return level;
        }
    }
    return counts.back().first;
}

std::optional<VolumeStats> computeStats(
    Volume& volume, const std::string& volumeId, const ShapeSpec& spec, const FrameMap& map, const StatsRequest& request, const Progress& progress, std::string* error)
{
    const auto started = std::chrono::steady_clock::now();
    const auto setError = [error](std::string message) {
        if (error) {
            *error = std::move(message);
        }
    };
    const auto cache = volume.sharedChunkCache();
    vc::render::IChunkedArray* array = cache.get();
    if (!array) {
        setError("the volume has no chunk source");
        return std::nullopt;
    }
    const bool wide = array->dtype() == vc::render::ChunkDtype::UInt16;
    const std::size_t bins = wide ? 65536 : 256;
    for (const double p : request.percentiles) {
        if (!isValidPercentile(p)) {
            setError("percentiles must be in [0, 100] with at most 4 decimals");
            return std::nullopt;
        }
    }

    const RayArc* arc = std::get_if<RayArc>(&spec.shape);
    if (arc && !spec.arc) {
        setError("the ray arc's umbilicus is not compiled");
        return std::nullopt;
    }
    const int level = request.level >= 0 ? request.level : chooseLevel(volume, spec, map, request.voxelBudget);
    if (!volume.hasScaleLevel(level) || level >= array->numLevels()) {
        setError("level " + std::to_string(level) + " is not stored in this volume");
        return std::nullopt;
    }
    VolumeStats stats;
    stats.volumeId = volumeId;
    stats.dtype = wide ? "uint16" : "uint8";
    stats.level = level;
    stats.range = shapeIndexRange(spec, map, level, array->shape(level));
    stats.threshold = request.threshold;
    stats.binWidth = wide ? 256 : 1;
    try {
        stats.levelVoxelSizeUm = volume.voxelSize() * std::ldexp(1.0, level);
    } catch (...) {
        stats.levelVoxelSizeUm = 0;
    }
    if (stats.range.empty()) {
        setError("the region is outside this volume at level " + std::to_string(level));
        return std::nullopt;
    }

    const int threads = std::clamp(request.threads, 1, 32);
    std::vector<std::vector<std::uint64_t>> histograms(static_cast<std::size_t>(threads), std::vector<std::uint64_t>(bins, 0));
    const double rawFill = array->fillValue();
    const auto fill = static_cast<std::size_t>(std::clamp(std::isfinite(rawFill) ? rawFill : 0.0, 0.0, static_cast<double>(bins - 1)));

    // A ray arc is scanned over its bounding range: chunks wholly outside it
    // are never read, chunks wholly inside count every voxel, and the rest
    // test each voxel's region point i * 2^L / k.
    const double s = std::ldexp(1.0, level - map.log2RegionToViewer);
    const auto coverageOf = [&](const cv::Vec3i& fromZyx, const cv::Vec3i& toZyx) {
        if (!arc) {
            return Coverage::Inside;
        }
        return classify(*arc, *spec.arc, cv::Vec3d(fromZyx[2] * s, fromZyx[1] * s, fromZyx[0] * s), cv::Vec3d(toZyx[2] * s, toZyx[1] * s, toZyx[0] * s));
    };
    std::function<bool(const cv::Vec3i&, const cv::Vec3i&)> keepChunk;
    if (arc) {
        keepChunk = [&](const cv::Vec3i& fromZyx, const cv::Vec3i& toZyx) {
            if (coverageOf(fromZyx, toZyx) == Coverage::Outside) {
                ++stats.chunksSkipped;
                return false;
            }
            return true;
        };
    }

    const ChunkVisitor visit = [&](int worker, const ChunkView& chunk) {
        auto& h = histograms[static_cast<std::size_t>(worker)];
        const std::uint64_t nz = static_cast<std::uint64_t>(chunk.toZyx[0] - chunk.fromZyx[0] + 1);
        const std::uint64_t ny = static_cast<std::uint64_t>(chunk.toZyx[1] - chunk.fromZyx[1] + 1);
        const std::uint64_t nx = static_cast<std::uint64_t>(chunk.toZyx[2] - chunk.fromZyx[2] + 1);
        const std::size_t strideY = static_cast<std::size_t>(chunk.chunkShapeZyx[2]);
        const std::size_t strideZ = strideY * static_cast<std::size_t>(chunk.chunkShapeZyx[1]);
        if (coverageOf(chunk.fromZyx, chunk.toZyx) != Coverage::Inside) {
            for (int z = chunk.fromZyx[0]; z <= chunk.toZyx[0]; ++z) {
                for (int y = chunk.fromZyx[1]; y <= chunk.toZyx[1]; ++y) {
                    const std::size_t rowStart = static_cast<std::size_t>(z - chunk.chunkOriginZyx[0]) * strideZ +
                                                 static_cast<std::size_t>(y - chunk.chunkOriginZyx[1]) * strideY;
                    for (int x = chunk.fromZyx[2]; x <= chunk.toZyx[2]; ++x) {
                        if (!contains(*arc, *spec.arc, cv::Vec3d(x * s, y * s, z * s))) {
                            continue;
                        }
                        std::size_t value = fill;
                        if (!chunk.allFill && chunk.data) {
                            const std::size_t at = rowStart + static_cast<std::size_t>(x - chunk.chunkOriginZyx[2]);
                            value = wide ? reinterpret_cast<const std::uint16_t*>(chunk.data)[at]
                                         : reinterpret_cast<const std::uint8_t*>(chunk.data)[at];
                        }
                        ++h[value];
                    }
                }
            }
            return;
        }
        if (chunk.allFill || !chunk.data) {
            h[fill] += nz * ny * nx;
            return;
        }
        for (int z = chunk.fromZyx[0]; z <= chunk.toZyx[0]; ++z) {
            for (int y = chunk.fromZyx[1]; y <= chunk.toZyx[1]; ++y) {
                const std::size_t rowStart = static_cast<std::size_t>(z - chunk.chunkOriginZyx[0]) * strideZ +
                                             static_cast<std::size_t>(y - chunk.chunkOriginZyx[1]) * strideY +
                                             static_cast<std::size_t>(chunk.fromZyx[2] - chunk.chunkOriginZyx[2]);
                if (wide) {
                    const auto* row = reinterpret_cast<const std::uint16_t*>(chunk.data) + rowStart;
                    for (std::uint64_t x = 0; x < nx; ++x) {
                        ++h[row[x]];
                    }
                } else {
                    const auto* row = reinterpret_cast<const std::uint8_t*>(chunk.data) + rowStart;
                    for (std::uint64_t x = 0; x < nx; ++x) {
                        ++h[row[x]];
                    }
                }
            }
        }
    };

    std::string readError;
    const bool completed =
        forEachChunk(volume, level, stats.range, threads, progress, visit, &readError, &stats.chunksRead, &stats.fillChunks, keepChunk);
    if (!completed && !readError.empty()) {
        setError(readError);
        return std::nullopt;
    }
    stats.canceled = !completed;

    std::vector<std::uint64_t> merged(bins, 0);
    for (const auto& h : histograms) {
        for (std::size_t v = 0; v < bins; ++v) {
            merged[v] += h[v];
        }
    }
    const Moments m = momentsOf(merged);
    stats.count = m.n;
    stats.sum = m.s1;
    stats.sumSquares = m.s2;
    if (m.n > 0) {
        for (std::size_t v = 0; v < bins; ++v) {
            if (merged[v] > 0) {
                stats.min = static_cast<std::uint32_t>(v);
                break;
            }
        }
        for (std::size_t v = bins; v-- > 0;) {
            if (merged[v] > 0) {
                stats.max = static_cast<std::uint32_t>(v);
                break;
            }
        }
        const double n = static_cast<double>(m.n);
        stats.mean = static_cast<double>(m.s1) / n;
        const unsigned __int128 nS2 = static_cast<unsigned __int128>(m.n) * m.s2;
        const unsigned __int128 s1Squared = static_cast<unsigned __int128>(m.s1) * m.s1;
        const unsigned __int128 numerator = nS2 >= s1Squared ? nS2 - s1Squared : 0;
        stats.stddev = std::sqrt(static_cast<double>(numerator) / (n * n));
        for (const double p : request.percentiles) {
            stats.percentiles.emplace_back(p, percentileOf(merged, m.n, p));
        }
        if (stats.threshold) {
            for (std::size_t v = 0; v < bins; ++v) {
                if (static_cast<double>(v) > *stats.threshold) {
                    stats.countAbove += merged[v];
                }
            }
            stats.fractionAbove = static_cast<double>(stats.countAbove) / n;
        }
    }
    for (std::size_t v = 0; v < bins; ++v) {
        stats.histogram[wide ? (v >> 8) : v] += merged[v];
    }
    stats.volumeMm3 = static_cast<double>(stats.count) * std::pow(stats.levelVoxelSizeUm, 3.0) * 1e-9;
    stats.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    return stats;
}

Json statsJson(const VolumeStats& stats, const Region& region, const FrameMap& map, const std::string& location, const std::string& created)
{
    Json doc = Json::object();
    doc["schema"] = "vc3d.region_stats";
    doc["version"] = 1;
    doc["region_id"] = region.id;
    doc["region_revision"] = region.revision;
    doc["shape_type"] = std::string(shapeTypeName(region.shape));
    Json volume = Json::object();
    volume["id"] = stats.volumeId;
    volume["location"] = location;
    volume["dtype"] = stats.dtype;
    volume["voxel_size_um"] = stats.levelVoxelSizeUm / std::ldexp(1.0, stats.level);
    doc["volume"] = std::move(volume);
    doc["level"] = stats.level;
    doc["level_scale"] = 1LL << stats.level;
    doc["region_to_viewer"] = map.regionToViewer();
    Json range = Json::object();
    range["min"] = Json::array({stats.range.minZyx[0], stats.range.minZyx[1], stats.range.minZyx[2]});
    range["max"] = Json::array({stats.range.maxZyx[0], stats.range.maxZyx[1], stats.range.maxZyx[2]});
    doc["index_range_zyx"] = std::move(range);
    doc["count"] = stats.count;
    doc["min"] = stats.min;
    doc["max"] = stats.max;
    doc["sum"] = toDecimal(stats.sum);
    doc["sum_squares"] = toDecimal(stats.sumSquares);
    doc["mean"] = stats.mean;
    doc["std"] = stats.stddev;
    Json percentiles = Json::array();
    for (const auto& [p, value] : stats.percentiles) {
        Json entry = Json::object();
        entry["p"] = p;
        entry["value"] = value;
        percentiles.push_back(std::move(entry));
    }
    doc["percentiles"] = std::move(percentiles);
    Json histogram = Json::object();
    histogram["bin_width"] = stats.binWidth;
    histogram["counts"] = Json::array();
    for (const auto count : stats.histogram) {
        histogram["counts"].push_back(count);
    }
    doc["histogram"] = std::move(histogram);
    if (stats.threshold) {
        doc["threshold"] = *stats.threshold;
    } else {
        doc["threshold"] = nullptr;
    }
    doc["count_above"] = stats.countAbove;
    doc["fraction_above"] = stats.fractionAbove;
    doc["volume_mm3"] = stats.volumeMm3;
    doc["chunks_read"] = stats.chunksRead;
    doc["chunks_skipped"] = stats.chunksSkipped;
    doc["fill_chunks"] = stats.fillChunks;
    doc["seconds"] = stats.seconds;
    doc["vc3d_version"] = ProjectInfo::NameAndVersion();
    doc["repository_hash"] = ProjectInfo::RepositoryHash();
    doc["created"] = created;
    return doc;
}

std::string statsFileName(const std::string& volumeId, int level)
{
    std::string name = volumeId.empty() ? std::string("volume") : volumeId;
    for (char& ch : name) {
        const auto c = static_cast<unsigned char>(ch);
        if (!std::isalnum(c) && ch != '.' && ch != '-' && ch != '_') {
            ch = '_';
        }
    }
    return name + "-L" + std::to_string(level) + ".json";
}

}  // namespace vc3d::regions
