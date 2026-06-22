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

#include <cstdlib>
#include <iostream>
#include <string>

int main()
{
    // Gemini returns Korean as UTF-8 (our extractor decodes \uXXXX
    // escapes into UTF-8 too). Without this, the default console
    // codepage (CP949 on Korean Windows) interprets the bytes as
    // its own encoding and the text shows up garbled.
    SetConsoleOutputCP(CP_UTF8);

    std::cout << "=== weather_llm: KMA front page -> Gemini summary ===\n\n";

    // Offer to enter a Gemini API key at startup. With a key the live LLM
    // summary runs; without one, the example just prints the fetched XML.
    std::cout
        << "This example fetches a weather page and can ask Google's Gemini\n"
        << "model to summarize it.\n\n"
        << "  - With a Gemini API key: it runs the live LLM summary.\n"
        << "  - Without a key:         it prints the fetched XML only.\n\n"
        << "How to get a free Gemini API key:\n"
        << "  1. Open https://aistudio.google.com/app/apikey in your browser\n"
        << "     and sign in with a Google account.\n"
        << "  2. Click \"Create API key\" (you can pick or create a project),\n"
        << "     then copy the generated key (it looks like \"AIza...\").\n"
        << "  3. Paste it at the prompt below. The key is used only for this\n"
        << "     run - it is not saved anywhere.\n\n"
        << "Enter your GEMINI_API_KEY (or just press Enter to skip): "
        << std::flush;

    std::string key;
    std::getline(std::cin, key);
    const std::size_t a = key.find_first_not_of(" \t\r\n");
    const std::size_t b = key.find_last_not_of(" \t\r\n");
    key = (a == std::string::npos) ? std::string() : key.substr(a, b - a + 1);

    if (!key.empty())
    {
        // The flow reads GEMINI_API_KEY via std::getenv; inject what was typed.
        _putenv_s("GEMINI_API_KEY", key.c_str());
        std::cout << "\n[weather] key accepted - running the LLM summary.\n\n";
    }
    else
    {
        std::cout << "\n[weather] no key entered - printing the raw XML only.\n\n";
    }

    App& app = App::inst();
    app.Start();
    app.Shutdown();

    return 0;
}
