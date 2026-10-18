#pragma once

#include "ViewerOverlayControllerBase.hpp"

class RegionController;

// Draws every region that fits the current volume on the slice views: the
// plane's cross-section of a box (voxel faces), its name, and the active
// region's handles while a region tool is on. Primitives are rebuilt from
// volume-space geometry on every request, so pan and zoom never show stale
// outlines.
class RegionOverlayController : public ViewerOverlayControllerBase
{
    Q_OBJECT

public:
    explicit RegionOverlayController(RegionController* regions, QObject* parent = nullptr);

protected:
    bool isOverlayEnabledFor(VolumeViewerBase* viewer) const override;
    void collectPrimitives(VolumeViewerBase* viewer, OverlayBuilder& builder) override;

private:
    RegionController* _regions = nullptr;
};
