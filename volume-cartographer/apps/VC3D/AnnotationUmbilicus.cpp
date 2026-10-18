#include "AnnotationUmbilicus.hpp"

#include "vc/core/types/Volume.hpp"
#include "vc/core/types/VolumePkg.hpp"

#include <cmath>

namespace vc3d::annotation
{

std::shared_ptr<Volume> attachedVolumeByStoreName(const VolumePkg& pkg, const std::string& storeName)
{
    const std::string wanted = std::filesystem::path(storeName).filename().string();
    if (wanted.empty())
        return nullptr;
    try {
        for (const std::string& id : pkg.volumeIDs()) {
            const std::shared_ptr<Volume> volume = pkg.volume(id);
            if (!volume || volume->isRemote())
                continue;
            if (volume->path().filename().string() == wanted)
                return volume;
        }
    } catch (...) {
    }
    return nullptr;
}

UmbilicusResolution resolveAnnotationUmbilicus(const VolumePkg& pkg, const AnnotationFrame& frame)
{
    UmbilicusResolution out;
    vc::core::util::ScrollUmbilicusResolution resolved = vc::core::util::resolveScrollUmbilicus(pkg);
    out.path = std::move(resolved.path);
    out.info = std::move(resolved.info);
    out.ambiguous = std::move(resolved.ambiguous);
    out.error = std::move(resolved.error);
    if (out.path.empty()) {
        return out;
    }

    const std::array<double, 3>& grid = frame.extentXyz;
    const bool haveGrid = grid[0] > 0.0 && grid[1] > 0.0 && grid[2] > 0.0;
    out.scale = vc::core::util::deriveUmbilicusScale(out.info, grid, frame.voxelSizeUm);
    out.claim = vc::core::util::umbilicusFrameClaim(out.info);

    // A stamp naming a volume this project has can be checked against it; one
    // it does not have merely goes unverified, which is not a contradiction.
    if (out.info.volume && !out.info.volume->empty()) {
        out.stampedVolume = attachedVolumeByStoreName(pkg, *out.info.volume);
        if (out.stampedVolume) {
            std::array<double, 3> namedGrid{0.0, 0.0, 0.0};
            std::optional<double> namedVoxelUm;
            try {
                namedGrid =
                    {static_cast<double>(out.stampedVolume->sliceWidth()),
                     static_cast<double>(out.stampedVolume->sliceHeight()),
                     static_cast<double>(out.stampedVolume->numSlices())};
                const double voxel = out.stampedVolume->voxelSize();
                if (std::isfinite(voxel) && voxel > 0.0) {
                    namedVoxelUm = voxel;
                }
            } catch (...) {
            }
            out.stampContradiction = vc::core::util::umbilicusStampContradiction(out.info, namedGrid, namedVoxelUm);
        }
    }

    out.action = vc::core::util::decideUmbilicusLoadAction(out.scale, out.claim, haveGrid, out.stampContradiction.has_value());
    return out;
}

std::vector<cv::Vec3f> framedControlPoints(const UmbilicusResolution& resolution)
{
    if (resolution.action != vc::core::util::UmbilicusLoadAction::Apply || !resolution.scale) {
        return {};
    }
    std::vector<cv::Vec3f> points = resolution.info.controlPoints;
    for (auto& point : points) {
        point *= static_cast<float>(resolution.scale->factor);
    }
    return points;
}

}  // namespace vc3d::annotation
