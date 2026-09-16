#pragma once

// Small string/file helpers shared across modules.

#include <string>

// UTF-8 <-> UTF-16 conversions (Win32 WideCharToMultiByte wrappers).
std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& ws);

// Strips leading/trailing spaces and tabs.
std::string Trim(const std::string& s);

// Reads a whole file in binary mode. Returns false when it cannot be opened
// or exceeds maxBytes.
bool ReadFileBytes(const std::string& path, size_t maxBytes, std::string& out);

// Percent-encodes everything except unreserved URI characters, so the result
// is safe to embed in href attributes.
std::string UriEncode(const std::string& s);

// Escapes &, <, > and " for HTML text and attribute contexts.
std::string HtmlEscape(const std::string& s);
