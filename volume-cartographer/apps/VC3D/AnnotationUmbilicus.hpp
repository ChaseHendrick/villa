#pragma once

// The project umbilicus, framed for consumers whose coordinates live in the
// annotation frame (fibers, line annotation, regions). One resolution path so
// the consumers cannot drift: the shared resolver picks the file, and
// deriveUmbilicusScale(), umbilicusStampContradiction() and
// decideUmbilicusLoadAction() decide which frame its points index and whether
// they may be used. Each consumer keeps its own messages and fallbacks.

#include "AnnotationFrame.hpp"

#include "vc/core/util/ScrollUmbilicus.hpp"
#include "vc/core/util/Umbilicus.hpp"

#include <opencv2/core/matx.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class Volume;
class VolumePkg;

namespace vc3d::annotation
{

struct UmbilicusResolution {
    // The chosen file; empty when nothing could be resolved (see error and
    // ambiguous).
    std::filesystem::path path;
    vc::core::util::UmbilicusFileInfo info;
    std::vector<std::filesystem::path> ambiguous;
    std::string error;

    // Only meaningful when path is set.
    std::optional<vc::core::util::UmbilicusScale> scale;
    vc::core::util::UmbilicusFrameClaim claim;
    // The attached local volume the file's stamp names, when the project has
    // it, and what it says against the stamp.
    std::shared_ptr<Volume> stampedVolume;
    std::optional<std::string> stampContradiction;
    vc::core::util::UmbilicusLoadAction action = vc::core::util::UmbilicusLoadAction::Refuse;
};

// The attached volume an umbilicus stamp names by store directory name, so the
// stamp can be checked against it. Remote volumes are skipped: the check needs
// local shape metadata.
std::shared_ptr<Volume> attachedVolumeByStoreName(const VolumePkg& pkg, const std::string& storeName);

// Resolves the package's umbilicus and decides how it maps into `frame`. The
// target grid counts as known only when all three extents are positive, which
// is every frame deriveAnnotationFrame() returns with a factor.
UmbilicusResolution resolveAnnotationUmbilicus(const VolumePkg& pkg, const AnnotationFrame& frame);

// The control points carried into the annotation frame; empty unless the
// action is Apply.
std::vector<cv::Vec3f> framedControlPoints(const UmbilicusResolution& resolution);

}  // namespace vc3d::annotation
