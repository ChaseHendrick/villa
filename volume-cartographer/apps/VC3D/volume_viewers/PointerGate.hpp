#pragma once

#include <QPointF>
#include <Qt>

#include <functional>

#include <opencv2/core/matx.hpp>

namespace vc3d
{

// Pointer input a slice viewer offers to one tool before its own handling.
// volumePos is in viewer coordinates.
struct PointerEvent {
    enum class Kind { Press, DoubleClick, Move, Release, Cancel };
    Kind kind = Kind::Press;
    QPointF scenePos;
    cv::Vec3f volumePos{0, 0, 0};
    Qt::MouseButton button = Qt::NoButton;
    Qt::MouseButtons buttons = Qt::NoButton;
    Qt::KeyboardModifiers modifiers = Qt::NoModifier;
};

// Returns true when the event was consumed.
using PointerOverride = std::function<bool(const PointerEvent&)>;

// Owns "this left-button gesture belongs to the override", so the click and
// double click Qt and CVolumeViewerView emit around a consumed press are
// swallowed too. Header-only so a test can drive it with a real
// CVolumeViewerView.
class PointerGate
{
public:
    // Press or DoubleClick, offered for every press so a new press always
    // ends a canceled gesture; only a plain left button reaches the override.
    bool offerPress(const PointerOverride& override, const PointerEvent& event)
    {
        _canceledHeld = false;
        if (!override || event.button != Qt::LeftButton || event.modifiers != Qt::NoModifier) {
            return false;
        }
        if (!override(event)) {
            return false;
        }
        _owned = true;
        return true;
    }
    // Offered for every move (hover previews); consumed when the override
    // returns true or while it owns the gesture. The rest of a canceled
    // gesture is consumed without being offered, until the left button is up
    // (its release can be lost to a modal dialog or a focus change).
    bool offerMove(const PointerOverride& override, const PointerEvent& event)
    {
        if (_canceledHeld) {
            if (event.buttons.testFlag(Qt::LeftButton)) {
                return true;
            }
            _canceledHeld = false;
        }
        if (!override) {
            return false;
        }
        const bool consumed = override(event);
        return consumed || _owned;
    }
    bool swallowClick() const { return _owned || _canceledHeld; }
    // Ends an owned gesture and delivers the left release to the override.
    // The release of a canceled gesture is swallowed and not delivered.
    bool offerRelease(const PointerOverride& override, const PointerEvent& event)
    {
        if (event.button != Qt::LeftButton) {
            return false;
        }
        if (_canceledHeld) {
            _canceledHeld = false;
            return true;
        }
        if (!_owned) {
            return false;
        }
        _owned = false;
        if (override) {
            override(event);
        }
        return true;
    }
    // Esc or a tool change: ends an owned gesture with Kind::Cancel and
    // swallows the rest of it (moves, the click and the release), so the
    // button coming up never reaches point selection or a fiber start.
    // With no gesture the override still sees the Cancel (Esc leaves its
    // tool) and decides whether it was consumed.
    bool offerCancel(const PointerOverride& override)
    {
        const bool wasOwned = _owned;
        _owned = false;
        if (wasOwned) {
            _canceledHeld = true;
        }
        if (!override) {
            return wasOwned;
        }
        PointerEvent event;
        event.kind = PointerEvent::Kind::Cancel;
        return override(event) || wasOwned;
    }
    bool owned() const { return _owned; }

private:
    bool _owned = false;
    bool _canceledHeld = false;
};

}  // namespace vc3d
