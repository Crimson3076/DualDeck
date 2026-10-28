#include <string>

#include "dualdeck/json_string.h"
#include "test_framework.h"

using dualdeck::jsonQuote;

MDR_TEST(json_quote_wraps_plain_text) {
    MDR_CHECK_EQ(jsonQuote("Steam Deck"), std::string("\"Steam Deck\""));
    MDR_CHECK_EQ(jsonQuote(""), std::string("\"\""));
}

MDR_TEST(json_quote_escapes_quotes_backslashes_and_control_characters) {
    MDR_CHECK_EQ(jsonQuote("a\"b\\c"), std::string("\"a\\\"b\\\\c\""));
    MDR_CHECK_EQ(jsonQuote("line\nnext\ttab\r"), std::string("\"line\\nnext\\ttab\\r\""));
    MDR_CHECK_EQ(jsonQuote(std::string("\x01", 1)), std::string("\"\\u0001\""));
}

MDR_TEST(json_quote_leaves_utf8_alone) {
    MDR_CHECK_EQ(jsonQuote("Pok\xc3\xa9mon"), std::string("\"Pok\xc3\xa9mon\""));
}
