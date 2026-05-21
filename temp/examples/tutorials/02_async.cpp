// ============================================================================
//  02_async.cpp — offloading a blocking call without stalling the main thread
//
//  Teaches: UF_ASYNC_OPT (hand a static function to a thread pool),
//           AsyncResult<T>() (collect its result in the continuation step),
//           and why the worker function must be `static` (no `this`).
//
//  The framework's core promise: the main pump thread is NEVER blocked.
//  A blocking call (file IO, a network request, ...) goes to the pool; the
//  step that submitted it names a continuation step, which the runtime
//  invokes once the result is ready.
//
//  Build (from repo root):
//    cl /std:c++17 /EHsc /I include ^
//       examples\tutorials\02_async.cpp /Fe:build\02_async.exe
//    g++ -std=c++17 -pthread -I include ^
//       examples/tutorials/02_async.cpp -o build/02_async
// ============================================================================
#include "uniflow.hpp"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>

class FileLoader : public uniflow::Uniflow<FileLoader>
{
    using S = FileLoader;
    UF_USES_UNIFLOW(FileLoader);

public:
    explicit FileLoader(uniflow::UniflowRuntime& rt)
        : Uniflow(rt)
    {
        UF_ENTRY("Load", OnLoad_Begin);
    }

    void RequestLoad(std::string path)
    {
        path_ = std::move(path);
        Start("Load");
    }

private:
    // The value the worker produces. A plain type — it is moved out of the
    // pool and handed to the continuation step.
    struct Blob
    {
        std::size_t bytes = 0;
    };

    StepResult OnLoad_Begin()
    {
        std::cout << "  loading '" << path_ << "'\n";
        return UF_NEXT(OnLoad_Read);
    }

    StepResult OnLoad_Read()
    {
        // Per-call options live right next to the call: where to run it,
        // when to warn that it is slow, when to give up.
        uniflow::AsyncOpts opts;
        opts.timeout         = std::chrono::seconds(2);
        opts.slow_warn_after = std::chrono::milliseconds(100);

        // Hand the blocking work to the pool. This step itself costs almost
        // no main-thread CPU — it just queues the job and names what runs
        // next. OnLoad_Done will not run until ReadFromDisk has finished.
        UF_ASYNC_OPT(ReadFromDisk, opts, path_);
        return UF_NEXT(OnLoad_Done);
    }

    StepResult OnLoad_Done()
    {
        // Collect the typed result of the most recent async submission.
        auto r = AsyncResult<Blob>();
        if (r.is_timeout())
        {
            std::cout << "  read timed out\n";
            return Fail();
        }
        if (r.failed())
        {
            std::cout << "  read threw\n";
            return Fail();
        }
        std::cout << "  read " << r.value().bytes << " bytes\n";
        return Done();
    }

    // ── Worker: runs on the pool, NOT on the main thread. ──
    // It is `static` on purpose: with no `this`, it cannot touch member
    // state and race the main thread. Everything it needs arrives by value
    // through the arguments. UF_ASYNC enforces this with a static_assert.
    static Blob ReadFromDisk(std::string path)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(150)); // fake IO
        return Blob{path.size() * 1024};
    }

    std::string path_;
};

int main()
{
    uniflow::UniflowRuntime rt;

    // Async work needs an executor (thread pool). UF_ASYNC submits to the
    // one registered under the name "default"; UF_ASYNC_OPT can pick another
    // via AsyncOpts::executor_name.
    rt.RegisterExecutor("default", std::make_shared<uniflow::StdThreadPool>(2));

    auto* loader = rt.Create<FileLoader>("loader");

    // The runtime owns its pump thread (stopped and joined by its destructor).
    rt.RunInBackground();

    loader->RequestLoad("config.json");
    while (!loader->IsIdle())
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

    return 0;
}
