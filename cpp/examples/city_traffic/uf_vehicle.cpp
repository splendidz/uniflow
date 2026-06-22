// uf_vehicle.cpp - vehicle kinematics and the driving flow. See uf_vehicle.h.
#include "uf_vehicle.h"

#include "globals.h"
#include "map.h"
#include "world.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace
{
constexpr double kCruise    = 0.55; // grid units / second (target cruise speed)
constexpr double kTurnSpeed = 0.26; // capped speed while turning through a node
constexpr double kAccel     = 0.70; // grid units / second^2 (accel and braking)
constexpr double kSafeGap   = 0.15; // min centre-to-centre distance to a leader
                                    // (tight so ~5 cars queue between junctions)
constexpr double kCornerCut = 0.12; // ~one car length: the turn arc starts this
                                    // far before the node and ends this far
                                    // after it, so the car pivots earlier
constexpr double kEnterClear = 0.27; // a car may enter a junction only if the
                                     // nearest car already on the exit edge is
                                     // at least this far in (else the exit is
                                     // backed up and we wait at the stop line)
constexpr double kJunctionZone = 0.25; // a car this far past our node is still
                                       // "in the junction" - we follow it so a
                                       // straight car never rear-ends a slow
                                       // turner crossing ahead of it

} // namespace

Flow_Vehicle::Flow_Vehicle(uniflow::Runtime& rt, int id, int from, int to,
                           double dist0, uint8_t r, uint8_t g, uint8_t b)
    : uniflow::Uniflow<Flow_Vehicle>(rt)
{
    AddTask(ctx_);
    ctx_.Init(id, from, to, dist0, r, g, b);
}

void Flow_Vehicle::Task_Drive::Init(int id, int from, int to, double dist0,
                                    uint8_t r, uint8_t g, uint8_t b)
{
    id_   = id;
    from_ = from;
    to_   = to;
    dist_ = dist0;
    r_    = r;
    g_    = g;
    b_    = b;
    last_ = uniflow::Clock::now();
    rng_.seed(static_cast<unsigned>(id * 2654435761u + 12345u));
    UpdateHeading();
}

double Flow_Vehicle::Task_Drive::Dt()
{
    auto   now = uniflow::Clock::now();
    double dt  = uniflow::to_ms(now - last_) / 1000.0;
    last_      = now;
    if (dt < 0.0)
        dt = 0.0;
    if (dt > 0.1) // clamp the first/large gap so position never jumps
        dt = 0.1;
    return dt;
}

void Flow_Vehicle::Task_Drive::ApproachSpeed(double target, double dt)
{
    double step = kAccel * dt;
    if (speed_ < target)
        speed_ = std::min(target, speed_ + step);
    else
        speed_ = std::max(target, speed_ - step);
    if (speed_ < 0.0)
        speed_ = 0.0;
}

void Flow_Vehicle::Task_Drive::UpdateHeading()
{
    const citymap::Node& a = citymap::NodeById(from_);
    const citymap::Node& b = citymap::NodeById(to_);
    double dx = b.gx - a.gx;
    double dy = b.gy - a.gy;
    double L  = std::sqrt(dx * dx + dy * dy);
    if (L > 1e-9)
    {
        hx_ = dx / L;
        hy_ = dy / L;
    }
}

void Flow_Vehicle::Task_Drive::UpdateEdgePos()
{
    const citymap::Node& a = citymap::NodeById(from_);
    const citymap::Node& b = citymap::NodeById(to_);
    px_ = a.gx + (b.gx - a.gx) * dist_;
    py_ = a.gy + (b.gy - a.gy) * dist_;
    UpdateHeading();
}

void Flow_Vehicle::Task_Drive::Publish()
{
    city::VehicleState s;
    s.from   = from_;
    s.to     = to_;
    s.dist   = dist_;
    s.gx     = px_;
    s.gy     = py_;
    s.dx     = hx_;
    s.dy     = hy_;
    s.blink  = blink_;
    s.active = true;
    s.r      = r_;
    s.g      = g_;
    s.b      = b_;
    city::SetVehicle(id_, s);
}

double Flow_Vehicle::Task_Drive::GapAhead(int next) const
{
    const std::vector<city::VehicleState>& vs = city::Vehicles();
    double                                 best = 1e9;
    for (int i = 0; i < static_cast<int>(vs.size()); ++i)
    {
        if (i == id_)
            continue;
        const city::VehicleState& o = vs[i];
        if (!o.active)
            continue;
        if (o.from == from_ && o.to == to_)
        {
            if (o.dist > dist_)
                best = std::min(best, o.dist - dist_);
        }
        else if (o.from == to_)
        {
            // A car that has crossed our node: either our path leader on the
            // next edge, or any car still inside the junction zone (so we
            // follow, not rear-end, a slow turner crossing ahead of us).
            bool myPath = (next >= 0 && o.to == next);
            if (myPath || o.dist < kJunctionZone)
                best = std::min(best, (1.0 - dist_) + o.dist);
        }
    }
    return best;
}

double Flow_Vehicle::Task_Drive::NextEdgeRoom(int next) const
{
    const std::vector<city::VehicleState>& vs = city::Vehicles();
    double                                 best = 1e9;
    for (int i = 0; i < static_cast<int>(vs.size()); ++i)
    {
        if (i == id_)
            continue;
        const city::VehicleState& o = vs[i];
        if (o.active && o.from == to_ && o.to == next)
            best = std::min(best, o.dist);
    }
    return best;
}

int Flow_Vehicle::Task_Drive::TurnDir(int n) const
{
    const citymap::Node& nt = citymap::NodeById(to_);
    const citymap::Node& nn = citymap::NodeById(n);
    double ox = nn.gx - nt.gx, oy = nn.gy - nt.gy;
    double L  = std::sqrt(ox * ox + oy * oy);
    if (L > 1e-9) { ox /= L; oy /= L; }
    double dot = hx_ * ox + hy_ * oy;
    if (dot > 0.8)
        return 0;                       // straight
    double cz = hx_ * oy - hy_ * ox;    // z of cross(heading, out); y-down screen
    return cz > 0 ? 1 : -1;             // +1 right, -1 left
}

int Flow_Vehicle::Task_Drive::PickNextNode()
{
    const int            prev = from_;
    const int            at   = to_;
    const citymap::Node& pa   = citymap::NodeById(prev);
    const citymap::Node& pat  = citymap::NodeById(at);

    double inx = pat.gx - pa.gx;
    double iny = pat.gy - pa.gy;
    double Li  = std::sqrt(inx * inx + iny * iny);
    if (Li > 1e-9)
    {
        inx /= Li;
        iny /= Li;
    }

    std::vector<int> cand;
    std::vector<int> candTurn; // -1 left, 0 straight, +1 right (parallel)
    for (int nb : citymap::NeighborsOf(at))
    {
        if (nb == prev)
            continue; // no U-turn
        const citymap::Node& n  = citymap::NodeById(nb);
        double               ox = n.gx - pat.gx;
        double               oy = n.gy - pat.gy;
        double               Lo = std::sqrt(ox * ox + oy * oy);
        if (Lo > 1e-9)
        {
            ox /= Lo;
            oy /= Lo;
        }
        double dot = inx * ox + iny * oy;
        if (dot < -0.5)
            continue;        // would be a U-turn
        double cz = inx * oy - iny * ox;
        cand.push_back(nb);
        candTurn.push_back(dot > 0.8 ? 0 : (cz > 0 ? 1 : -1));
    }

    if (cand.empty())
        return prev; // dead-end fallback (should not happen on this map)

    // No left turns in this build: drop left exits when a straight/right option
    // exists (keep them only as a last resort, e.g. a forced corner bend).
    {
        std::vector<int> kc, kt;
        for (std::size_t k = 0; k < cand.size(); ++k)
            if (candTurn[k] != -1)
            {
                kc.push_back(cand[k]);
                kt.push_back(candTurn[k]);
            }
        if (!kc.empty())
        {
            cand     = kc;
            candTurn = kt;
        }
    }

    // 60%: follow the car ahead on this edge, matching its turn direction, so
    // cars form platoons and the map looks busier (more moving at once).
    bool   hasLeader  = false;
    int    leaderTurn = 0;
    double bestDist    = 1e9;
    const std::vector<city::VehicleState>& vs = city::Vehicles();
    for (int i = 0; i < static_cast<int>(vs.size()); ++i)
    {
        if (i == id_)
            continue;
        const city::VehicleState& o = vs[i];
        if (o.active && o.from == from_ && o.to == to_ && o.dist > dist_ &&
            o.dist < bestDist)
        {
            bestDist   = o.dist;
            leaderTurn = o.blink; // -1 left, 0 straight, +1 right
            hasLeader  = true;
        }
    }
    if (hasLeader)
    {
        std::uniform_real_distribution<double> u(0.0, 1.0);
        if (u(rng_) < 0.6)
            for (std::size_t k = 0; k < cand.size(); ++k)
                if (candTurn[k] == leaderTurn)
                    return cand[k];
    }

    // otherwise pick uniformly among the exits
    std::uniform_int_distribution<std::size_t> d(0, cand.size() - 1);
    return cand[d(rng_)];
}

uniflow::StepResult Flow_Vehicle::Task_Drive::Step_Cruise()
{
    if (sim::Stop())
        return Done();

    // Decide the route for this edge up front, so we know which signal phase
    // to obey and can signal the turn on the approach.
    if (next_ < 0)
        next_ = PickNextNode();

    double               dt = Dt();
    const citymap::Node& a  = citymap::NodeById(from_);
    const citymap::Node& b  = citymap::NodeById(to_);
    city::Axis           ax = city::AxisOfEdge(a, b);
    int                  td = TurnDir(next_); // -1 left, 0 straight, +1 right
    bool junction = b.kind != citymap::NodeKind::Corner;
    blink_        = junction ? td : 0; // signal turns only at real junctions

    city::SignalState sig   = city::GetSignal(to_);
    bool              mayGo = city::MayProceed(sig, ax, td,
                                               city::AxisSingleEnded(to_, ax));
    bool              exitBlocked = NextEdgeRoom(next_) < kEnterClear;

    const double stopline  = 1.0 - geo::kStopOffset;
    const bool   need_stop = !mayGo || exitBlocked;

    // Nearest point we must not pass: the stop line (if we must stop) or the
    // safe distance behind the leader on this edge - whichever is closer.
    double bind = 1e9;
    if (need_stop)
        bind = std::min(bind, stopline);
    double leadGap = GapAhead(-1);
    if (leadGap < 1e8)
        bind = std::min(bind, dist_ + (leadGap - kSafeGap));

    double clearance = bind - dist_;
    double target =
        clearance <= 0.0 ? 0.0 : std::min(kCruise, std::sqrt(2.0 * kAccel * clearance));

    double prev = dist_;
    ApproachSpeed(target, dt);
    dist_ += speed_ * dt;
    if (dist_ > bind) // do not overshoot the binding constraint
    {
        dist_  = std::max(prev, bind);
        speed_ = 0.0;
    }
    UpdateEdgePos();
    Publish();

    if (dist_ >= stopline - 1e-9)
    {
        if (mayGo && !exitBlocked)
            return Next(UF_FN(Step_Cross));
        speed_ = 0.0;
        return Next(UF_FN(Step_Wait));
    }

    Describe("cruise");
    return Stay();
}

uniflow::StepResult Flow_Vehicle::Task_Drive::Step_Wait()
{
    if (sim::Stop())
        return Done();

    (void)Dt(); // keep the clock fresh so Cross starts with a small dt
    speed_ = 0.0;
    UpdateEdgePos();
    Publish();

    if (next_ < 0)
        next_ = PickNextNode();

    const citymap::Node& a  = citymap::NodeById(from_);
    const citymap::Node& b  = citymap::NodeById(to_);
    city::Axis           ax = city::AxisOfEdge(a, b);
    int                  td = TurnDir(next_);
    bool junction = b.kind != citymap::NodeKind::Corner;
    blink_        = junction ? td : 0; // keep signalling: turning, not stalled

    city::SignalState sig = city::GetSignal(to_);
    if (city::MayProceed(sig, ax, td, city::AxisSingleEnded(to_, ax)) &&
        NextEdgeRoom(next_) >= kEnterClear)
        return Next(UF_FN(Step_Cross));

    Describe("wait");
    return Stay();
}

uniflow::StepResult Flow_Vehicle::Task_Drive::Step_Cross()
{
    if (sim::Stop())
        return Done();

    if (next_ < 0)
        next_ = PickNextNode();

    double dt = Dt();

    // Is this move a turn? Compare incoming heading with the outgoing one.
    const citymap::Node& nt = citymap::NodeById(to_);
    const citymap::Node& nn = citymap::NodeById(next_);
    double ox = nn.gx - nt.gx, oy = nn.gy - nt.gy;
    double Lo = std::sqrt(ox * ox + oy * oy);
    if (Lo > 1e-9) { ox /= Lo; oy /= Lo; }
    double     turnDot  = hx_ * ox + hy_ * oy; // 1 straight, 0 = 90-degree turn
    const bool turning  = turnDot < 0.8;
    // A turn pivots early: hand off to the arc kCornerCut before the node.
    const double handoff = turning ? (1.0 - kCornerCut) : 1.0;

    double cz = hx_ * oy - hy_ * ox;           // turn direction (y-down screen)
    bool   atJunction = citymap::NodeById(to_).kind != citymap::NodeKind::Corner;
    blink_ = (turning && atJunction) ? (cz > 0 ? 1 : -1) : 0;

    double leadGap   = GapAhead(next_);
    double bind      = leadGap < 1e8 ? dist_ + (leadGap - kSafeGap) : 1e9;
    double clearance = bind - dist_; // leader only; handoff just clamps position
    double target =
        clearance <= 0.0 ? 0.0 : std::min(kCruise, std::sqrt(2.0 * kAccel * clearance));
    if (turning)
        target = std::min(target, kTurnSpeed); // arrive at the bend at turn speed

    double prev = dist_;
    ApproachSpeed(target, dt);
    dist_ += speed_ * dt;
    if (dist_ > bind)
    {
        dist_  = std::max(prev, bind);
        speed_ = 0.0;
    }
    if (dist_ > handoff)
        dist_ = handoff; // never roll past the arc's start under straight motion
    UpdateEdgePos();
    Publish();

    if (turning && dist_ >= handoff - 1e-9)
    {
        turnS_ = 0.0;
        ta_    = from_; // capture the arc's three nodes before we mutate them
        tnode_ = to_;
        tc_    = next_;
        return Next(UF_FN(Step_Turn));
    }
    if (!turning && dist_ >= 1.0 - 1e-9)
    {
        from_ = to_;
        to_   = next_;
        dist_ -= 1.0;
        if (dist_ < 0.0)
            dist_ = 0.0;
        next_ = -1;
        UpdateHeading();
        return Next(UF_FN(Step_Cruise));
    }

    Describe("cross");
    return Stay();
}

uniflow::StepResult Flow_Vehicle::Task_Drive::Step_Turn()
{
    if (sim::Stop())
        return Done();

    double dt = Dt();
    ApproachSpeed(kTurnSpeed, dt);

    // Quadratic Bezier across the corner, using the nodes captured at entry:
    // from kCornerCut before the node, through the node, to kCornerCut along
    // the next edge.
    const citymap::Node& a  = citymap::NodeById(ta_);    // edge start
    const citymap::Node& nd = citymap::NodeById(tnode_); // the node (corner)
    const citymap::Node& c  = citymap::NodeById(tc_);    // next edge end

    double inx = nd.gx - a.gx, iny = nd.gy - a.gy;
    double Li  = std::sqrt(inx * inx + iny * iny);
    if (Li > 1e-9) { inx /= Li; iny /= Li; }
    double oux = c.gx - nd.gx, ouy = c.gy - nd.gy;
    double Lo  = std::sqrt(oux * oux + ouy * ouy);
    if (Lo > 1e-9) { oux /= Lo; ouy /= Lo; }

    double p0x = nd.gx - inx * kCornerCut, p0y = nd.gy - iny * kCornerCut;
    double p1x = nd.gx,                    p1y = nd.gy;
    double p2x = nd.gx + oux * kCornerCut, p2y = nd.gy + ouy * kCornerCut;

    // Advance the arc parameter by distance / (approx arc length).
    const double approxLen = 1.6 * kCornerCut;
    turnS_ += (speed_ * dt) / approxLen;
    if (turnS_ > 1.0)
        turnS_ = 1.0;
    double s = turnS_;

    px_ = (1 - s) * (1 - s) * p0x + 2 * (1 - s) * s * p1x + s * s * p2x;
    py_ = (1 - s) * (1 - s) * p0y + 2 * (1 - s) * s * p1y + s * s * p2y;

    double tx = 2 * (1 - s) * (p1x - p0x) + 2 * s * (p2x - p1x);
    double ty = 2 * (1 - s) * (p1y - p0y) + 2 * s * (p2y - p1y);
    double Lt = std::sqrt(tx * tx + ty * ty);
    if (Lt > 1e-9) { hx_ = tx / Lt; hy_ = ty / Lt; }

    // Occupy the destination edge (from the node onward) so another car cannot
    // turn into the same spot and overlap us mid-corner.
    from_ = tnode_;
    to_   = tc_;
    dist_ = s * kCornerCut;
    Publish();

    if (turnS_ >= 1.0)
    {
        dist_ = kCornerCut; // emerge just past the node on the new edge
        next_ = -1;
        blink_ = 0;
        UpdateHeading();
        return Next(UF_FN(Step_Cruise));
    }

    Describe("turn");
    return Stay();
}
