# quickstart

> 🌐 언어: **한국어** | [English](README.en.md) &nbsp;|&nbsp; [<- 예제 갤러리](../../EXAMPLES.md)

[README](../../README.md)의 3개 퀵 튜토리얼을 그대로 컴파일/실행할 수 있는 단일 파일들입니다. CI가 Windows/Linux/macOS에서 이 파일들을 빌드하고 실행하므로, README의 코드가 항상 동작함이 보증됩니다.

| 파일 | 튜토리얼 |
|---|---|
| [tut1_roundrobin.cpp](tut1_roundrobin.cpp) | 흐름 여럿이 동시에 (라운드로빈) |
| [tut2_async.cpp](tut2_async.cpp) | 시간이 걸리는 일은 풀로 (async) |
| [tut3_observer.cpp](tut3_observer.cpp) | 관측은 공짜다 (observer) |

전부 콘솔 전용이라 플랫폼 무관합니다.

```bash
g++ -std=c++17 -O2 -pthread -I . examples/quickstart/tut1_roundrobin.cpp -o tut1 && ./tut1
```

```powershell
cl /std:c++17 /EHsc /I . examples\quickstart\tut1_roundrobin.cpp /Fe:tut1.exe && tut1.exe
```
