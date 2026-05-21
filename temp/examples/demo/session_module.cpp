#include "session_module.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <utility>

using uniflow::AsyncOpts;
using uniflow::AsyncRef;

namespace
{
// Per-call options shared by every network step in this module.
uniflow::AsyncOpts net_opts()
{
    uniflow::AsyncOpts opts;
    opts.timeout         = std::chrono::seconds(2);
    opts.slow_warn_after = std::chrono::milliseconds(300);
    opts.executor_name   = "net";
    return opts;
}
} // namespace

SessionModule::SessionModule(uniflow::UniflowRuntime& rt)
    : Uniflow(rt)
{
    UF_ENTRY("Open", OnOpen_Begin);
    UF_ENTRY("Close", OnClose_Begin);
}

bool SessionModule::RequestOpen(std::string host)
{
    if (!IsIdle())
        return false;
    host_ = std::move(host);
    return Start("Open");
}

bool SessionModule::RequestClose()
{
    if (!IsIdle())
        return false;
    return Start("Close");
}

// ─── Open flow ────────────────────────────────────────────────────────────────
SessionModule::StepResult SessionModule::OnOpen_Begin()
{
    std::cout << "  [session] OPEN " << host_ << "\n";
    if (open_)
    {
        std::cout << "  [session] already open - abort\n";
        return Fail();
    }
    return UF_NEXT(OnOpen_Resolve);
}

SessionModule::StepResult SessionModule::OnOpen_Resolve()
{
    UF_ASYNC_OPT(DoResolve, net_opts(), host_);
    return UF_NEXT(OnOpen_ResolveDone);
}
SessionModule::StepResult SessionModule::OnOpen_ResolveDone()
{
    auto r = AsyncResult<ResolveResult>();
    if (r.failed() || !r.value().ok)
        return Fail();
    address_ = r.value().address;
    return UF_NEXT(OnOpen_Connect);
}

SessionModule::StepResult SessionModule::OnOpen_Connect()
{
    UF_ASYNC_OPT(DoConnect, net_opts(), address_);
    return UF_NEXT(OnOpen_ConnectDone);
}
SessionModule::StepResult SessionModule::OnOpen_ConnectDone()
{
    auto r = AsyncResult<ConnectResult>();
    if (r.failed() || !r.value().ok)
        return Fail();
    socket_id_ = r.value().socket_id;
    return UF_NEXT(OnOpen_Authenticate);
}

SessionModule::StepResult SessionModule::OnOpen_Authenticate()
{
    UF_ASYNC_OPT(DoAuthenticate, net_opts(), socket_id_);
    return UF_NEXT(OnOpen_AuthenticateDone);
}
SessionModule::StepResult SessionModule::OnOpen_AuthenticateDone()
{
    auto r = AsyncResult<bool>();
    if (r.failed() || !r.value())
        return Fail();
    open_ = true;
    std::cout << "  [session] open (socket " << socket_id_ << ")\n";
    return Done();
}

// ─── Close flow ───────────────────────────────────────────────────────────────
SessionModule::StepResult SessionModule::OnClose_Begin()
{
    std::cout << "  [session] CLOSE\n";
    if (!open_)
    {
        std::cout << "  [session] nothing open - abort\n";
        return Fail();
    }
    return UF_NEXT(OnClose_Drain);
}

SessionModule::StepResult SessionModule::OnClose_Drain()
{
    UF_ASYNC_OPT(DoDrain, net_opts(), socket_id_);
    return UF_NEXT(OnClose_DrainDone);
}
SessionModule::StepResult SessionModule::OnClose_DrainDone()
{
    auto r = AsyncResult<int>();
    if (r.failed())
        return Fail();
    std::cout << "  [session] drained " << r.value() << " pending message(s)\n";
    return UF_NEXT(OnClose_Disconnect);
}

SessionModule::StepResult SessionModule::OnClose_Disconnect()
{
    UF_ASYNC_OPT(DoDisconnect, net_opts(), socket_id_);
    return UF_NEXT(OnClose_DisconnectDone);
}
SessionModule::StepResult SessionModule::OnClose_DisconnectDone()
{
    auto r = AsyncResult<bool>();
    if (r.failed() || !r.value())
        return Fail();
    open_ = false;
    std::cout << "  [session] closed\n";
    return Done();
}

// ─── Static async workers (run on the pool — no `this`) ───────────────────────
SessionModule::ResolveResult SessionModule::DoResolve(std::string host)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    ResolveResult r;
    r.ok      = true;
    r.address = static_cast<long>(host.size() * 1000 + 42);
    return r;
}

SessionModule::ConnectResult SessionModule::DoConnect(long address)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    ConnectResult r;
    r.ok        = true;
    r.socket_id = static_cast<int>(address % 1000);
    return r;
}

bool SessionModule::DoAuthenticate(int socket_id)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    (void)socket_id;
    return true;
}

int SessionModule::DoDrain(int socket_id)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    (void)socket_id;
    return 3;
}

bool SessionModule::DoDisconnect(int socket_id)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    (void)socket_id;
    return true;
}
