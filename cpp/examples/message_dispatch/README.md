# message_dispatch

> Language: [한국어](README.kr.md) | **English**

A console example: one **student** worker, two **spawners** (a professor and a
friend), and a shared **mailbox**. The professor posts *assignments*, the friend
posts *play invites*, and the student drains the mailbox one message at a time,
routing each by its kind. All four are uniflow modules on the **same Runtime**,
so the mailbox is touched on one pump thread and needs no lock.

```
  uniflow message_dispatch  v1.0.0
  professor + friend -> shared mailbox -> student   (one Runtime, one pump, lock-free)
  ----------------------------------------------------------------------
  Professor  2/4 sent   posted "project"  inbox=2
  Friend     1/3 sent   waiting before next invite
  ----------------------------------------------------------------------
  Mailbox  (2 queued)
    ASN  project    need_ability=8 need_time=9h
    PLAY soccer     6h
  ----------------------------------------------------------------------
  Student   active: ASN  lab        need_ability=5 need_time=6h
    phase: training (+1 ability, 3h)
    ability [######..............]  3/10
    stress  [####................]  2/10
    hours spent: 18        messages handled: 3
  ----------------------------------------------------------------------
  Recent activity
    student trained -> ability=3 stress=2
    professor posted assignment "project"  inbox=2
  ----------------------------------------------------------------------
  press Enter to quit
```

## Feature focus

This example is about **message routing on one cooperative pump**.

- **Dispatch by message kind.** The student's `Step1_TakeNext` pops one
  `Message` and routes by `Message::Kind`: an `Assignment` enters the
  train/sleep/work chain (`Step2_CheckAbility` branches on ability and stress); a
  `Play` enters the play chain. (see [uf_student.cpp](uf_student.cpp))

- **Lock-free shared mailbox on one pump.** Two spawners push and the student
  pops, but every step runs on the **same pump thread**, so [mailbox.cpp](mailbox.cpp)
  is a plain `std::deque` with **no mutex**. That is the core uniflow guarantee:
  one thread, no locks between cooperating modules.

- **Blocking work via SubmitAsync + poll.** The "n simulated hours" of work would
  stall the pump if run inline, so each chain step offloads it with
  `SubmitAsync(UF_FN(SimHours), ...)`, carries the returned `AsyncId` to a wait
  step via `Next(UF_FN(StepX_Wait), job)`, and there polls
  `AsyncResult<int>(job)` - `Stay()` while `pending()`, read `*r.return_value`
  when done. While the student "works", the pump keeps driving the spawners.

- **Console / ANSI render pattern.** A `Flow_Visualization` task snapshots the
  world each pump round into `g_snap`; a background render thread paints the
  dashboard ~25 fps from that snapshot, and the main thread blocks on
  `std::getline` (Enter quits). The renderer owns stdout, so the Runtime uses a
  `SilentObserver` to keep step traces off the screen.
  [console.h](console.h) / [console.cpp](console.cpp) is the same dependency-free
  ANSI helper used by the simulator and the console renderers of city_traffic and
  pick_and_place.

## File map

| File | Role |
| --- | --- |
| [uf_student.h](uf_student.h) / [.cpp](uf_student.cpp) | The worker. One `Task_Drain` pops a message, routes by kind, runs the matching chain, offloads SimHours via SubmitAsync. |
| [uf_professor.h](uf_professor.h) / [.cpp](uf_professor.cpp) | Spawner: posts assignments at random gaps, wakes the student. |
| [uf_friend.h](uf_friend.h) / [.cpp](uf_friend.cpp) | Spawner: posts play invites. Same shape as the professor. |
| [mailbox.h](mailbox.h) / [.cpp](mailbox.cpp) | Shared inbox (lock-free; pump thread only). |
| [globals.h](globals.h) / [.cpp](globals.cpp) | Message type, timing config, quit latch, recent-activity log. |
| [uf_visualization.h](uf_visualization.h) / [.cpp](uf_visualization.cpp) | Pump-side snapshot task + the background ANSI render loop. |
| [snapshot.h](snapshot.h) / [.cpp](snapshot.cpp) | Pump -> render-thread hand-off (carries a mutex; cross-thread). |
| [console.h](console.h) / [.cpp](console.cpp) | Reusable ANSI console helper. |
| [app.h](app.h) | Runtime + every module, two-phase init, silent observer, the `friend_` keyword-clash workaround. |
| [main.cpp](main.cpp) | Enable ANSI, start the app, run the render thread, block on Enter, shut down. |

## Build / run

Console only, nothing extra to install.

**Linux / macOS (g++ or clang++):**

```sh
cd cpp/examples/message_dispatch
g++ -std=c++17 -O2 -I../.. *.cpp -o message_dispatch -pthread
./message_dispatch
```

**Windows (MSVC, x64 Native Tools prompt):**

```bat
cd cpp\examples\message_dispatch
cl /std:c++17 /EHsc /O2 /I..\.. *.cpp /Fe:message_dispatch.exe
message_dispatch.exe
```

> Note: this demo assumes an ANSI terminal. On Windows, `console::EnableAnsi()`
> turns on the console's VT processing (Windows Terminal recommended).
