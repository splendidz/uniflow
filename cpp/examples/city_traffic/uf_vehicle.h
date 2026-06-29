// uf_vehicle.h - one car as a uniflow module. The driving loop is a single
// perpetual task whose steps form the state machine:
//
//   Cruise  - accelerate toward cruise speed; brake to a stop at the stop line
//             if the approach signal is not green
//   Wait    - held at the stop line until the signal turns green
//   Cross   - drive through the junction; at the far node pick the next edge
//             (probabilistic turn, straight preferred, no U-turn) and loop
//   Turn    - arc through a junction/corner, then re-enter Cruise on the new edge
//
// One flow = one car = one task. The task OWNS all the kinematic state and the
// steps; the flow is the shell that binds it. Position is a directed edge
// (from -> to) plus a fraction dist in [0,1]; all edges are one grid unit long,
// so dist doubles as distance travelled.
#pragma once

#include "uniflow.hpp"

#include <cstdint>
#include <random>

class Flow_Vehicle : public uniflow::Uniflow<Flow_Vehicle>
{
public:
    Flow_Vehicle(uniflow::Runtime& rt, int id, int from, int to, double dist0,
                 uint8_t r, uint8_t g, uint8_t b);

    struct Task_Drive : uniflow::Task<Flow_Vehicle>
    {
        void Init(int id, int from, int to, double dist0,
                  uint8_t r, uint8_t g, uint8_t b);   // called once from the flow ctor
        StepResult Entry() override { return Step_Cruise(); }

    private:
        StepResult Step_Cruise();
        StepResult Step_Wait();
        StepResult Step_Cross();
        StepResult Step_Turn();                  // arc through a junction/corner

        double Dt();                              // seconds since the last tick
        void   ApproachSpeed(double target, double dt);
        void   UpdateHeading();
        void   UpdateEdgePos();                   // centreline pos + heading on edge
        void   Publish();
        int    PickNextNode();                    // probabilistic turn (no U-turn)
        // Centre-to-centre distance to the nearest leader on this directed edge,
        // optionally continuing onto edge to_ -> next. Large value if none.
        double GapAhead(int next) const;
        // Position of the car nearest the node on edge to_ -> next (i.e. how far
        // the back of the queue sits from the junction). Large value if empty.
        double NextEdgeRoom(int next) const;
        int    TurnDir(int n) const;              // -1 left, 0 straight, +1 right

        int                id_    = 0;
        int                from_  = 0;
        int                to_    = 0;
        int                next_  = -1;           // committed turn while crossing
        int                blink_ = 0;            // turn signal: -1 left, +1 right
        double             dist_  = 0.0;          // [0,1] along edge from_ -> to_
        double             speed_ = 0.0;          // grid units per second
        double             turnS_ = 0.0;          // arc parameter [0,1] while turning
        int                ta_    = -1;           // arc nodes captured at turn start
        int                tnode_ = -1;
        int                tc_    = -1;
        double             px_    = 0.0;          // published centreline position
        double             py_    = 0.0;
        double             hx_    = 1.0;          // heading (normalised grid)
        double             hy_    = 0.0;
        uint8_t            r_ = 0, g_ = 0, b_ = 0;
        uniflow::TimePoint last_{};
        std::mt19937       rng_;
    } task_;
};
