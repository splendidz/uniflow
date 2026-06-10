// app.h - owns the Runtime and every module: one traffic-light module per
// junction, a fixed fleet of vehicles, and the visualisation module. All share
// the one pump thread, so the World (signals + vehicle positions) is touched
// without locks.
#pragma once

#include "globals.h"
#include "map.h"
#include "uf_traffic_light.h"
#include "uf_vehicle.h"
#include "uf_visualization.h"
#include "uniflow.hpp"
#include "world.h"

#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

class App
{
public:
    static App& inst()
    {
        static App a;
        return a;
    }

    uniflow::Runtime rt{MakeOpts()};
    UF_Visualization viz{rt};
    std::vector<std::unique_ptr<UF_TrafficLight>> lights;
    std::vector<std::unique_ptr<UF_Vehicle>>      vehicles;

    void Start()
    {
        UF_START_FLOW(viz, OnViz_Begin);
        for (auto& l : lights)
            UF_START_FLOW(*l, OnLight_Begin);
        for (auto& v : vehicles)
            UF_START_FLOW(*v, OnSeg_Cruise);
    }

    void Shutdown()
    {
        sim::RequestStop();
        for (auto& v : vehicles)
            v->WaitUntilIdle();
        for (auto& l : lights)
            l->WaitUntilIdle();
        viz.WaitUntilIdle();
    }

private:
    App()
    {
        for (const citymap::Node& n : citymap::Nodes())
            if (n.kind != citymap::NodeKind::Corner)
                lights.push_back(std::make_unique<UF_TrafficLight>(rt, n.id));

        BuildFleet();
    }

    void BuildFleet()
    {
        const int   n     = 15;
        const auto& edges = citymap::Edges();
        const int   E     = static_cast<int>(edges.size());

        city::InitVehicles(n);
        for (int i = 0; i < n; ++i)
        {
            const citymap::Edge& e    = edges[i % E];
            int                  slot = i / E;           // 0 or 1 cars per edge
            double               dist0 = 0.22 + 0.45 * slot;

            uint8_t r, g, b;
            Hsv(std::fmod(i * 137.5, 360.0), 0.62, 0.96, r, g, b); // golden angle
            vehicles.push_back(std::make_unique<UF_Vehicle>(
                rt, i, e.a, e.b, dist0, r, g, b));
        }
    }

    // HSV -> RGB for spreading distinct car colours across the fleet.
    static void Hsv(double h, double s, double v,
                    uint8_t& r, uint8_t& g, uint8_t& b)
    {
        double c = v * s;
        double x = c * (1.0 - std::fabs(std::fmod(h / 60.0, 2.0) - 1.0));
        double m = v - c;
        double rr = 0, gg = 0, bb = 0;
        if (h < 60)       { rr = c; gg = x; }
        else if (h < 120) { rr = x; gg = c; }
        else if (h < 180) { gg = c; bb = x; }
        else if (h < 240) { gg = x; bb = c; }
        else if (h < 300) { rr = x; bb = c; }
        else              { rr = c; bb = x; }
        r = static_cast<uint8_t>((rr + m) * 255.0);
        g = static_cast<uint8_t>((gg + m) * 255.0);
        b = static_cast<uint8_t>((bb + m) * 255.0);
    }

    static uniflow::Runtime::Opts MakeOpts()
    {
        uniflow::Runtime::Opts opts;
        opts.threads = 2; // demo does no async work; the pump does it all
        // Leave the observer at its default (ConsoleObserver): every step
        // transition (signal phase changes, each car's Cruise/Wait/Cross/Turn)
        // streams to the console so it is obvious the simulation is running.
        return opts;
    }
};
