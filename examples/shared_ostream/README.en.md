# shared_ostream

> 🌐 Language: [한국어](README.md) | **English** &nbsp;|&nbsp; [<- Example gallery](../../EXAMPLES.en.md)

The smallest example. Two modules attached to the same Runtime alternate writes into one `std::ostringstream` with no locks. It is the code that *proves* uniflow's most essential property: **every step body on the same Runtime runs on one thread**. Two modules or ten, you can share a resource with no mutex and no race.

---

## What it shows

- The very fact that two `UF_Writer` instances (`"Hello"`, `"World"`) run on the same pump thread
- Order enforced by a shared turn flag (`SharedState::Turn()`)
- Output verification: `"Hello World."` must appear exactly 10 times

**Why it matters** - this example proves "safe without locks" in code, not in words. Two writers alternate into one buffer with no mutex, and the output is still exactly preserved.

---

## Files worth reading

- [uf_writer.cpp](uf_writer.cpp) - the 16-line core
- [main.cpp](main.cpp) - the verification logic

---

## Build / run

A console example, so it is platform-independent.

```powershell
cl /std:c++17 /EHsc /I . examples\shared_ostream\*.cpp /Fe:shared_ostream.exe
shared_ostream.exe
```

```bash
g++ -std=c++17 -O2 -pthread -I . examples/shared_ostream/*.cpp -o shared_ostream
./shared_ostream
```

Output:

```text
=== shared_ostream: two writers, one log, no locks ===
...
--- output ---
Hello World. Hello World. Hello World. Hello World. Hello World.
Hello World. Hello World. Hello World. Hello World. Hello World.
--- end ---

expected "Hello World." occurrences = 10, got = 10
PASS: shared log is race-free, order preserved
```

---

## Read more

- One Runtime, many modules, lock-free sharing: [TUTORIAL.en.md chapter 5](../../TUTORIAL.en.md)
- Full example gallery: [EXAMPLES.en.md](../../EXAMPLES.en.md)
