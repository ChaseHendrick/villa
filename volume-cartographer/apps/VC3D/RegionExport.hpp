#pragma once

// Saves the voxels of a region at one level as a local OME-Zarr crop with a
// manifest that places it in the scan. Free of Qt. The crop is built in a
// `.partial-*` folder and renamed into place only when complete, so a
// half-written export never looks valid.

#include "RegionModel.hpp"
#include "RegionStats.hpp"

#include <cstdint>
#include <filesystem>
#include <string>

class Volume;

namespace vc3d::regions
{

struct ExportRequest {
    // Absent or empty folder; a non-empty one is refused.
    std::filesystem::path outDir;
    int level = 0;
    std::uint64_t maxBytes = kMaxReadBytes;
    std::string author;
    std::string created;  // ISO 8601 UTC
};

struct ExportResult {
    std::filesystem::path dir;
    Json manifest = Json::object();
    std::uint64_t bytesWritten = 0;
    bool canceled = false;
    std::string error;
    bool ok() const { return error.empty() && !canceled; }
};

// Uncompressed size of the crop at `level` (a ray arc adds a uint8 mask).
std::uint64_t estimateExportBytes(Volume& volume, const ShapeSpec& spec, const FrameMap& map, int level);

// `<regions dir>/exports/<first 8 of id>-<YYYYMMDDTHHMMSSZ>-<4 hex>`, from an
// ISO 8601 UTC time such as "2026-10-18T14:10:00Z".
std::filesystem::path defaultExportDir(const std::filesystem::path& regionsDir, const std::string& regionId, const std::string& createdUtc);

ExportResult exportCrop(
    Volume& volume, const std::string& volumeId, const std::string& location, const Region& region, const FrameMap& map, const ExportRequest& request, const Progress& progress);

}  // namespace vc3d::regions
