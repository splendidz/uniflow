// ======================================================================
//  picker_motion.cpp - constructor for the shared picker motion base.
// ======================================================================
#include "picker_motion.h"

PickerMotion::PickerMotion(double home_x_mm)
    : x_axis_(home_x_mm),
      z_axis_(GlobalGeometry::kZUp_mm),
      finger_axis_(GlobalGeometry::kFingerOpen_mm)
{
    finger_axis_.SetTarget(GlobalGeometry::kFingerOpen_mm);
}
