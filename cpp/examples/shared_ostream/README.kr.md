# shared_ostream

> 🌐 언어: **한국어** | [English](README.md) &nbsp;|&nbsp; [<- 예제 갤러리](../../../EXAMPLES.kr.md)

가장 작은 예제. 같은 Runtime에 매단 모듈 두 개가 한 `std::ostringstream`에 락 없이 번갈아 적습니다. uniflow의 가장 본질적 성질을 *증명*하는 코드입니다: **같은 Runtime의 모든 step body는 한 스레드 위에서 돈다**. 모듈이 둘이든 열이든, 공유 자원에 mutex를 쓰지 않아도 race가 없습니다.

---

## 무엇을 보여주나

- `UF_Writer` 인스턴스 둘(`"Hello"`, `"World"`)이 같은 펌프 스레드 위에서 돈다는 사실 자체
- 공유 turn 플래그로 순서 강제 (`SharedState::Turn()`)
- 출력 검증: `"Hello World."`가 정확히 10회 등장하는지 확인

**왜 중요한가** - 이 예제는 "lock 없이 안전하다"를 말이 아니라 코드로 증명합니다. 두 writer가 한 버퍼에 번갈아 쓰는데 mutex가 없고, 그럼에도 출력이 정확히 보존됩니다.

---

## 보면 좋은 파일

- [uf_writer.cpp](uf_writer.cpp) - 16줄짜리 핵심
- [main.cpp](main.cpp) - 검증 로직

---

## 빌드 / 실행

콘솔 예제라 플랫폼 무관합니다.

```powershell
cl /std:c++17 /EHsc /I cpp cpp\examples\shared_ostream\*.cpp /Fe:shared_ostream.exe
shared_ostream.exe
```

```bash
g++ -std=c++17 -O2 -pthread -I cpp cpp/examples/shared_ostream/*.cpp -o shared_ostream
./shared_ostream
```

실행 결과:

```text
=== shared_ostream: two writers, one log, no locks ===
...
--- output ---
Hello World. Hello World. Hello World. Hello World. Hello World. Hello World. Hello World. Hello World. Hello World. Hello World.
--- end ---

expected "Hello World." occurrences = 10, got = 10
PASS: shared log is race-free, order preserved
```

---

## 더 읽기

- 한 Runtime, 여러 모듈, lock-free 공유: [TUTORIAL.kr.md 챕터 5](../../../TUTORIAL.kr.md)
- 전체 예제 갤러리: [EXAMPLES.kr.md](../../../EXAMPLES.kr.md)
