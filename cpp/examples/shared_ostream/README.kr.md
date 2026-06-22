# shared_ostream

> 언어: **한국어** | [English](README.md) &nbsp;|&nbsp; [<- 예제 갤러리](../../EXAMPLES.kr.md)

가장 작은 예제. 같은 Runtime에 매단 모듈 두 개가 한 `std::ostringstream`에 락 없이 번갈아 적습니다. uniflow의 본질적 성질 하나를 보여줍니다: **같은 Runtime의 모든 step body는 한 펌프 스레드 위에서 돈다**. 모듈이 둘이든 열이든, 공유 자원에 mutex를 쓰지 않아도 race가 없습니다.

유한한 콘솔 프로그램입니다. writer 둘을 돌려 번갈아 쓰인 출력을 찍고, 검증한 뒤 종료합니다. 대시보드도 Enter 종료도 없으며, 검증 단계가 결과를 확인합니다.

---

## Feature focus

한 펌프 스레드 위의 lock-free 공유 상태. `Flow_Writer` 모듈 둘이 같은 `std::ostringstream`과 같은 turn 플래그를 mutex 없이 만집니다. Runtime이 모든 step을 단일 펌프 스레드에서 돌리므로, 공유 sink는 일관성을 유지하고 출력 순서가 정확히 보존됩니다.

---

## 무엇을 보여주나

- `Flow_Writer` 인스턴스 둘(`"Hello"`, `"World"`)이 같은 펌프 스레드 위에서 돈다
- 공유 turn 플래그로 순서 강제 (`SharedState::Turn()`) - 락 없음
- 출력 검증: `"Hello World."`가 정확히 10회 등장하는지 확인
- 프레임워크 기본 trace가 stdout을 오염시키지 않도록 `SilentObserver` 사용

---

## 파일 맵

| 파일 | 역할 |
| --- | --- |
| `main.cpp` | 진입점 + 검증 (`"Hello World."` 횟수 세기) |
| `app.h` | 한 Runtime, 두 `Flow_Writer`, 2단계 초기화, SilentObserver |
| `uf_writer.h` / `uf_writer.cpp` | `Flow_Writer` 모듈과 `Task_Write` 스텝 (lock-free 핵심) |
| `shared_state.h` / `shared_state.cpp` | 모든 writer가 만지는 공유 `ostringstream`과 turn 플래그 |

---

## 빌드 / 실행

콘솔 예제라 플랫폼 무관합니다.

Linux / macOS (g++):

```bash
g++ -std=c++17 -O2 -I../.. *.cpp -o shared_ostream -pthread
./shared_ostream
```

Windows (MSVC, `vcvars64.bat` 호출 후 이 디렉터리에서):

```powershell
cl /std:c++17 /EHsc /W4 /nologo /I..\.. *.cpp /Fe:shared_ostream.exe
.\shared_ostream.exe
```

실행 결과:

```text
=== shared_ostream: two writers, one log, no locks ===

--- output ---
Hello World. Hello World. Hello World. Hello World. Hello World. Hello World. Hello World. Hello World. Hello World. Hello World.
--- end ---

expected "Hello World." occurrences = 10, got = 10
PASS: shared log is race-free, order preserved
```

---

## 더 읽기

- 한 Runtime, 여러 모듈, lock-free 공유: [TUTORIAL.kr.md 챕터 5](../../TUTORIAL.kr.md)
- 전체 예제 갤러리: [EXAMPLES.kr.md](../../EXAMPLES.kr.md)
