# uniflow example gallery

> Language: [한국어](EXAMPLES.kr.md) | **English**

Six reference examples are provided **identically in C++ / Python / C#** (except weather_llm). Each
has a detailed page in its own folder and covers a specific uniflow capability.

| # | Example | Focus | Render | Python | C# |
|---|---|---|---|---|---|
| 1 | [pick_and_place](examples/pick_and_place/README.md) | Task<Flow> units + orchestrator state polling + async-poll acks (reference) | dual | [py](../python/examples/pick_and_place.py) | [cs](../cs/examples/pick_and_place/) |
| 2 | [city_traffic](examples/city_traffic/README.md) | dozens of modules on one thread + lock-free shared World | dual | [py](../python/examples/city_traffic.py) | [cs](../cs/examples/city_traffic/) |
| 3 | [simulator](examples/simulator/README.md) | VirtualClock scale/freeze + the renderer is a flow + lock-free snapshot | console | [py](../python/examples/simulator.py) | [cs](../cs/examples/simulator/) |
| 4 | [message_dispatch](examples/message_dispatch/README.md) | routing by message kind + lock-free mailbox + async poll | console | [py](../python/examples/message_dispatch.py) | [cs](../cs/examples/message_dispatch/) |
| 5 | [queue_drain](examples/queue_drain/README.md) | single-thread producer/consumer + park/relaunch wake | console | [py](../python/examples/queue_drain.py) | [cs](../cs/examples/queue_drain/) |
| 6 | [shared_ostream](examples/shared_ostream/README.md) | one pump = lock-free shared state (minimal) | console | [py](../python/examples/shared_ostream.py) | [cs](../cs/examples/shared_ostream/) |
| + | [weather_llm](examples/weather_llm/README.md) | two async stages chained, pump never blocks on the network (C++ only) | console | - | - |

> "dual" means a Win32 GUI on Windows and an ANSI console on Linux/macOS (`UF_RENDER=console` forces
> console on Windows too). The rest are console everywhere and run with nothing to install.

Build / run:

```sh
# C++ console example (Linux/macOS)
g++ -std=c++17 -O2 -I cpp cpp/examples/<name>/*.cpp -o <name> -pthread
# Python
python python/examples/<name>.py
# C#
dotnet run --project cs/examples/<name>
```

On Windows the dual-render examples (pick_and_place / city_traffic) open as `.vcxproj`, and console
examples build with `cl /std:c++17 /EHsc /I cpp cpp\examples\<name>\*.cpp /Fe:<name>.exe`.

---

## Suggested reading order

If you are new: [README](../README.md) Quick Start -> [TUTORIAL.md](TUTORIAL.md) ->
**6 shared_ostream** -> **3 simulator** -> **5 queue_drain** -> **4 message_dispatch** ->
**2 city_traffic** -> **1 pick_and_place**.

To see the overall shape first, skim **city_traffic** or **pick_and_place**, then return to the
tutorial.
