# shared_ostream

> Language: [한국어](README.kr.md) | **English** &nbsp;|&nbsp; [<- Example gallery](../../EXAMPLES.md)

The smallest example. Two modules attached to the same Runtime alternate writes into one `std::ostringstream` with no locks. It demonstrates one of uniflow's essential properties: **every step body on the same Runtime runs on one pump thread**. Whether two modules or ten, a resource can be shared with no mutex and no race.

It is a finite console program: it runs the two writers, prints the interleaved-yet-ordered output, verifies it, then exits. There is no dashboard and no Enter-to-quit; the verification step confirms the result.

---

## Feature focus

Lock-free shared state on one pump thread. Two `Flow_Writer` modules touch the same `std::ostringstream` and the same turn flag with no mutex anywhere. Because the Runtime drives every step on a single pump thread, the shared sink stays consistent and the output order is exactly preserved.

---

## What it shows

- Two `Flow_Writer` instances (`"Hello"`, `"World"`) running on the same pump thread
- Order enforced by a shared turn flag (`SharedState::Turn()`) - no lock
- Output verification: `"Hello World."` must appear exactly 10 times
- A `SilentObserver` so the framework's default trace does not pollute stdout

---

## File map

| File | Role |
| --- | --- |
| `main.cpp` | entry point + verification (counts `"Hello World."`) |
| `app.h` | one Runtime, two `Flow_Writer`, two-phase init, SilentObserver |
| `uf_writer.h` / `uf_writer.cpp` | the `Flow_Writer` module and its `Task_Write` steps (the lock-free core) |
| `shared_state.h` / `shared_state.cpp` | the shared `ostringstream` and turn flag every writer touches |

---

## Build / run

A console example, platform-independent.

Linux / macOS (g++):

```bash
g++ -std=c++17 -O2 -I../.. *.cpp -o shared_ostream -pthread
./shared_ostream
```

Windows (MSVC, from inside this directory after `vcvars64.bat`):

```powershell
cl /std:c++17 /EHsc /W4 /nologo /I..\.. *.cpp /Fe:shared_ostream.exe
.\shared_ostream.exe
```

Output:

```text
=== shared_ostream: two writers, one log, no locks ===

--- output ---
Hello World. Hello World. Hello World. Hello World. Hello World. Hello World. Hello World. Hello World. Hello World. Hello World.
--- end ---

expected "Hello World." occurrences = 10, got = 10
PASS: shared log is race-free, order preserved
```

---

## Read more

- One Runtime, many modules, lock-free sharing: [TUTORIAL.md chapter 5](../../TUTORIAL.md)
- Full example gallery: [EXAMPLES.md](../../EXAMPLES.md)
