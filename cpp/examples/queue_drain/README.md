# queue_drain

> Language: [한국어](README.kr.md) | **English**

The **producer / consumer** pattern on uniflow. A `Flow_Sender` pushes
random arithmetic jobs into a shared mailbox in bursts; a `Flow_Receiver` drains
the mailbox one job at a time, dispatching by operator. Both run on **one pump
thread**, so the mailbox between them needs **no lock**.

```
  uniflow queue_drain  v1.0.0
  ------------------------------------------------------------
  sender   bursts 7/20   last burst 5   idle gap
  vec A: 10  7 24 11 27  6 30  9 30  4
  vec B: 18 14 13 14 24 14  4 18  9  2
  ------------------------------------------------------------
  queue    depth 3
  [12 + 4] [27 - 9] [6 + 30]
  ------------------------------------------------------------
  receiver Adding   processed 41   add: 12 + 4 = 16
  last result: 12 + 4 = 16
  ------------------------------------------------------------
  press Enter to quit
```

## Feature focus

This example is about a **lock-free producer / consumer on one pump thread** and
the **park / relaunch** wake pattern.

- **One thread, no lock on the queue.** The sender and the receiver are two
  modules on the SAME pump, so they advance round-robin and never touch the
  mailbox at the same instant. `Mailbox::Push` / `TryPop` are plain `std::deque`
  calls with no mutex (see [mailbox.h](mailbox.h) / [mailbox.cpp](mailbox.cpp)).

- **Queue drain loop.** The receiver's single task loops
  `Step1_TakeNext -> Step2_Add` or `Step3_Sub -> Step1_TakeNext`. When `TryPop`
  finds the queue empty it returns `Done()`, parking the module
  (see [uf_receiver.cpp](uf_receiver.cpp)).

- **Park / relaunch wake.** After each burst the sender checks
  `receiver.IsIdle()` and, if the receiver parked, relaunches its drain task with
  `recv.ctx_drain_.StartFlow()`. Both calls are in-thread, so there is no
  cross-thread signal (see `Step1_Tick` in [uf_sender.cpp](uf_sender.cpp)).

- **Console / ANSI render pattern.** [console.h](console.h) /
  [console.cpp](console.cpp) is a dependency-free ANSI helper (byte-identical to
  the simulator's). A pump-side snapshot step copies state under one mutex; a
  background thread redraws ~25 fps while `main` blocks on stdin (one Enter
  quits).

## Code map

| File | Role |
| --- | --- |
| [uf_sender.h](uf_sender.h) / [.cpp](uf_sender.cpp) | Producer. One perpetual task: every `kSendGap`, push a 1..10 job burst, then relaunch the receiver if parked. |
| [uf_receiver.h](uf_receiver.h) / [.cpp](uf_receiver.cpp) | Consumer. Drain loop: pop one job, dispatch Add/Sub, repeat; `Done()` when empty. |
| [mailbox.h](mailbox.h) / [.cpp](mailbox.cpp) | The lock-free FIFO (reason in the header note). |
| [uf_visualization.h](uf_visualization.h) / [.cpp](uf_visualization.cpp) | Pump-side snapshot step + main-thread ANSI dashboard. |
| [snapshot.h](snapshot.h) / [.cpp](snapshot.cpp) | Pump -> render-thread hand-off (the demo's only mutex). |
| [console.h](console.h) / [.cpp](console.cpp) | Reusable ANSI console helper. |
| [globals.h](globals.h) / [.cpp](globals.cpp) | `Msg` value type, tuning constants, stop latch. |
| [app.h](app.h) | Runtime + every module, two-phase init, silent observer. |
| [main.cpp](main.cpp) | Start, run the renderer, shutdown, print a summary line. |

## Build / run

Console only, nothing extra to install.

**Linux / macOS (g++ or clang++):**

```sh
cd cpp/examples/queue_drain
g++ -std=c++17 -O2 -I../.. *.cpp -o queue_drain -pthread
./queue_drain
```

**Windows (MSVC, x64 Native Tools prompt):**

```bat
cd cpp\examples\queue_drain
cl /std:c++17 /EHsc /O2 /I..\.. *.cpp /Fe:queue_drain.exe
queue_drain.exe
```

> Note: this demo assumes an ANSI terminal. On Windows, `console::EnableAnsi()`
> turns on the console's VT processing (Windows Terminal recommended).
