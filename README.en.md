# uniflow-cpp

> 🌐 Language: **English** · [한국어](README.md)

> A tiny step-based async framework that runs everything cooperatively on one thread. One header (`uniflow.hpp`) and you're done.

C++17, tested on MSVC v143 and GCC 10+. No dependencies outside the C library. [BS::thread_pool](https://github.com/bshoshany/thread-pool) is bundled inside the header, so all you need is a compiler.

---

## 30-second pitch

Device automation, message dispatch, async I/O — when you have things that "must happen in order", you usually end up in one of three traps:

- Callback hell (`.then().then().then()`)
- A state machine (switch + enum + 30 member variables)
- Holding a lock across shared state and deadlocking yourself

This framework dodges all three. A module's flow is written as a **chain of step functions**. The runtime pumps every module round-robin on one thread. Blocking work is shoved onto a pool with one line (`UF_ASYNC`), and the result is picked up in the *next* step.

```cpp
class OrderRouter : public uniflow::Uniflow<OrderRouter> {
    UF_UNIFLOW_IMPLEMENT(OrderRouter);
public:
    explicit OrderRouter(uniflow::Runtime& rt)
        : uniflow::Uniflow<OrderRouter>(rt) {}

    StepResult OnRoute_Begin(Order o) {
        order_ = std::move(o);
        return UF_NEXT(OnRoute_Validate);
    }

private:
    StepResult OnRoute_Validate() {
        if (!order_.valid()) return Fail();
        UF_ASYNC(HitPaymentApi, order_.id);    // blocking call -> pool
        return UF_NEXT(OnRoute_AfterPayment);
    }

    StepResult OnRoute_AfterPayment() {
        auto r = AsyncResult<PaymentResult>();
        if (r.failed()) return Fail();
        std::cout << "paid: " << r.value().txid << "\n";
        return Done();
    }

    static PaymentResult HitPaymentApi(int id); // runs on a pool thread

    Order order_;
};

int main() {
    uniflow::Runtime rt;            // owns the pump thread
    OrderRouter      router{rt};
    UF_START_FLOW(router, OnRoute_Begin, Order{42});
    router.WaitUntilIdle();
}
```

The pump thread never blocks, and every module on that pump shares the same thread — so shared state needs no locks.

---

## Why does this exist

Three assumptions:

1. **Blocking I/O is real.** Motor motion, HTTP, DB, LLM calls. If you scatter that across callbacks or `co_await`, the flow becomes invisible. Step chains keep the *flow shaped like the code*.
2. **Shared-resource bugs are the most common kind.** If every step runs serially on one thread, you don't need locks — two modules can poke the same `ostringstream` and never race. ([Example 1](examples/shared_ostream/) literally demonstrates this.)
3. **When something goes wrong, the first question is "which step died?"** So every step transition is funnelled through an observer, and the default observer prints them nicely.

The longer rationale lives in [DESIGN.md](DESIGN.md) (Korean).

---

## Where to start

| Path | For who |
|---|---|
| [TUTORIAL.en.md](TUTORIAL.en.md) | "I want to grow into it one chapter at a time" — from a 1-step module up to async |
| [EXAMPLES.en.md](EXAMPLES.en.md) | "Show me real code first" — 5 working projects (CNC, message dispatch, weather + LLM, ...) |
| [DESIGN.md](DESIGN.md) | "I want to understand the why" — concepts, trade-offs, implementation notes (Korean) |
| [uniflow.hpp](uniflow.hpp) | "Just let me read the header" — ~1300 lines, well-commented |

---

## Building

Single header — there's nothing to wire into your build system. Just put this folder on your include path.

**Visual Studio**: every `examples/*/*.vcxproj` works as-is. The only required setting is `AdditionalIncludeDirectories=..\..\`.

**MSVC from a shell**:
```powershell
cl /std:c++17 /EHsc /I . examples\shared_ostream\*.cpp /Fe:shared_ostream.exe
```

**g++ / clang++**:
```bash
g++ -std=c++17 -O2 -pthread -I . examples/shared_ostream/*.cpp -o shared_ostream
```

---

## Five words you need

- **`Runtime`** — owns one pump thread and one thread pool. Modules attach to it.
- **`Uniflow<Derived>`** — CRTP base for a module. Step functions are members on it.
- **Step** — a member function that returns `StepResult`. It only returns an *intent* (Next/Stay/Done/Fail) and lets the next round happen.
- **`UF_ASYNC(fn, args...)`** — macro that shoves blocking work onto the pool. The result is read with `AsyncResult<T>()` in the next step.
- **Observer** — the hook every step transition, async completion, and exception goes through. Default writes to the console; swap it for a file logger, Slack, anything.

---

## Demo videos

![demo](docs/videos/uniflow_overview.gif)

> 📹 Short demo clips live under `docs/videos/`. Placeholders ready.

---

## License

[MIT](LICENSE). The bundled BS::thread_pool is also MIT.
