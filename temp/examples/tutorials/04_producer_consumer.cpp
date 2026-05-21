// ============================================================================
//  04_producer_consumer.cpp — the classic bounded-buffer problem, lock-free
//
//  The textbook producer/consumer needs a mutex plus two condition variables
//  (buffer-not-full, buffer-not-empty) to guard the shared queue. Here the
//  producer and the consumer are two modules on ONE cooperative thread: only
//  one step runs at a time, so the shared queue needs NO lock at all. A step
//  that cannot proceed (queue full, or empty) just returns Stay() and yields.
//
//  Build (from repo root):
//    cl /std:c++17 /EHsc /I include ^
//       examples\tutorials\04_producer_consumer.cpp /Fe:build\04_producer_consumer.exe
//    g++ -std=c++17 -pthread -I include ^
//       examples/tutorials/04_producer_consumer.cpp -o build/04_producer_consumer
// ============================================================================
#include "uniflow.hpp"

#include <chrono>
#include <deque>
#include <iostream>
#include <thread>

// The shared resource. In a multi-threaded design every touch of this would
// be under a mutex; here it is just a plain struct — see main() for why.
struct Buffer
{
    std::deque<int> queue;
    std::size_t     capacity = 3;
};

// ── Producer: puts items 1..N into the buffer, one per step ──────────────────
class Producer : public uniflow::Uniflow<Producer>
{
    using S = Producer;
    UF_USES_UNIFLOW(Producer);

public:
    Producer(uniflow::UniflowRuntime& rt, Buffer& buf, int count)
        : Uniflow(rt), buf_(buf), count_(count)
    {
        UF_ENTRY("Produce", OnProduce);
    }

private:
    StepResult OnProduce()
    {
        if (produced_ >= count_)
            return Done(); // produced everything

        if (buf_.queue.size() >= buf_.capacity)
        {
            std::cout << "  [producer] buffer full - waiting\n";
            return Stay(); // cooperative wait — no condition variable
        }

        int item = ++produced_;
        buf_.queue.push_back(item);
        std::cout << "  [producer] put " << item << "   (queue " << buf_.queue.size() << ")\n";
        return Stay(); // loop: run this step again for the next item
    }

    Buffer& buf_;
    int     count_;
    int     produced_ = 0;
};

// ── Consumer: takes N items out, then "processes" each (a 2-step cycle, so
//    the consumer runs at half the producer's rate and the buffer fills up) ──
class Consumer : public uniflow::Uniflow<Consumer>
{
    using S = Consumer;
    UF_USES_UNIFLOW(Consumer);

public:
    Consumer(uniflow::UniflowRuntime& rt, Buffer& buf, int count)
        : Uniflow(rt), buf_(buf), count_(count)
    {
        UF_ENTRY("Consume", OnConsume);
    }

private:
    StepResult OnConsume()
    {
        if (consumed_ >= count_)
            return Done();

        if (buf_.queue.empty())
        {
            std::cout << "  [consumer] buffer empty - waiting\n";
            return Stay();
        }

        last_ = buf_.queue.front();
        buf_.queue.pop_front();
        ++consumed_;
        std::cout << "  [consumer] got " << last_ << "   (queue " << buf_.queue.size() << ")\n";
        return UF_NEXT(OnProcess);
    }

    StepResult OnProcess()
    {
        std::cout << "  [consumer] processed " << last_ << "\n";
        return UF_NEXT(OnConsume); // loop back for the next item
    }

    Buffer& buf_;
    int     count_;
    int     consumed_ = 0;
    int     last_     = 0;
};

int main()
{
    constexpr int kItems = 8;

    Buffer buffer; // the shared queue — no mutex, no condition variable

    uniflow::UniflowRuntime rt;
    auto*                   producer = rt.Create<Producer>("producer", buffer, kItems);
    auto*                   consumer = rt.Create<Consumer>("consumer", buffer, kItems);

    // Why no lock is needed: the runtime runs exactly one step at a time on
    // one thread, so the producer's push_back and the consumer's pop_front
    // can never overlap. Each step is, in effect, already a critical section.
    rt.RunInBackground();

    producer->Start("Produce");
    consumer->Start("Consume");

    while (!producer->IsIdle() || !consumer->IsIdle())
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

    std::cout << "\nall " << kItems << " items produced and consumed\n";
    return 0;
}
