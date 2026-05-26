// picker_motion.h - x/z/finger axes shared by Load- and UF_UnloadPicker via
// composition (not via the CRTP uniflow base, which is per-Derived).
// finger target: 0 = closed, kFingerOpen_mm = open.
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
