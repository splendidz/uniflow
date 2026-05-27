// weather_llm - hardest of the four examples. One Uniflow module runs
// a three-step flow:
//
//   1) UF_ASYNC: HTTPS GET the KMA front page (Korean weather site).
//   2) UF_ASYNC: POST that HTML to Google's Gemini generateContent
//      endpoint, asking it to extract Seoul location / temperature /
//      fine-dust readings.
//   3) Print the model's reply.
//
// Both async calls run on the Runtime's thread pool; the pump thread
// is never blocked. Set GEMINI_API_KEY in the environment to enable
// the LLM call - free keys from https://aistudio.google.com/app/apikey.
#include "app.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <iostream>

int main()
{
    // Gemini returns Korean as UTF-8 (our extractor decodes \uXXXX
    // escapes into UTF-8 too). Without this, the default console
    // codepage (CP949 on Korean Windows) interprets the bytes as
    // its own encoding and the text shows up garbled.
    SetConsoleOutputCP(CP_UTF8);

    std::cout << "=== weather_llm: KMA front page -> Gemini summary ===\n\n";

    App& app = App::inst();
    app.Start();
    app.Shutdown();

    return 0;
}
