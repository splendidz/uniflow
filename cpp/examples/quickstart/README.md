# quickstart

> 🌐 Language: [한국어](README.kr.md) | **English** &nbsp;|&nbsp; [<- Example gallery](../../../EXAMPLES.md)

Single-file, compile-and-run versions of the 3 quick tutorials from the [README](../../../README.md). CI builds and runs these on Windows/Linux/macOS, so the README code is guaranteed to work.

| File | Tutorial |
|---|---|
| [tut1_roundrobin.cpp](tut1_roundrobin.cpp) | Many flows at once (round-robin) |
| [tut2_async.cpp](tut2_async.cpp) | Throw slow work to the pool (async) |
| [tut3_observer.cpp](tut3_observer.cpp) | Observability is free (observer) |

All are console-only and platform-independent.

```bash
g++ -std=c++17 -O2 -pthread -I cpp cpp/examples/quickstart/tut1_roundrobin.cpp -o tut1 && ./tut1
```

```powershell
cl /std:c++17 /EHsc /I cpp cpp\examples\quickstart\tut1_roundrobin.cpp /Fe:tut1.exe && tut1.exe
```
