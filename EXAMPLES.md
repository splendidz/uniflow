# uniflow Example Gallery

> 🌐 Language: [한국어](EXAMPLES.kr.md) | **English**

Six working projects. They all build and run as-is. Each example has its own detailed page inside its folder.

| Example | One line | Difficulty | UI |
|---|---|---|---|
| [city_traffic](cpp/examples/city_traffic/README.md) | Dozens of cars driving a city on a single pump (the finale) | ★★★★★ | Win32 |
| [cnc_pickers](cpp/examples/cnc_pickers/README.md) | Full CNC line simulation (reference project) | ★★★★☆ | Win32 |
| [shared_ostream](cpp/examples/shared_ostream/README.md) | Two modules share one ostringstream with no locks | ★☆☆☆☆ | Console |
| [queue_drain](cpp/examples/queue_drain/README.md) | Producer/consumer queue-drain pattern | ★★☆☆☆ | Win32 |
| [message_dispatch](cpp/examples/message_dispatch/README.md) | Two sponsors -> one student message dispatch | ★★★☆☆ | Win32 |
| [weather_llm](cpp/examples/weather_llm/README.md) | KMA XML -> Gemini summary (real async I/O) | ★★★★★ | Console |

Build/run is the same pattern for every example:

```powershell
# Visual Studio
add cpp\examples\<name>\<name>.vcxproj to your solution and hit F5

# CLI (MSVC)
cl /std:c++17 /EHsc /I cpp cpp\examples\<name>\*.cpp /Fe:<name>.exe
```

---

## Flagships

### [city_traffic](cpp/examples/city_traffic/README.md) - the grand finale

<p align="center">
  <img src="docs/videos/city_traffic.gif" alt="city_traffic demo" width="560"/>
</p>

Dozens of cars, each an independent uniflow module, accelerate/stop/turn around a city while watching shared traffic lights and the car ahead. Zero application-level threads - every car and signal runs on a single pump. [Page](cpp/examples/city_traffic/README.md) | [Folder](cpp/examples/city_traffic/)

### [cnc_pickers](cpp/examples/cnc_pickers/README.md) - the reference project

<p align="center">
  <img src="docs/videos/cnc_picker.gif" alt="cnc_pickers demo" width="560"/>
</p>

A virtual CNC machining line of three modules plus an orchestrator. Two pickers run the line continuously without ever sharing zone B. The Cmd -> Wait 2-step motion pattern is the core. [Page](cpp/examples/cnc_pickers/README.md) | [Folder](cpp/examples/cnc_pickers/)

---

## The rest

- [shared_ostream](cpp/examples/shared_ostream/README.md) - the smallest example. Two modules on the same Runtime alternate writes into one `std::ostringstream` with no locks. The code that *proves* uniflow's most essential property (all steps on one Runtime are one thread).
- [queue_drain](cpp/examples/queue_drain/README.md) - a "receive -> queue -> process" pattern. A Sender pushes bursts of messages; a Receiver drains until the queue is empty. Shows why the queue needs no lock.
- [message_dispatch](cpp/examples/message_dispatch/README.md) - professor/friend/student modules cooperating through a shared mailbox. Conditional branching within a flow and an async simulation.
- [weather_llm](cpp/examples/weather_llm/README.md) - real async I/O. Fetches KMA forecast XML over HTTPS and summarizes it with Gemini. Two async calls chained in one flow.

---

## Suggested reading order

New here? README's 3 quick tutorials -> [TUTORIAL.md](TUTORIAL.md) chapters 1-5 -> **shared_ostream** -> **queue_drain** -> **message_dispatch** -> **weather_llm** -> **cnc_pickers** -> **city_traffic**.

If you would rather see the *whole shape* first, skim **city_traffic** or **cnc_pickers**, then come back to the tutorial.
