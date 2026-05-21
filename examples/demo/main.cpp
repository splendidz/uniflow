// ============================================================================
//  main.cpp — wiring example
//
//  Build (from the repository root):
//    g++ -std=c++17 -O2 -pthread -I include \
//        examples/demo/main.cpp examples/demo/order_module.cpp \
//        examples/demo/session_module.cpp -o build/demo
//
//  Add -DUNIFLOW_USE_BS_THREAD_POOL to use the vendored BS::thread_pool
//  instead of the bundled StdThreadPool.  See README.md for MSVC / VS2022.
// ============================================================================
#include "uniflow.hpp"

#include "order_module.h"
#include "session_module.h"

#include <chrono>
#include <iostream>
#include <thread>

int main()
{
    using namespace std::chrono_literals;

    uniflow::UniflowRuntime::Config cfg;
    cfg.slow_cpu_threshold = std::chrono::microseconds(500);
    cfg.idle_sleep         = 1ms;

    uniflow::UniflowRuntime rt(cfg);

#if defined(UNIFLOW_USE_BS_THREAD_POOL)
    rt.RegisterExecutor("default", std::make_shared<uniflow::BSThreadPoolExecutor>(4));
    rt.RegisterExecutor("payments", std::make_shared<uniflow::BSThreadPoolExecutor>(2));
    rt.RegisterExecutor("net", std::make_shared<uniflow::BSThreadPoolExecutor>(4));
#else
    rt.RegisterExecutor("default", std::make_shared<uniflow::StdThreadPool>(4));
    rt.RegisterExecutor("payments", std::make_shared<uniflow::StdThreadPool>(2));
    rt.RegisterExecutor("net", std::make_shared<uniflow::StdThreadPool>(4));
#endif

    auto* order   = rt.Create<OrderModule>("order");
    auto* session = rt.Create<SessionModule>("session");

    // The runtime owns its pump thread: RunInBackground() starts it, and the
    // runtime's destructor stops and joins it — no manual std::thread needed.
    rt.RunInBackground();

    // Block until a module finishes its flow (or a 5s safety budget runs out).
    // Takes IUniflowObject* — the interface every module shares.
    auto wait_idle = [](uniflow::IUniflowObject* obj)
    {
        auto t0 = std::chrono::steady_clock::now();
        while (!obj->IsIdle())
        {
            std::this_thread::sleep_for(5ms);
            if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(5))
            {
                std::cerr << "wait_idle timeout\n";
                return;
            }
        }
    };

    std::cout << "\n=== scenario 1: order checkout ===\n";
    order->RequestCheckout(3, 25.0, 0.0);
    wait_idle(order);

    std::cout << "\n=== scenario 2: zero-total order (should skip payment) ===\n";
    order->RequestCheckout(2, 10.0, 100.0);
    wait_idle(order);

    std::cout << "\n=== scenario 3: empty cart (should Fail at Validate) ===\n";
    order->RequestCheckout(0, 10.0, 0.0);
    wait_idle(order);

    std::cout << "\n=== scenario 4: session open + close ===\n";
    session->RequestOpen("example.com");
    wait_idle(session);
    session->RequestClose();
    wait_idle(session);

    std::cout << "\n=== scenario 5: close with no open session (should abort) ===\n";
    session->RequestClose();
    wait_idle(session);

    std::cout << "\n=== scenario 6: order + session concurrently ===\n";
    order->RequestCheckout(5, 12.0, 0.0);
    session->RequestOpen("uniflow.dev");
    wait_idle(order);
    wait_idle(session);

    std::cout << "\n[done]\n";
    return 0;
}
