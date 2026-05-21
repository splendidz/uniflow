#include "order_module.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

using uniflow::AsyncOpts;
using uniflow::AsyncRef;

OrderModule::OrderModule(uniflow::UniflowRuntime& rt)
    : Uniflow(rt)
{
    UF_ENTRY("Checkout", OnCheckout_Begin);
}

bool OrderModule::RequestCheckout(int item_count, double unit_price, double discount)
{
    if (!IsIdle())
        return false;
    item_count_ = item_count;
    unit_price_ = unit_price;
    discount_   = discount;
    return Start("Checkout");
}

// ── Steps ────────────────────────────────────────────────────────────────────
OrderModule::StepResult OrderModule::OnCheckout_Begin()
{
    // Reset per-order state — this module is reused for every checkout.
    total_   = 0.0;
    charged_ = 0.0;
    std::cout << "  [order] checkout: " << item_count_ << " item(s) @ "
              << unit_price_ << ", discount " << discount_ << "\n";
    return UF_NEXT(OnCheckout_Validate);
}

OrderModule::StepResult OrderModule::OnCheckout_Validate()
{
    if (item_count_ <= 0)
    {
        std::cout << "  [order] empty cart - rejected\n";
        return Fail();
    }
    return UF_NEXT(OnCheckout_Price);
}

OrderModule::StepResult OrderModule::OnCheckout_Price()
{
    total_ = item_count_ * unit_price_ - discount_;
    if (total_ < 0.0)
        total_ = 0.0;
    // Early-exit jump: a fully-discounted order needs no payment step.
    if (total_ <= 1e-9)
    {
        std::cout << "  [order] total is 0 - skipping payment\n";
        return UF_NEXT(OnCheckout_Confirm);
    }
    return UF_NEXT(OnCheckout_Charge);
}

OrderModule::StepResult OrderModule::OnCheckout_Charge()
{
    // Per-call opts: warn if the gateway is slow, hard timeout 3s, run on the
    // dedicated "payments" executor (created in main.cpp).
    AsyncOpts opts;
    opts.timeout         = std::chrono::seconds(3);
    opts.slow_warn_after = std::chrono::milliseconds(200);
    opts.executor_name   = "payments";

    UF_ASYNC_OPT(DoChargeCard, opts, total_);
    return UF_NEXT(OnCheckout_ChargeDone);
}

OrderModule::StepResult OrderModule::OnCheckout_ChargeDone()
{
    auto r = AsyncResult<ChargeResult>();
    if (r.is_timeout())
    {
        std::cout << "  [order] payment TIMEOUT - flow aborted\n";
        return Fail();
    }
    if (r.failed())
    {
        std::cout << "  [order] payment threw - flow aborted\n";
        return Fail();
    }
    if (!r.value().ok)
    {
        std::cout << "  [order] payment declined\n";
        return Fail();
    }
    charged_ = r.value().amount;
    std::cout << "  [order] charged " << charged_ << " in "
              << r.value().took_ms << "ms\n";
    return UF_NEXT(OnCheckout_Confirm);
}

OrderModule::StepResult OrderModule::OnCheckout_Confirm()
{
    std::cout << "  [order] confirmed (charged " << charged_ << ")\n";
    return Done();
}

// ── Async worker (static — no `this`) ─────────────────────────────────────────
OrderModule::ChargeResult OrderModule::DoChargeCard(double amount)
{
    // Pretend to call a payment gateway over the network. In a real system
    // this would be an HTTPS request; here we just sleep to simulate latency.
    const auto sleep_ms = std::chrono::milliseconds(
        static_cast<int>(std::min(500.0, 20.0 + amount * 0.5)));

    auto t0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(sleep_ms);
    auto t1 = std::chrono::steady_clock::now();

    ChargeResult r;
    r.ok      = true;
    r.amount  = amount;
    r.took_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    return r;
}
