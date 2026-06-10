// city_traffic - a city-driving simulation built entirely on uniflow
// (single pump thread, no application-level threading). Intersections and,
// later, vehicles are independent uniflow modules sharing an immutable Map and
// a mutable World; the shared state is lock-free because every step runs on
// the one pump thread.
//
// Built incrementally (UI-first):
//   step 1 - static map render                                  [done]
//   step 2 - per-intersection traffic signals (independent)     [done]
//   step 3 - one vehicle: acceleration, signals, turns          [done]
//   step 4 - fleet: car-ahead safety distance, yielding         [this commit]
#include "app.h"
#include "uf_visualization.h"

int main()
{
    App& app = App::inst(); // phase 1: Runtime + modules constructed
    app.Start();            // phase 2: flows armed

    RunVisualisation();     // main-thread render loop (Win32)

    app.Shutdown();
    return 0;
}
