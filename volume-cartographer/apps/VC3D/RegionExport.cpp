#include "RegionExport.hpp"

#include "vc/core/Version.hpp"
#include "vc/core/render/ChunkCache.hpp"
#include "vc/core/render/IChunkedArray.hpp"
#include "vc/core/types/Array3D.hpp"
#include "vc/core/types/Volume.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <random>
#include <system_error>
#include <variant>

namespace fs = std::filesystem;

namespace vc3d::regions
{

namespace
{

std::string randomHex(int digits)
{
    std::random_device device;
    std::mt19937_64 rng((static_cast<std::uint64_t>(device()) << 32) ^ device());
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text;
    for (int i = 0; i < digits; ++i) {
        text.push_back(kHex[rng() % 16]);
    }
    return text;
}

std::string formatGiB(std::uint64_t bytes)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", static_cast<double>(bytes) / static_cast<double>(1ull << 30));
    return buffer;
}

// Copies the range [minZyx, maxZyx] of `level` into `block` (Array3D is
// indexed (z, y, x) relative to minZyx).
template <typename T>
bool readBlock(Volume& volume, int level, const LevelRange& sub, T fill, Array3D<T>& block, const Progress& progress, std::string* error)
{
    block.fill(fill);
    const ChunkVisitor visit = [&](int, const ChunkView& chunk) {
        if (chunk.allFill || !chunk.data) {
            return;  // already the fill value
        }
        const std::size_t strideY = static_cast<std::size_t>(chunk.chunkShapeZyx[2]);
        const std::size_t strideZ = strideY * static_cast<std::size_t>(chunk.chunkShapeZyx[1]);
        const auto* data = reinterpret_cast<const T*>(chunk.data);
        for (int z = chunk.fromZyx[0]; z <= chunk.toZyx[0]; ++z) {
            for (int y = chunk.fromZyx[1]; y <= chunk.toZyx[1]; ++y) {
                const std::size_t row = static_cast<std::size_t>(z - chunk.chunkOriginZyx[0]) * strideZ +
                                        static_cast<std::size_t>(y - chunk.chunkOriginZyx[1]) * strideY;
                for (int x = chunk.fromZyx[2]; x <= chunk.toZyx[2]; ++x) {
                    block(static_cast<std::size_t>(z - sub.minZyx[0]), static_cast<std::size_t>(y - sub.minZyx[1]), static_cast<std::size_t>(x - sub.minZyx[2])) =
                        data[row + static_cast<std::size_t>(x - chunk.chunkOriginZyx[2])];
                }
            }
        }
    };
    Progress quiet;
    quiet.cancel = progress.cancel;
    return forEachChunk(volume, level, sub, 1, quiet, visit, error);
}

template <typename T>
bool copyRange(Volume& volume, Volume& crop, const LevelRange& range, const std::array<int, 3>& blockShape, T fill, const Progress& progress, std::string* error, std::uint64_t* bytesWritten)
{
    const int size[3] = {
        range.maxZyx[0] - range.minZyx[0] + 1,
        range.maxZyx[1] - range.minZyx[1] + 1,
        range.maxZyx[2] - range.minZyx[2] + 1,
    };
    const int blocks[3] = {
        (size[0] + blockShape[0] - 1) / blockShape[0],
        (size[1] + blockShape[1] - 1) / blockShape[1],
        (size[2] + blockShape[2] - 1) / blockShape[2],
    };
    const std::uint64_t total = static_cast<std::uint64_t>(blocks[0]) * blocks[1] * blocks[2];
    std::uint64_t done = 0;
    for (int bz = 0; bz < blocks[0]; ++bz) {
        for (int by = 0; by < blocks[1]; ++by) {
            for (int bx = 0; bx < blocks[2]; ++bx) {
                if (progress.canceled()) {
                    return false;
                }
                const int z0 = bz * blockShape[0];
                const int y0 = by * blockShape[1];
                const int x0 = bx * blockShape[2];
                const int z1 = std::min(size[0], z0 + blockShape[0]) - 1;
                const int y1 = std::min(size[1], y0 + blockShape[1]) - 1;
                const int x1 = std::min(size[2], x0 + blockShape[2]) - 1;
                LevelRange sub;
                sub.level = range.level;
                sub.minZyx = {range.minZyx[0] + z0, range.minZyx[1] + y0, range.minZyx[2] + x0};
                sub.maxZyx = {range.minZyx[0] + z1, range.minZyx[1] + y1, range.minZyx[2] + x1};
                Array3D<T> block({static_cast<std::size_t>(z1 - z0 + 1), static_cast<std::size_t>(y1 - y0 + 1), static_cast<std::size_t>(x1 - x0 + 1)});
                if (!readBlock(volume, range.level, sub, fill, block, progress, error)) {
                    return false;
                }
                crop.writeZYX(block, {z0, y0, x0}, 0);
                *bytesWritten += static_cast<std::uint64_t>(block.size()) * sizeof(T);
                ++done;
                if (progress.report) {
                    progress.report(static_cast<double>(done) / static_cast<double>(total));
                }
            }
        }
    }
    return true;
}

// 255 where a crop voxel is inside the shape, else 0.
bool writeMask(const ShapeSpec& spec, const FrameMap& map, const LevelRange& range, const std::array<int, 3>& blockShape, Volume& mask, const Progress& progress, std::uint64_t* bytesWritten)
{
    const int size[3] = {
        range.maxZyx[0] - range.minZyx[0] + 1,
        range.maxZyx[1] - range.minZyx[1] + 1,
        range.maxZyx[2] - range.minZyx[2] + 1,
    };
    for (int z0 = 0; z0 < size[0]; z0 += blockShape[0]) {
        for (int y0 = 0; y0 < size[1]; y0 += blockShape[1]) {
            for (int x0 = 0; x0 < size[2]; x0 += blockShape[2]) {
                if (progress.canceled()) {
                    return false;
                }
                const int nz = std::min(blockShape[0], size[0] - z0);
                const int ny = std::min(blockShape[1], size[1] - y0);
                const int nx = std::min(blockShape[2], size[2] - x0);
                Array3D<std::uint8_t> block({static_cast<std::size_t>(nz), static_cast<std::size_t>(ny), static_cast<std::size_t>(nx)});
                for (int z = 0; z < nz; ++z) {
                    for (int y = 0; y < ny; ++y) {
                        for (int x = 0; x < nx; ++x) {
                            const bool inside =
                                containsIndex(spec, map, range.level, range.minZyx[0] + z0 + z, range.minZyx[1] + y0 + y, range.minZyx[2] + x0 + x);
                            block(static_cast<std::size_t>(z), static_cast<std::size_t>(y), static_cast<std::size_t>(x)) = inside ? 255 : 0;
                        }
                    }
                }
                mask.writeZYX(block, {z0, y0, x0}, 0);
                *bytesWritten += static_cast<std::uint64_t>(block.size());
            }
        }
    }
    return true;
}

}  // namespace

std::uint64_t estimateExportBytes(Volume& volume, const ShapeSpec& spec, const FrameMap& map, int level)
{
    auto* array = volume.chunkedCache();
    if (!array || !volume.hasScaleLevel(level)) {
        return 0;
    }
    const std::uint64_t voxelBytes = array->dtype() == vc::render::ChunkDtype::UInt16 ? 2 : 1;
    const std::uint64_t voxels = shapeIndexRange(spec, map, level, array->shape(level)).count();
    // A ray arc also writes a uint8 mask of the same shape.
    return voxels * voxelBytes + (std::holds_alternative<RayArc>(spec.shape) ? voxels : 0);
}

fs::path defaultExportDir(const fs::path& regionsDir, const std::string& regionId, const std::string& createdUtc)
{
    std::string stamp;
    for (const char ch : createdUtc) {
        if (ch != '-' && ch != ':') {
            stamp.push_back(ch);
        }
    }
    return regionsDir / "exports" / (regionId.substr(0, 8) + "-" + stamp + "-" + randomHex(4));
}

ExportResult exportCrop(
    Volume& volume, const std::string& volumeId, const std::string& location, const Region& region, const FrameMap& map, const ExportRequest& request, const Progress& progress)
{
    ExportResult out;
    const auto cache = volume.sharedChunkCache();
    vc::render::IChunkedArray* array = cache.get();
    if (!array) {
        out.error = "the volume has no chunk source";
        return out;
    }
    const int level = request.level;
    if (!volume.hasScaleLevel(level) || level >= array->numLevels()) {
        out.error = "level " + std::to_string(level) + " is not stored in this volume";
        return out;
    }
    std::string specError;
    const auto spec = shapeSpec(region, &specError);
    if (!spec) {
        out.error = specError;
        return out;
    }
    const bool withMask = std::holds_alternative<RayArc>(spec->shape);
    const LevelRange range = shapeIndexRange(*spec, map, level, array->shape(level));
    if (range.empty()) {
        out.error = "the region is outside this volume at level " + std::to_string(level);
        return out;
    }
    const bool wide = array->dtype() == vc::render::ChunkDtype::UInt16;
    const std::uint64_t bytes = range.count() * ((wide ? 2 : 1) + (withMask ? 1 : 0));
    if (bytes > request.maxBytes) {
        out.error = "the crop would be " + formatGiB(bytes) + " GiB; exports above " + formatGiB(request.maxBytes) +
                    " GiB are refused (choose a coarser level)";
        return out;
    }
    if (request.outDir.empty()) {
        out.error = "no output folder";
        return out;
    }
    std::error_code ec;
    if (fs::exists(request.outDir, ec)) {
        if (!fs::is_directory(request.outDir, ec) || !fs::is_empty(request.outDir, ec)) {
            out.error = "the output folder " + request.outDir.string() + " is not empty";
            return out;
        }
    }
    const fs::path partial = request.outDir.parent_path() / (request.outDir.filename().string() + ".partial-" + randomHex(8));
    fs::create_directories(partial, ec);
    if (ec) {
        out.error = "could not create " + partial.string() + ": " + ec.message();
        return out;
    }
    const auto cleanup = [&partial]() {
        std::error_code ignored;
        fs::remove_all(partial, ignored);
    };

    double voxelSize = 1.0;
    try {
        voxelSize = volume.voxelSize();
    } catch (...) {
    }
    const double levelScale = std::ldexp(1.0, level);
    const std::array<int, 3> shape = {
        range.maxZyx[0] - range.minZyx[0] + 1,
        range.maxZyx[1] - range.minZyx[1] + 1,
        range.maxZyx[2] - range.minZyx[2] + 1,
    };
    const std::array<int, 3> chunk = {std::min(64, shape[0]), std::min(64, shape[1]), std::min(64, shape[2])};
    try {
        Volume::ZarrCreateOptions options;
        options.shapeZYX = {static_cast<std::size_t>(shape[0]), static_cast<std::size_t>(shape[1]), static_cast<std::size_t>(shape[2])};
        options.chunkShapeZYX = {static_cast<std::size_t>(chunk[0]), static_cast<std::size_t>(chunk[1]), static_cast<std::size_t>(chunk[2])};
        options.dtype = array->dtype();
        options.numLevels = 1;
        options.fillValue = array->fillValue();
        // Volume::New writes uncompressed chunks: its codec table is compiled
        // out, so any other compressor would fail at the first write.
        options.compressor = "none";
        options.voxelSize = voxelSize * levelScale;
        options.name = region.name.empty() ? region.id : region.name;
        auto crop = Volume::New(partial / "volume.zarr", options);
        std::string error;
        bool completed = false;
        const double fill = std::isfinite(array->fillValue()) ? array->fillValue() : 0.0;
        if (wide) {
            completed =
                copyRange<std::uint16_t>(volume, *crop, range, chunk, static_cast<std::uint16_t>(std::clamp(fill, 0.0, 65535.0)), progress, &error, &out.bytesWritten);
        } else {
            completed =
                copyRange<std::uint8_t>(volume, *crop, range, chunk, static_cast<std::uint8_t>(std::clamp(fill, 0.0, 255.0)), progress, &error, &out.bytesWritten);
        }
        if (completed && withMask) {
            // One level, so writeZYX's mean downsampling never touches the mask.
            Volume::ZarrCreateOptions maskOptions = options;
            maskOptions.dtype = vc::render::ChunkDtype::UInt8;
            maskOptions.fillValue = 0.0;
            auto mask = Volume::New(partial / "mask.zarr", maskOptions);
            completed = writeMask(*spec, map, range, chunk, *mask, progress, &out.bytesWritten);
        }
        if (!completed) {
            cleanup();
            if (!error.empty()) {
                out.error = error;
            } else {
                out.canceled = true;
            }
            return out;
        }
    } catch (const std::exception& e) {
        cleanup();
        out.error = std::string("export failed: ") + e.what();
        return out;
    }

    // The crop's index (z, y, x) maps to annotation (x, y, z) by
    // a = (i + i0) * 2^L / k.
    const double s = levelScale / map.regionToViewer();
    const cv::Vec3i i0(range.minZyx[2], range.minZyx[1], range.minZyx[0]);  // x y z
    Json manifest = Json::object();
    manifest["schema"] = "vc3d.region_export";
    manifest["version"] = 1;
    manifest["description"] =
        "The crop opens in VC3D as its own local volume with its own coordinates starting at 0; use the matrix to place it in the scan.";
    manifest["region"] = toJson(region);
    Json source = Json::object();
    source["id"] = volumeId;
    source["location"] = location;
    source["level"] = level;
    source["dtype"] = wide ? "uint16" : "uint8";
    source["level_voxel_size_um"] = voxelSize * levelScale;
    manifest["source_volume"] = std::move(source);
    Json crop = Json::object();
    crop["index_origin_zyx"] = Json::array({range.minZyx[0], range.minZyx[1], range.minZyx[2]});
    crop["shape_zyx"] = Json::array({shape[0], shape[1], shape[2]});
    crop["viewer_origin_xyz"] = Json::array({i0[0] * levelScale, i0[1] * levelScale, i0[2] * levelScale});
    crop["annotation_origin_xyz"] = Json::array({i0[0] * s, i0[1] * s, i0[2] * s});
    crop["annotation_voxels_per_crop_voxel"] = s;
    crop["crop_index_zyx_to_annotation_xyz"] = Json::array({
        Json::array({0.0, 0.0, s, i0[0] * s}),
        Json::array({0.0, s, 0.0, i0[1] * s}),
        Json::array({s, 0.0, 0.0, i0[2] * s}),
        Json::array({0.0, 0.0, 0.0, 1.0}),
    });
    crop["files"] = withMask ? Json::array({"volume.zarr", "mask.zarr"}) : Json::array({"volume.zarr"});
    if (withMask) {
        crop["mask"] = "mask.zarr is 255 where a crop voxel's region point lies inside the ray arc and 0 elsewhere";
    }
    manifest["crop"] = std::move(crop);
    manifest["vc3d_version"] = ProjectInfo::NameAndVersion();
    manifest["repository_hash"] = ProjectInfo::RepositoryHash();
    manifest["author"] = request.author;
    manifest["created"] = request.created;

    {
        std::ofstream file(partial / "manifest.json", std::ios::binary | std::ios::trunc);
        file << manifest.dump(2) << "\n";
        if (!file.good()) {
            cleanup();
            out.error = "could not write the manifest in " + partial.string();
            return out;
        }
    }
    if (fs::exists(request.outDir, ec)) {
        fs::remove(request.outDir, ec);  // empty, checked above
    }
    ec.clear();
    fs::rename(partial, request.outDir, ec);
    if (ec) {
        cleanup();
        out.error = "could not move the crop into " + request.outDir.string() + ": " + ec.message();
        return out;
    }
    out.dir = request.outDir;
    out.manifest = std::move(manifest);
    return out;
}

}  // namespace vc3d::regions
