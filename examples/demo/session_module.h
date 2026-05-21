// ============================================================================
//  session_module.h — connection-session example
//
//  Two flows on one module:
//    Open  — Begin → Resolve(async) → Connect(async) → Authenticate(async) → Done
//    Close — Begin → Drain(async) → Disconnect(async) → Done
//
//  Demonstrates: multiple flows in one module, several distinct async worker
//                signatures, and shared state that gates one flow against the
//                other (you cannot open twice, or close what is not open).
// ============================================================================
#pragma once

#include "uniflow.hpp"

#include <string>

class SessionModule : public uniflow::Uniflow<SessionModule>
{
    using S = SessionModule;
    UF_USES_UNIFLOW(SessionModule);

public:
    explicit SessionModule(uniflow::UniflowRuntime& rt);

    bool RequestOpen(std::string host);
    bool RequestClose();

    bool is_open() const { return open_; }

private:
    // ── Open flow ──
    StepResult OnOpen_Begin();
    StepResult OnOpen_Resolve();
    StepResult OnOpen_ResolveDone();
    StepResult OnOpen_Connect();
    StepResult OnOpen_ConnectDone();
    StepResult OnOpen_Authenticate();
    StepResult OnOpen_AuthenticateDone();

    // ── Close flow ──
    StepResult OnClose_Begin();
    StepResult OnClose_Drain();
    StepResult OnClose_DrainDone();
    StepResult OnClose_Disconnect();
    StepResult OnClose_DisconnectDone();

    // ── Async workers (static, no `this`) ──
    struct ResolveResult
    {
        bool ok      = false;
        long address = 0;
    };
    struct ConnectResult
    {
        bool ok        = false;
        int  socket_id = 0;
    };

    static ResolveResult DoResolve(std::string host);
    static ConnectResult DoConnect(long address);
    static bool          DoAuthenticate(int socket_id);
    static int           DoDrain(int socket_id);
    static bool          DoDisconnect(int socket_id);

    // ── Domain state ──
    std::string host_;
    long        address_   = 0;
    int         socket_id_ = 0;
    bool        open_      = false;
};
