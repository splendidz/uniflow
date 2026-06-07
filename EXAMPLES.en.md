# uniflow Example Gallery

> 🌐 Language: [한국어](EXAMPLES.md) | **English**

Six working projects. They all build and run as-is. Each example has its own detailed page inside its folder.

| Example | One line | Difficulty | UI |
|---|---|---|---|
| [city_traffic](examples/city_traffic/README.en.md) | Dozens of cars driving a city on a single pump (the finale) | ★★★★★ | Win32 |
| [cnc_pickers](examples/cnc_pickers/README.en.md) | Full CNC line simulation (reference project) | ★★★★☆ | Win32 |
| [shared_ostream](examples/shared_ostream/README.en.md) | Two modules share one ostringstream with no locks | ★☆☆☆☆ | Console |
| [queue_drain](examples/queue_drain/README.en.md) | Producer/consumer queue-drain pattern | ★★☆☆☆ | Win32 |
| [message_dispatch](examples/message_dispatch/README.en.md) | Two sponsors -> one student message dispatch | ★★★☆☆ | Win32 |
| [weather_llm](examples/weather_llm/README.en.md) | KMA XML -> Gemini summary (real async I/O) | ★★★★★ | Console |

Build/run is the same pattern for every example:

```powershell
# Visual Studio
add examples\<name>\<name>.vcxproj to your solution and hit F5

# CLI (MSVC)
cl /std:c++17 /EHsc /I . examples\<name>\*.cpp /Fe:<name>.exe
```

---

## Flagships

### [city_traffic](examples/city_traffic/README.en.md) - the grand finale

<p align="center">
  <img src="docs/videos/city_traffic.gif" alt="city_traffic demo" width="560"/>
</p>

Dozens of cars, each an independent uniflow module, accelerate/stop/turn around a city while watching shared traffic lights and the car ahead. Zero application-level threads - every car and signal runs on a single pump. [Page](examples/city_traffic/README.en.md) | [Folder](examples/city_traffic/)

### [cnc_pickers](examples/cnc_pickers/README.en.md) - the reference project

<p align="center">
  <img src="docs/videos/cnc_picker.gif" alt="cnc_pickers demo" width="560"/>
</p>

A virtual CNC machining line of three modules plus an orchestrator. Two pickers run the line continuously without ever sharing zone B. The Cmd -> Wait 2-step motion pattern is the core. [Page](examples/cnc_pickers/README.en.md) | [Folder](examples/cnc_pickers/)

---

## The rest

- [shared_ostream](examples/shared_ostream/README.en.md) - the smallest example. Two modules on the same Runtime alternate writes into one `std::ostringstream` with no locks. The code that *proves* uniflow's most essential property (all steps on one Runtime are one thread).
- [queue_drain](examples/queue_drain/README.en.md) - a "receive -> queue -> process" pattern. A Sender pushes bursts of messages; a Receiver drains until the queue is empty. Shows why the queue needs no lock.
- [message_dispatch](examples/message_dispatch/README.en.md) - professor/friend/student modules cooperating through a shared mailbox. Conditional branching within a flow and an async simulation.
- [weather_llm](examples/weather_llm/README.en.md) - real async I/O. Fetches KMA forecast XML over HTTPS and summarizes it with Gemini. Two async calls chained in one flow.

---

## Suggested reading order

New here? README's 3 quick tutorials -> [TUTORIAL.en.md](TUTORIAL.en.md) chapters 1-5 -> **shared_ostream** -> **queue_drain** -> **message_dispatch** -> **weather_llm** -> **cnc_pickers** -> **city_traffic**.

If you would rather see the *whole shape* first, skim **city_traffic** or **cnc_pickers**, then come back to the tutorial.
