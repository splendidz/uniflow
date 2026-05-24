// ======================================================================
//  picker_motion.h - shared motion + gripper state for both pickers.
//
//  Not a uniflow base. Composed by inheritance into LoadPicker and
//  UnloadPicker. Kept off the uniflow CRTP base because that base is
//  templated on Derived.
//
//  Three axes:
//    x_axis_      - linear travel along the rail (A <-> B <-> C).
//    z_axis_      - vertical up/down at a pick or place position.
//    finger_axis_ - separation between the two gripper fingers.
//                   target 0 = closed (gripping), kFingerOpen_mm = open.
//                   Initial pose is open: an idle picker is ready to grab.
// ======================================================================
#pragma once

#include "globals.h"

class PickerMotion
{
public:
    explicit PickerMotion(double home_x_mm);

    double X_mm()         const { return x_axis_.Position(); }
    double Z_mm()         const { return z_axis_.Position(); }
    double FingerGap_mm() const { return finger_axis_.Position(); }
    bool   Carrying()     const { return carrying_; }
    bool   InsideZoneB()  const { return GlobalGeometry::InsideZoneB(X_mm()); }

protected:
    void SetCarrying(bool v) { carrying_ = v; }

    MotorAxis x_axis_;
    MotorAxis z_axis_;
    MotorAxis finger_axis_;
    bool      carrying_ = false;
};
