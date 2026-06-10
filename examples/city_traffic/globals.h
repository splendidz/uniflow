// globals.h - process-wide simulation flags shared by every module and the
// main-thread renderer.
#pragma once

namespace sim
{

// Cooperative stop request. Set on shutdown; every module's step loop polls
// it and ends its flow (returns Done) so WaitUntilIdle can join cleanly.
bool Stop();
void RequestStop();

} // namespace sim

// Shared geometry constants in grid units (1.0 = one road segment). Single
// source of truth so vehicle physics and the renderer agree on where the
// stop line is and which side the driving lane is on.
namespace geo
{
constexpr double kStopOffset = 0.32; // stop line this far before the junction
                                     // (clears the car length + junction pad so
                                     // a waiting nose stays out of cross traffic)
constexpr double kLaneHalf   = 0.10; // lane centre offset from the road centre
constexpr double kRoadWidth  = 0.38; // drawn road width (wide enough that a car
                                     // cutting a corner never reaches oncoming)
} // namespace geo
