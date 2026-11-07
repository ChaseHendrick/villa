#pragma once

// Regions of interest on a label canvas: boxes with a note and a status, kept
// in the label sidecar. They mark where to label next, what is done, and which
// boxes hold the validation set.

#include <opencv2/core/types.hpp>

#include <QJsonArray>
#include <QString>

#include <optional>
#include <string>
#include <vector>

namespace vc3d::ink_labels {

enum class RegionStatus : uint8_t { Todo, Done, Skip };

struct LabelRegion {
    int id = 0;
    cv::Rect rect;  // label pixels (the render canvas at scale 1)
    std::string note;
    RegionStatus status = RegionStatus::Todo;
    // The box's pixels are in the validation set (its validation bits were
    // painted when this was turned on, and cleared when it was turned off).
    bool validation = false;
    std::string createdUtc;
    bool operator==(const LabelRegion&) const = default;
};
using RegionList = std::vector<LabelRegion>;

[[nodiscard]] const char* regionStatusName(RegionStatus status);
[[nodiscard]] std::optional<RegionStatus> regionStatusFromName(const QString& name);
[[nodiscard]] QJsonArray regionsToJson(const RegionList& regions);
// Drops entries that are not objects, have no positive size or repeat an id;
// boxes are clipped to `canvas` when it is not empty.
[[nodiscard]] RegionList regionsFromJson(const QJsonArray& array, cv::Size canvas = {});
[[nodiscard]] int nextRegionId(const RegionList& regions);
[[nodiscard]] const LabelRegion* findRegion(const RegionList& regions, int id);

}  // namespace vc3d::ink_labels
