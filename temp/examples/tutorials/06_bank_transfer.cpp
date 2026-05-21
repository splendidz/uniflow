// ============================================================================
//  06_bank_transfer.cpp — money transfers between shared accounts, lock-free
//
//  Transferring money is the classic two-lock problem: a multi-threaded
//  transfer must lock BOTH the source and the destination account, in a
//  globally fixed order — otherwise two transfers in opposite directions
//  deadlock. Get the debit and credit only half-applied and money is lost.
//
//  Here the debit and the credit happen in ONE step. The runtime never
//  interleaves steps, so no other transfer can observe a half-done one and
//  no update is ever lost — with no account lock, no lock ordering, and no
//  deadlock. The grand total stays conserved no matter how the agents
//  interleave.
//
//  Build (from repo root):
//    cl /std:c++17 /EHsc /I include ^
//       examples\tutorials\06_bank_transfer.cpp /Fe:build\06_bank_transfer.exe
//    g++ -std=c++17 -pthread -I include ^
//       examples/tutorials/06_bank_transfer.cpp -o build/06_bank_transfer
// ============================================================================
#include "uniflow.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// Shared accounts. Multi-threaded, each account would need a mutex; here the
// vector is plain — see TransferAgent::OnTransfer.
struct Bank
{
    std::vector<long> balance;
};

struct Transfer
{
    int  from;
    int  to;
    long amount;
};

class TransferAgent : public uniflow::Uniflow<TransferAgent>
{
    using S = TransferAgent;
    UF_USES_UNIFLOW(TransferAgent);

public:
    TransferAgent(uniflow::UniflowRuntime& rt, Bank& bank, std::vector<Transfer> work)
        : Uniflow(rt), bank_(bank), work_(std::move(work))
    {
        UF_ENTRY("Run", OnTransfer);
    }

private:
    StepResult OnTransfer()
    {
        if (next_ >= work_.size())
            return Done();

        const Transfer t = work_[next_++];
        // Debit and credit in ONE step. No other step — no other agent — can
        // run in between, so no half-applied transfer is ever visible and no
        // update is lost. No two-account lock, no ordering, no deadlock.
        bank_.balance[t.from] -= t.amount;
        bank_.balance[t.to] += t.amount;
        std::cout << "  [" << InstanceName() << "] moved " << t.amount
                  << " : acct " << t.from << " -> acct " << t.to << "\n";
        return Stay(); // loop to the next transfer in the list
    }

    Bank&                 bank_;
    std::vector<Transfer> work_;
    std::size_t           next_ = 0;
};

int main()
{
    Bank bank;
    bank.balance = {1000, 1000, 1000, 1000}; // four accounts

    long total_before = 0;
    for (long b : bank.balance)
        total_before += b;

    uniflow::UniflowRuntime rt;
    auto*                   agentA = rt.Create<TransferAgent>(
        "agent-A", bank, std::vector<Transfer>{{0, 1, 100}, {1, 2, 50}, {2, 3, 30}});
    auto* agentB = rt.Create<TransferAgent>(
        "agent-B", bank, std::vector<Transfer>{{3, 0, 70}, {2, 1, 40}, {0, 3, 25}});

    rt.RunInBackground();

    agentA->Start("Run");
    agentB->Start("Run");
    while (!agentA->IsIdle() || !agentB->IsIdle())
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

    long total_after = 0;
    std::cout << "\nfinal balances:";
    for (long b : bank.balance)
    {
        std::cout << " " << b;
        total_after += b;
    }
    std::cout << "\ntotal before = " << total_before
              << ", total after = " << total_after
              << (total_before == total_after ? "  (conserved)" : "  (LOST!)") << "\n";
    return 0;
}
