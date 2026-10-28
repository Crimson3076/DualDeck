#pragma once

// Quotes a string as a JSON string literal: wraps it in double quotes and
// escapes quotes, backslashes and control characters. Bytes >= 0x80 pass
// through unchanged, so UTF-8 text stays UTF-8. Shared by the host's
// control socket replies and the client's `--discover` output.

#include <string>

namespace dualdeck {

std::string jsonQuote(const std::string& value);

} // namespace dualdeck
