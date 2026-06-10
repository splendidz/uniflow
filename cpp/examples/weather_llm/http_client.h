// http_client.h - thin WinHTTP wrapper. Two operations used by the
// weather demo: a plain HTTPS GET and a Gemini generateContent POST.
// Static, free-function-shaped on purpose: UF_ASYNC submits these to
// the thread pool, so they must not capture 'this'.
#pragma once

#include <string>

namespace HttpClient
{
    // HTTPS GET. Throws std::runtime_error on transport failure.
    std::string Get(const std::string& url);

    // Gemini generateContent API (Google AI Studio). Returns the raw
    // response JSON; pass to ExtractGeminiText() to pull the model's
    // reply out. Free tier works for short prompts.
    std::string GeminiGenerate(const std::string& api_key,
                               const std::string& model,
                               const std::string& user_message,
                               int                max_output_tokens);

    // Pull the first {"text":"..."} value out of a Gemini response.
    // Forgiving: if the response is not in that shape, returns the
    // whole input so the user still sees something.
    std::string ExtractGeminiText(const std::string& response_json);

    // Pull an error "message":"..." out of an error response if any.
    std::string ExtractGeminiError(const std::string& response_json);

    // JSON-escape a string so it can be embedded inside a "..." literal.
    std::string JsonEscape(const std::string& s);
}
