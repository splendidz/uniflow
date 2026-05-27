# uniflow Examples

> 🌐 Language: **English** · [한국어](EXAMPLES.md)

Five working projects. They all build and run as-is.

| # | Name | One-liner | Difficulty | UI |
|---|---|---|---|---|
| 0 | [cnc_pickers](#0-cnc_pickers--the-reference-project) | Simulated CNC line | ★★★★☆ | Win32 |
| 1 | [shared_ostream](#1-shared_ostream--lock-free-shared-state) | Two modules share one ostringstream, no lock | ★☆☆☆☆ | Console |
| 2 | [queue_drain](#2-queue_drain--send-receive-drain) | Producer / consumer with queue drain | ★★☆☆☆ | Win32 |
| 3 | [message_dispatch](#3-message_dispatch--professor-friend-student) | Two spawners dispatching messages to one worker | ★★★☆☆ | Win32 |
| 4 | [weather_llm](#4-weather_llm--kma-llm) | KMA XML → Gemini summary | ★★★★★ | Console |

Every example builds the same way:

```powershell
# Visual Studio
add examples\<name>\<name>.vcxproj to your solution and hit F5

# CLI (MSVC)
cl /std:c++17 /EHsc /I . examples\<name>\*.cpp /Fe:<name>.exe
```

---

## 0. cnc_pickers — the reference project

![cnc_pickers demo](docs/videos/cnc_pickers.gif)

> 📹 Video slot: `docs/videos/cnc_pickers.mp4`

A virtual CNC machining line: three modules (`UF_LoadPicker`, `UF_Stage`, `UF_UnloadPicker`) plus one orchestrator. The two pickers must never share zone B simultaneously while the line keeps running continuously.

**What it demonstrates**
- The *Cmd → Wait* two-step pattern for motor/gripper motion (`OnLoad_CmdMoveToSource` → `OnLoad_WaitAtSource`)
- A virtual hardware model (`MotorAxis`) with realistic settle-time polling
- An orchestrator that watches `picker.IsIdle()` / `stage.state()` and decides the next action
- A `UF_Visualization` module + Win32 message loop for real-time line rendering
- `EnvLogObserver` mirrors output to both console and a log file

**Worth a read**
- [uf_orchestrator.cpp](examples/cnc_pickers/uf_orchestrator.cpp) — every scheduling decision
- [uf_load_picker.cpp](examples/cnc_pickers/uf_load_picker.cpp) — a 16-step picker cycle
- [app.h](examples/cnc_pickers/app.h) — the two-phase module init pattern

---

## 1. shared_ostream — lock-free shared state

![shared_ostream demo](docs/videos/shared_ostream.gif)

> 📹 Video slot: `docs/videos/shared_ostream.mp4`

The smallest example. Two modules on the same Runtime take turns writing to one `std::ostringstream` — with no lock.

**What it demonstrates**
- That two `UF_Writer` instances (`"Hello"`, `"World"`) actually run on the same pump thread
- A shared turn flag (`SharedState::Turn()`) to enforce ordering
- End-of-run verification: counts that `"Hello World."` appears exactly 10 times in the output

**Why it matters** — uniflow's most fundamental property: *every step body on the same Runtime runs on one thread*. Two modules, ten modules — shared state still needs no mutex. This example is the proof.

**Sample output**
```
=== shared_ostream: two writers, one log, no locks ===
...
--- output ---
Hello World. Hello World. Hello World. Hello World. Hello World.
Hello World. Hello World. Hello World. Hello World. Hello World.
--- end ---

expected "Hello World." occurrences = 10, got = 10
PASS: shared log is race-free, order preserved
```

**Worth a read**
- [uf_writer.cpp](examples/shared_ostream/uf_writer.cpp) — 16 lines of substance
- [main.cpp](examples/shared_ostream/main.cpp) — the verification logic

---

## 2. queue_drain — send, receive, drain

![queue_drain demo](docs/videos/queue_drain.gif)

> 📹 Video slot: `docs/videos/queue_drain.mp4`

The classic "receive thread → queue → processing thread" pattern, ported to uniflow. `UF_Sender` pushes 1–10 messages every second; `UF_Receiver` drains the queue one at a time.

**What it demonstrates**
- The queue is *lock-free* because sender and receiver share the pump thread
- After sender pushes, it checks `receiver.IsIdle()` and wakes it with `UF_START_FLOW` immediately
- Dispatch pattern in the receiver — `OnRecv_TakeNext` → `OnRecv_Add` or `OnRecv_Sub` → back to `OnRecv_TakeNext`
- Win32 panel showing both source vectors, the current queue, receiver state, the latest result

**Real-world mapping** — in a real system, the sender lives on another machine or process. uniflow gives you the *skeleton for what happens after the message arrives*. A mutex-friendly inbox plus one receiver module is enough for production.

**Worth a read**
- [uf_sender.cpp](examples/queue_drain/uf_sender.cpp) — burst generation + waking the receiver
- [uf_receiver.cpp](examples/queue_drain/uf_receiver.cpp) — drain loop + dispatch
- [uf_visualization.cpp](examples/queue_drain/uf_visualization.cpp) — chip-based Win32 panel

---

## 3. message_dispatch — professor, friend, student

![message_dispatch demo](docs/videos/message_dispatch.gif)

> 📹 Video slot: `docs/videos/message_dispatch.mp4`

Three modules (`UF_Professor`, `UF_Friend`, `UF_Student`) cooperate through a shared mailbox. The professor sends assignments, the friend sends play invitations; the student handles them in order. If ability is low, the student `trains`; if too stressed, they `sleep`; otherwise they `do the work`. `Play` is a separate chain.

**What it demonstrates**
- Three concurrent modules with no inter-module locks — communication is a lock-free mailbox plus `IsIdle` polling
- *Conditional chaining* inside a single flow — `OnAssign_CheckAbility` decides which step comes next
- `UF_ASYNC(SimHours, n)` simulates an "n-hour task". While the student is "working", the pump keeps running other modules
- Win32 panel: chips for the mailbox, ability/stress gauges, pending lists on both spawners

**Worth a read**
- [uf_student.cpp](examples/message_dispatch/uf_student.cpp) — train / sleep / work / play branching
- [uf_visualization.cpp](examples/message_dispatch/uf_visualization.cpp) — clean panels + gauge bars
- [app.h](examples/message_dispatch/app.h) — workaround for the `friend` keyword (member named `friend_`)

---

## 4. weather_llm — KMA + LLM

![weather_llm demo](docs/videos/weather_llm.gif)

> 📹 Video slot: `docs/videos/weather_llm.mp4`

The async one. A single module (`UF_Weather`) talks to *two external services* across three steps:

1. `UF_ASYNC`: HTTPS GET the KMA DFS forecast XML via WinHTTP
2. `UF_ASYNC`: POST the XML to Google's Gemini `generateContent` endpoint
3. Print the Korean summary Gemini returns

**What it demonstrates**
- *Two async calls in a single flow*, feeding the first result into the second
- Step bodies *never* block — the pump thread is free during both GET and POST
- Failure + timeout handling. `AsyncResult<T>::failed()` and `is_timeout()`
- UTF-8 console mode (`SetConsoleOutputCP(CP_UTF8)`) — so the Korean response renders correctly
- API key injection via env var (`GEMINI_API_KEY`)

**Sample run**
```powershell
$env:GEMINI_API_KEY = "AIza...<key from aistudio.google.com>"
weather_llm.exe
```
```
=== weather_llm: KMA front page -> Gemini summary ===
[weather] fetching https://www.weather.go.kr/wid/queryDFS.jsp?gridx=60&gridy=127
[weather] got 6698 bytes of XML
[weather] submitting POST to Gemini (gemini-2.5-flash, prompt=...)

=== Gemini summary ===
  location: Seoul (grid 60, 127)
  temperature: 21.0 C
  sky/precipitation: rain
  humidity: 85%
  wind: east 3.1 m/s
  air_quality (PM10/PM2.5): <not found - this feed does not carry fine-dust readings>
=== end ===
```

**Worth a read**
- [uf_weather.cpp](examples/weather_llm/uf_weather.cpp) — the 3-step async chain
- [http_client.cpp](examples/weather_llm/http_client.cpp) — WinHTTP wrapper + JSON escape + response parser
- [main.cpp](examples/weather_llm/main.cpp) — UTF-8 console setup

**Get an API key** — https://aistudio.google.com/app/apikey — free with a Google account, works within free-tier limits.

---

## Suggested reading order

If you're new: **Tutorial chapters 1–5** → **shared_ostream** → **queue_drain** → **message_dispatch** → **weather_llm** → **cnc_pickers**.

If you want the big picture first, scan **cnc_pickers** then go back to the tutorial.

---

## Where to drop demo videos

Demo recordings live under [docs/videos/](docs/videos/). Recommended specs:

- Length: 20–40 s
- Format: MP4 (renders inline on Github) or GIF (smaller, auto-plays)
- Filename: `<example_name>.mp4` or `<example_name>.gif`
- Resolution: 720p–1080p, 30 fps

Each section above has an `![]()` line preconfigured for the gif path and a `📹 Video slot:` note for the mp4 path — drop a file with the matching name and it shows up.
