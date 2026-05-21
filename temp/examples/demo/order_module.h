// ============================================================================
//  order_module.h — order-checkout example
//
//  Flow: Checkout
//        Begin → Validate → Price → Charge(async) → ChargeDone → Confirm → Done
//
//  Demonstrates: an arg-only static async worker, AsyncResult<T>() consumption,
//                an early-exit jump (a zero-total order skips the payment
//                step), and a validation failure that aborts the flow.
// ============================================================================
#pragma once

#include "uniflow.hpp"

class OrderModule : public uniflow::Uniflow<OrderModule>
{
    using S = OrderModule;
    UF_USES_UNIFLOW(OrderModule);

public:
    explicit OrderModule(uniflow::UniflowRuntime& rt);

    // External entry: queue a checkout. Returns false if one is already running.
    bool RequestCheckout(int item_count, double unit_price, double discount);

private:
    // ── Step functions (named On<Flow>_<Purpose>) ──
    StepResult OnCheckout_Begin();
    StepResult OnCheckout_Validate();
    StepResult OnCheckout_Price();
    StepResult OnCheckout_Charge();     // submits async DoChargeCard
    StepResult OnCheckout_ChargeDone(); // receives ChargeResult
    StepResult OnCheckout_Confirm();

    // ── Async work — static, no `this`. All inputs arrive via args. ──
    struct ChargeResult
    {
        bool   ok      = false;
        double amount  = 0.0;
        double took_ms = 0.0;
    };
    static ChargeResult DoChargeCard(double amount);

    // ── Domain state (only touched on the main thread) ──
    int    item_count_ = 0;
    double unit_price_ = 0.0;
    double discount_   = 0.0;
    double total_      = 0.0;
    double charged_    = 0.0;
};
