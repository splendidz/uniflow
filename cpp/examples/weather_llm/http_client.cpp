#include "http_client.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    std::wstring Widen(const std::string& s)
    {
        if (s.empty())
            return {};
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                    static_cast<int>(s.size()),
                                    nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(),
                            static_cast<int>(s.size()),
                            w.data(), n);
        return w;
    }

    // Generic driver. method is L"GET" or L"POST"; headers come in
    // one-per-string (no trailing CRLFs); body is raw bytes.
    std::string DoRequest(const std::string& url,
                          const wchar_t* method,
                          const std::vector<std::wstring>& headers,
                          const std::string& body)
    {
        std::wstring wurl = Widen(url);

        URL_COMPONENTS uc{};
        uc.dwStructSize      = sizeof(uc);
        uc.dwSchemeLength    = static_cast<DWORD>(-1);
        uc.dwHostNameLength  = static_cast<DWORD>(-1);
        uc.dwUrlPathLength   = static_cast<DWORD>(-1);
        uc.dwExtraInfoLength = static_cast<DWORD>(-1);
        if (!WinHttpCrackUrl(wurl.c_str(),
                             static_cast<DWORD>(wurl.size()), 0, &uc))
            throw std::runtime_error("WinHttpCrackUrl failed");

        std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
        std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
        if (uc.dwExtraInfoLength > 0)
            path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);

        bool         is_https = (uc.nScheme == INTERNET_SCHEME_HTTPS);
        INTERNET_PORT port    = uc.nPort;

        HINTERNET hSession = WinHttpOpen(L"weather-uniflow/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!hSession)
            throw std::runtime_error("WinHttpOpen failed");

        HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
        if (!hConnect)
        {
            WinHttpCloseHandle(hSession);
            throw std::runtime_error("WinHttpConnect failed");
        }

        DWORD flags = is_https ? WINHTTP_FLAG_SECURE : 0;
        HINTERNET hReq = WinHttpOpenRequest(hConnect, method, path.c_str(),
            nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
        if (!hReq)
        {
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            throw std::runtime_error("WinHttpOpenRequest failed");
        }

        std::wstring all_headers;
        for (const auto& h : headers)
        {
            all_headers += h;
            all_headers += L"\r\n";
        }

        LPCWSTR hdr_ptr  = all_headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                               : all_headers.c_str();
        DWORD   hdr_len  = all_headers.empty() ? 0
                                               : static_cast<DWORD>(all_headers.size());
        LPVOID  body_ptr = body.empty() ? WINHTTP_NO_REQUEST_DATA
                                        : const_cast<char*>(body.data());
        DWORD   body_len = static_cast<DWORD>(body.size());

        if (!WinHttpSendRequest(hReq, hdr_ptr, hdr_len,
                                body_ptr, body_len, body_len, 0))
        {
            DWORD err = GetLastError();
            WinHttpCloseHandle(hReq);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            throw std::runtime_error("WinHttpSendRequest failed err="
                                     + std::to_string(err));
        }

        if (!WinHttpReceiveResponse(hReq, nullptr))
        {
            DWORD err = GetLastError();
            WinHttpCloseHandle(hReq);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            throw std::runtime_error("WinHttpReceiveResponse failed err="
                                     + std::to_string(err));
        }

        std::string result;
        for (;;)
        {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hReq, &avail))
                break;
            if (avail == 0)
                break;
            std::string buf(avail, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(hReq, buf.data(), avail, &read))
                break;
            result.append(buf.data(), read);
        }

        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return result;
    }
}

namespace HttpClient
{

std::string Get(const std::string& url)
{
    return DoRequest(url, L"GET", {}, "");
}

std::string JsonEscape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s)
    {
        switch (c)
        {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x",
                              static_cast<unsigned char>(c));
                out += buf;
            }
            else
            {
                out += c;
            }
        }
    }
    return out;
}

std::string GeminiGenerate(const std::string& api_key,
                           const std::string& model,
                           const std::string& user_message,
                           int max_output_tokens)
{
    std::string body;
    body += "{\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"";
    body += JsonEscape(user_message);
    body += "\"}]}],\"generationConfig\":{\"maxOutputTokens\":";
    body += std::to_string(max_output_tokens);
    body += "}}";

    std::vector<std::wstring> headers = {
        L"Content-Type: application/json",
        L"x-goog-api-key: " + Widen(api_key),
    };

    std::string url = "https://generativelanguage.googleapis.com/v1beta/models/"
                    + model + ":generateContent";
    return DoRequest(url, L"POST", headers, body);
}

std::string ExtractGeminiText(const std::string& resp)
{
    // First {"text":"..."} in the response - that is
    // candidates[0].content.parts[0].text.
    auto pos = resp.find("\"text\":");
    if (pos == std::string::npos)
        return resp;
    pos += 7;
    // Skip optional whitespace, then the opening quote.
    while (pos < resp.size() && (resp[pos] == ' ' || resp[pos] == '\t'))
        ++pos;
    if (pos >= resp.size() || resp[pos] != '"')
        return resp;
    ++pos;

    std::string out;
    while (pos < resp.size())
    {
        char c = resp[pos];
        if (c == '\\' && pos + 1 < resp.size())
        {
            char n = resp[pos + 1];
            switch (n)
            {
            case 'n':  out += '\n'; pos += 2; break;
            case 't':  out += '\t'; pos += 2; break;
            case 'r':  out += '\r'; pos += 2; break;
            case '"':  out += '"';  pos += 2; break;
            case '\\': out += '\\'; pos += 2; break;
            case '/':  out += '/';  pos += 2; break;
            case 'u':
                if (pos + 5 < resp.size())
                {
                    // Decode a single \uXXXX escape to UTF-8. Korean text from
                    // Gemini often arrives in this form.
                    unsigned code = 0;
                    auto hex = [](char h) -> int
                    {
                        if (h >= '0' && h <= '9') return h - '0';
                        if (h >= 'a' && h <= 'f') return 10 + (h - 'a');
                        if (h >= 'A' && h <= 'F') return 10 + (h - 'A');
                        return -1;
                    };
                    int h0 = hex(resp[pos + 2]);
                    int h1 = hex(resp[pos + 3]);
                    int h2 = hex(resp[pos + 4]);
                    int h3 = hex(resp[pos + 5]);
                    if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0)
                    {
                        out += n; pos += 2; break;
                    }
                    code = (h0 << 12) | (h1 << 8) | (h2 << 4) | h3;
                    if (code < 0x80)
                    {
                        out += static_cast<char>(code);
                    }
                    else if (code < 0x800)
                    {
                        out += static_cast<char>(0xC0 | (code >> 6));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    else
                    {
                        out += static_cast<char>(0xE0 | (code >> 12));
                        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (code & 0x3F));
                    }
                    pos += 6;
                }
                else
                {
                    out += n; pos += 2;
                }
                break;
            default: out += n; pos += 2; break;
            }
        }
        else if (c == '"')
        {
            break;
        }
        else
        {
            out += c;
            ++pos;
        }
    }
    return out;
}

std::string ExtractGeminiError(const std::string& resp)
{
    auto pos = resp.find("\"message\":\"");
    if (pos == std::string::npos)
        return {};
    pos += 11;
    std::string out;
    while (pos < resp.size() && resp[pos] != '"')
    {
        if (resp[pos] == '\\' && pos + 1 < resp.size())
        {
            out += resp[pos + 1];
            pos += 2;
        }
        else
        {
            out += resp[pos];
            ++pos;
        }
    }
    return out;
}

} // namespace HttpClient
