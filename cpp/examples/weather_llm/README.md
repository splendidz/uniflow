# weather_llm

> 🌐 Language: [한국어](README.kr.md) | **English** &nbsp;|&nbsp; [<- Example gallery](../../../EXAMPLES.md)

Real async I/O. A single module (`UF_Weather`) talks to the *outside world* across three steps:

1. `UF_ASYNC`: HTTPS GET the KMA DFS forecast XML via WinHTTP
2. `UF_ASYNC`: POST that XML to the Google Gemini `generateContent` endpoint
3. Print the Korean summary Gemini returns to the console

---

## What it shows

- *Two asyncs* in one module's single flow. The first result is passed as an argument to the second call
- Step bodies *never* block - the pump thread is free during both the GET and the POST
- Failure/timeout handling. `AsyncResult<T>::failed()` and `is_timeout()`
- UTF-8 console mode (`SetConsoleOutputCP(CP_UTF8)`) for correct Korean output
- API key injected via an environment variable (`GEMINI_API_KEY`)

---

## Files worth reading

- [uf_weather.cpp](uf_weather.cpp) - the 3-stage async chain
- [http_client.cpp](http_client.cpp) - a WinHTTP wrapper + JSON escape + response parser
- [main.cpp](main.cpp) - the UTF-8 console setup

---

## Build / run

It uses WinHTTP, so this is a Windows + MSVC example. Get a free API key from Google AI Studio and pass it via an environment variable.

```powershell
# Visual Studio
add cpp\examples\weather_llm\weather_llm.vcxproj to your solution and hit F5

# CLI (MSVC)
cl /std:c++17 /EHsc /I cpp cpp\examples\weather_llm\*.cpp /Fe:weather_llm.exe

$env:GEMINI_API_KEY = "AIza...<key from aistudio.google.com>"
weather_llm.exe
```

Sample run:

```text
=== weather_llm: KMA front page -> Gemini summary ===
[weather] fetching https://www.weather.go.kr/wid/queryDFS.jsp?gridx=60&gridy=127
[weather] got 6698 bytes of XML
[weather] submitting POST to Gemini (gemini-2.5-flash, prompt=...)

=== Gemini summary ===
  location: Seoul (grid 60, 127)
  temperature: 21.0 C
  sky/precipitation: rain
  humidity: 85%
  wind: E 3.1m/s
  air_quality (PM10/PM2.5): <not found - this feed has no particulate data>
=== end ===
```

**Get an API key** - https://aistudio.google.com/app/apikey - free with a Google account, works within the free tier.

---

## Read more

- UF_ASYNC and receiving results, timeouts/errors: [TUTORIAL.md chapters 6-7](../../../TUTORIAL.md)
- Full example gallery: [EXAMPLES.md](../../../EXAMPLES.md)
