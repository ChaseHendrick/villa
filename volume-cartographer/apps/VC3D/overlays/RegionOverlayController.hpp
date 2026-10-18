#pragma once

#include "ViewerOverlayControllerBase.hpp"

#include "../RegionModel.hpp"

#include <optional>

class RegionController;

// Draws every region that fits the current volume on the slice views: the
// plane's cross-section of a box (voxel faces) or the outline of a ray arc,
// its name, and the active region's handles while a region tool is on.
// Primitives are rebuilt from volume-space geometry on every request, so pan
// and zoom never show stale outlines.
class RegionOverlayController : public ViewerOverlayControllerBase
{
    Q_OBJECT

public:
    explicit RegionOverlayController(RegionController* regions, QObject* parent = nullptr);

protected:
    bool isOverlayEnabledFor(VolumeViewerBase* viewer) const override;
    void collectPrimitives(VolumeViewerBase* viewer, OverlayBuilder& builder) override;

private:
    struct ArcStyle {
        OverlayStyle outline;
        OverlayStyle handle;
        bool handles = false;
    };
    // Exact arcs on horizontal planes, the per-slice staircase on vertical
    // planes, nothing on tilted planes. Returns a label position when
    // anything was drawn.
    static std::optional<QPointF> drawArc(
        VolumeViewerBase* viewer,
        OverlayBuilder& builder,
        const vc3d::regions::RayArc& arc,
        const vc3d::regions::CompiledArc& compiled,
        const vc3d::regions::FrameMap& map,
        const vc3d::regions::Plane& plane,
        vc3d::regions::PlaneKind kind,
        const ArcStyle& style);

    RegionController* _regions = nullptr;
};
