/*
 * (C) 2026 see Authors.txt
 *
 * This file is part of MPC-HC.
 *
 * MPC-HC is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * MPC-HC is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "stdafx.h"
#include "LosslessCutProject.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <shlobj.h>

namespace llc
{
    // ---------------------------------------------------------------------------------------------
    // JsonValue

    const JsonValue* JsonValue::Find(const char* key) const
    {
        for (const auto& kv : obj) {
            if (kv.first == key) {
                return &kv.second;
            }
        }
        return nullptr;
    }

    JsonValue* JsonValue::Find(const char* key)
    {
        for (auto& kv : obj) {
            if (kv.first == key) {
                return &kv.second;
            }
        }
        return nullptr;
    }

    void JsonValue::Set(const char* key, JsonValue v)
    {
        if (JsonValue* existing = Find(key)) {
            *existing = std::move(v);
        } else {
            obj.emplace_back(key, std::move(v));
        }
    }

    void JsonValue::Erase(const char* key)
    {
        obj.erase(std::remove_if(obj.begin(), obj.end(), [key](const auto& kv) { return kv.first == key; }), obj.end());
    }

    bool JsonValue::operator==(const JsonValue& o) const
    {
        if (type != o.type) {
            return false;
        }
        switch (type) {
            case Type::Null:
                return true;
            case Type::Bool:
                return b == o.b;
            case Type::Number:
                return num == o.num || (std::isnan(num) && std::isnan(o.num));
            case Type::String:
                return str == o.str;
            case Type::Array:
                return arr == o.arr;
            case Type::Object:
                return obj == o.obj;
        }
        return false;
    }

    // ---------------------------------------------------------------------------------------------
    // UTF-8 helpers

    namespace
    {
        void AppendUtf8(std::string& out, uint32_t cp)
        {
            if (cp < 0x80) {
                out += static_cast<char>(cp);
            } else if (cp < 0x800) {
                out += static_cast<char>(0xC0 | (cp >> 6));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += static_cast<char>(0xE0 | (cp >> 12));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (cp >> 18));
                out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }

        // Decodes the code point at s[i]; the input has been validated as UTF-8 beforehand.
        uint32_t DecodeUtf8(const std::string& s, size_t i, size_t& len)
        {
            const unsigned char c = static_cast<unsigned char>(s[i]);
            if (c < 0x80) {
                len = 1;
                return c;
            }
            if ((c >> 5) == 0x6 && i + 1 < s.size()) {
                len = 2;
                return ((c & 0x1F) << 6) | (s[i + 1] & 0x3F);
            }
            if ((c >> 4) == 0xE && i + 2 < s.size()) {
                len = 3;
                return ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
            }
            if ((c >> 3) == 0x1E && i + 3 < s.size()) {
                len = 4;
                return ((c & 0x07) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
            }
            len = 1;
            return 0xFFFD;
        }

        bool IsValidUtf8(const std::string& s)
        {
            if (s.empty()) {
                return true;
            }
            return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0) > 0;
        }

        bool IsJson5Space(uint32_t cp)
        {
            switch (cp) {
                case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20:
                case 0xA0: case 0x1680: case 0x2028: case 0x2029: case 0x202F: case 0x205F:
                case 0x3000: case 0xFEFF:
                    return true;
                default:
                    return cp >= 0x2000 && cp <= 0x200A;
            }
        }

        bool IsLineTerminator(uint32_t cp)
        {
            return cp == '\n' || cp == '\r' || cp == 0x2028 || cp == 0x2029;
        }

        bool IsAsciiIdStart(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '$' || c == '_';
        }

        bool IsAsciiIdContinue(char c)
        {
            return IsAsciiIdStart(c) || (c >= '0' && c <= '9');
        }

        bool IsDigit(char c)
        {
            return c >= '0' && c <= '9';
        }

        int HexValue(char c)
        {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        }

        // -----------------------------------------------------------------------------------------
        // JSON5 parser

        class Json5Parser
        {
        public:
            explicit Json5Parser(const std::string& s) : m_s(s) {}

            bool Parse(JsonValue& out, std::string& error)
            {
                bool ok = SkipSpace() && ParseValue(out) && SkipSpace();
                if (ok && m_i != m_s.size()) {
                    ok = Fail("unexpected content after the end of the document");
                }
                if (!ok) {
                    error = m_error;
                }
                return ok;
            }

        private:
            const std::string& m_s;
            size_t m_i = 0;
            int m_depth = 0;
            std::string m_error;

            bool AtEnd() const { return m_i >= m_s.size(); }
            char Peek(size_t ahead = 0) const { return m_i + ahead < m_s.size() ? m_s[m_i + ahead] : '\0'; }

            bool Fail(const char* what)
            {
                if (m_error.empty()) {
                    size_t line = 1, col = 1;
                    for (size_t k = 0; k < m_i && k < m_s.size(); k++) {
                        if (m_s[k] == '\n') {
                            line++;
                            col = 1;
                        } else {
                            col++;
                        }
                    }
                    m_error = std::string(what) + " (line " + std::to_string(line) + ", column " + std::to_string(col) + ")";
                }
                return false;
            }

            bool SkipSpace()
            {
                while (!AtEnd()) {
                    const char c = m_s[m_i];
                    if (c == '/' && Peek(1) == '/') {
                        m_i += 2;
                        while (!AtEnd()) {
                            size_t len;
                            if (IsLineTerminator(DecodeUtf8(m_s, m_i, len))) {
                                break;
                            }
                            m_i += len;
                        }
                        continue;
                    }
                    if (c == '/' && Peek(1) == '*') {
                        const size_t endPos = m_s.find("*/", m_i + 2);
                        if (endPos == std::string::npos) {
                            return Fail("unterminated comment");
                        }
                        m_i = endPos + 2;
                        continue;
                    }
                    size_t len;
                    if (IsJson5Space(DecodeUtf8(m_s, m_i, len))) {
                        m_i += len;
                        continue;
                    }
                    break;
                }
                return true;
            }

            bool ParseValue(JsonValue& v)
            {
                if (AtEnd()) {
                    return Fail("unexpected end of document");
                }
                const char c = m_s[m_i];
                switch (c) {
                    case '{':
                        return ParseObject(v);
                    case '[':
                        return ParseArray(v);
                    case '"':
                    case '\'':
                        v.type = JsonValue::Type::String;
                        return ParseString(v.str);
                    default:
                        break;
                }
                if (Literal("true")) {
                    v = JsonValue::MakeBool(true);
                    return true;
                }
                if (Literal("false")) {
                    v = JsonValue::MakeBool(false);
                    return true;
                }
                if (Literal("null")) {
                    v = JsonValue();
                    return true;
                }
                if (c == '-' || c == '+' || c == '.' || c == 'I' || c == 'N' || IsDigit(c)) {
                    return ParseNumber(v);
                }
                return Fail("unexpected character");
            }

            bool Literal(const char* word)
            {
                const size_t n = strlen(word);
                if (m_s.compare(m_i, n, word) != 0) {
                    return false;
                }
                if (m_i + n < m_s.size() && (IsAsciiIdContinue(m_s[m_i + n]) || static_cast<unsigned char>(m_s[m_i + n]) >= 0x80)) {
                    return false;
                }
                m_i += n;
                return true;
            }

            bool Enter()
            {
                if (++m_depth > 256) {
                    return Fail("nesting too deep");
                }
                return true;
            }

            bool ParseObject(JsonValue& v)
            {
                if (!Enter()) {
                    return false;
                }
                v = JsonValue::MakeObject();
                m_i++; // {
                if (!SkipSpace()) {
                    return false;
                }
                if (Peek() == '}') {
                    m_i++;
                    m_depth--;
                    return true;
                }
                for (;;) {
                    std::string key;
                    const char c = Peek();
                    if (c == '"' || c == '\'') {
                        if (!ParseString(key)) {
                            return false;
                        }
                    } else if (!ParseIdentifier(key)) {
                        return false;
                    }
                    if (v.Find(key.c_str())) {
                        return Fail("duplicate key");
                    }
                    if (!SkipSpace()) {
                        return false;
                    }
                    if (Peek() != ':') {
                        return Fail("expected ':'");
                    }
                    m_i++;
                    if (!SkipSpace()) {
                        return false;
                    }
                    JsonValue member;
                    if (!ParseValue(member) || !SkipSpace()) {
                        return false;
                    }
                    v.obj.emplace_back(std::move(key), std::move(member));
                    if (Peek() == ',') {
                        m_i++;
                        if (!SkipSpace()) {
                            return false;
                        }
                        if (Peek() == '}') {
                            m_i++;
                            break;
                        }
                        continue;
                    }
                    if (Peek() == '}') {
                        m_i++;
                        break;
                    }
                    return Fail("expected ',' or '}'");
                }
                m_depth--;
                return true;
            }

            bool ParseArray(JsonValue& v)
            {
                if (!Enter()) {
                    return false;
                }
                v = JsonValue::MakeArray();
                m_i++; // [
                if (!SkipSpace()) {
                    return false;
                }
                if (Peek() == ']') {
                    m_i++;
                    m_depth--;
                    return true;
                }
                for (;;) {
                    JsonValue item;
                    if (!ParseValue(item) || !SkipSpace()) {
                        return false;
                    }
                    v.arr.push_back(std::move(item));
                    if (Peek() == ',') {
                        m_i++;
                        if (!SkipSpace()) {
                            return false;
                        }
                        if (Peek() == ']') {
                            m_i++;
                            break;
                        }
                        continue;
                    }
                    if (Peek() == ']') {
                        m_i++;
                        break;
                    }
                    return Fail("expected ',' or ']'");
                }
                m_depth--;
                return true;
            }

            bool ParseHex4(uint32_t& cp)
            {
                cp = 0;
                for (int k = 0; k < 4; k++) {
                    const int h = HexValue(Peek());
                    if (h < 0) {
                        return Fail("invalid \\u escape");
                    }
                    cp = (cp << 4) | static_cast<uint32_t>(h);
                    m_i++;
                }
                return true;
            }

            bool ParseUnicodeEscape(uint32_t& cp)
            {
                // positioned after "\u"
                if (!ParseHex4(cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    if (Peek() == '\\' && Peek(1) == 'u') {
                        m_i += 2;
                        uint32_t low;
                        if (!ParseHex4(low)) {
                            return false;
                        }
                        if (low < 0xDC00 || low > 0xDFFF) {
                            return Fail("invalid surrogate pair");
                        }
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        return true;
                    }
                    return Fail("unpaired surrogate");
                }
                if (cp >= 0xDC00 && cp <= 0xDFFF) {
                    return Fail("unpaired surrogate");
                }
                return true;
            }

            bool ParseString(std::string& out)
            {
                const char quote = m_s[m_i++];
                out.clear();
                for (;;) {
                    if (AtEnd()) {
                        return Fail("unterminated string");
                    }
                    const char c = m_s[m_i];
                    if (c == quote) {
                        m_i++;
                        return true;
                    }
                    if (c == '\n' || c == '\r') {
                        return Fail("unterminated string");
                    }
                    if (c != '\\') {
                        size_t len;
                        DecodeUtf8(m_s, m_i, len);
                        out.append(m_s, m_i, len);
                        m_i += len;
                        continue;
                    }
                    m_i++; // backslash
                    if (AtEnd()) {
                        return Fail("unterminated string");
                    }
                    const char e = m_s[m_i];
                    switch (e) {
                        case 'b': out += '\b'; m_i++; break;
                        case 'f': out += '\f'; m_i++; break;
                        case 'n': out += '\n'; m_i++; break;
                        case 'r':
                            out += '\r';
                            m_i++;
                            break;
                        case 't': out += '\t'; m_i++; break;
                        case 'v': out += '\v'; m_i++; break;
                        case '0':
                            m_i++;
                            if (IsDigit(Peek())) {
                                return Fail("octal escapes are not allowed");
                            }
                            out += '\0';
                            break;
                        case 'x': {
                            m_i++;
                            const int h1 = HexValue(Peek());
                            const int h2 = HexValue(Peek(1));
                            if (h1 < 0 || h2 < 0) {
                                return Fail("invalid \\x escape");
                            }
                            m_i += 2;
                            AppendUtf8(out, static_cast<uint32_t>(h1 * 16 + h2));
                            break;
                        }
                        case 'u': {
                            m_i++;
                            uint32_t cp;
                            if (!ParseUnicodeEscape(cp)) {
                                return false;
                            }
                            AppendUtf8(out, cp);
                            break;
                        }
                        case '\n':
                            m_i++;
                            break; // line continuation
                        case '\r':
                            m_i++;
                            if (Peek() == '\n') {
                                m_i++;
                            }
                            break;
                        default: {
                            if (e >= '1' && e <= '9') {
                                return Fail("invalid escape");
                            }
                            size_t len;
                            const uint32_t cp = DecodeUtf8(m_s, m_i, len);
                            if (cp != 0x2028 && cp != 0x2029) {
                                out.append(m_s, m_i, len); // any other escaped character is itself
                            }
                            m_i += len;
                            break;
                        }
                    }
                }
            }

            bool ParseIdentifier(std::string& out)
            {
                out.clear();
                bool first = true;
                while (!AtEnd()) {
                    const char c = m_s[m_i];
                    if (c == '\\') {
                        if (Peek(1) != 'u') {
                            return Fail("invalid identifier escape");
                        }
                        m_i += 2;
                        uint32_t cp;
                        if (!ParseUnicodeEscape(cp)) {
                            return false;
                        }
                        AppendUtf8(out, cp);
                    } else if (static_cast<unsigned char>(c) < 0x80) {
                        if (first ? !IsAsciiIdStart(c) : !IsAsciiIdContinue(c)) {
                            break;
                        }
                        out += c;
                        m_i++;
                    } else {
                        size_t len;
                        const uint32_t cp = DecodeUtf8(m_s, m_i, len);
                        if (IsJson5Space(cp)) {
                            break;
                        }
                        out.append(m_s, m_i, len);
                        m_i += len;
                    }
                    first = false;
                }
                if (out.empty()) {
                    return Fail("expected a property name");
                }
                return true;
            }

            bool ParseNumber(JsonValue& v)
            {
                bool negative = false;
                if (Peek() == '+' || Peek() == '-') {
                    negative = (Peek() == '-');
                    m_i++;
                }
                double value = 0.0;
                if (Literal("Infinity")) {
                    value = HUGE_VAL;
                } else if (Literal("NaN")) {
                    value = std::nan("");
                } else if (Peek() == '0' && (Peek(1) == 'x' || Peek(1) == 'X')) {
                    m_i += 2;
                    if (HexValue(Peek()) < 0) {
                        return Fail("invalid hexadecimal number");
                    }
                    while (HexValue(Peek()) >= 0) {
                        value = value * 16.0 + HexValue(Peek());
                        m_i++;
                    }
                } else {
                    const size_t begin = m_i;
                    bool digits = false;
                    if (Peek() == '0') {
                        m_i++;
                        digits = true;
                        if (IsDigit(Peek())) {
                            return Fail("leading zeros are not allowed");
                        }
                    } else {
                        while (IsDigit(Peek())) {
                            m_i++;
                            digits = true;
                        }
                    }
                    if (Peek() == '.') {
                        m_i++;
                        while (IsDigit(Peek())) {
                            m_i++;
                            digits = true;
                        }
                    }
                    if (!digits) {
                        return Fail("invalid number");
                    }
                    if (Peek() == 'e' || Peek() == 'E') {
                        m_i++;
                        if (Peek() == '+' || Peek() == '-') {
                            m_i++;
                        }
                        if (!IsDigit(Peek())) {
                            return Fail("invalid exponent");
                        }
                        while (IsDigit(Peek())) {
                            m_i++;
                        }
                    }
                    // from_chars does not accept a leading '.' or a trailing '.', so normalize.
                    std::string lexeme = m_s.substr(begin, m_i - begin);
                    if (lexeme[0] == '.') {
                        lexeme.insert(lexeme.begin(), '0');
                    }
                    const size_t dot = lexeme.find('.');
                    if (dot != std::string::npos && (dot + 1 == lexeme.size() || !IsDigit(lexeme[dot + 1]))) {
                        lexeme.insert(dot + 1, "0");
                    }
                    const auto res = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), value);
                    if (res.ec == std::errc::result_out_of_range) {
                        value = HUGE_VAL; // JS semantics for overflow
                    } else if (res.ec != std::errc() || res.ptr != lexeme.data() + lexeme.size()) {
                        return Fail("invalid number");
                    }
                }
                if (!AtEnd() && (IsAsciiIdContinue(Peek()) || static_cast<unsigned char>(Peek()) >= 0x80)) {
                    return Fail("invalid number");
                }
                v = JsonValue::MakeNumber(negative ? -value : value);
                return true;
            }
        };

        // -----------------------------------------------------------------------------------------
        // JSON5 stringify (json5@2 with space = 2)

        std::string QuoteString(const std::string& value)
        {
            double singleQuotes = 0.1, doubleQuotes = 0.2;
            std::string product;
            product.reserve(value.size() + 2);
            for (size_t i = 0; i < value.size(); i++) {
                const char c = value[i];
                switch (c) {
                    case '\'':
                        singleQuotes++;
                        product += c;
                        continue;
                    case '"':
                        doubleQuotes++;
                        product += c;
                        continue;
                    case '\\': product += "\\\\"; continue;
                    case '\b': product += "\\b"; continue;
                    case '\f': product += "\\f"; continue;
                    case '\n': product += "\\n"; continue;
                    case '\r': product += "\\r"; continue;
                    case '\t': product += "\\t"; continue;
                    case '\v': product += "\\v"; continue;
                    case '\0':
                        product += (i + 1 < value.size() && IsDigit(value[i + 1])) ? "\\x00" : "\\0";
                        continue;
                    default:
                        break;
                }
                const unsigned char uc = static_cast<unsigned char>(c);
                if (uc < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    product += "\\x";
                    product += hex[uc >> 4];
                    product += hex[uc & 0xF];
                    continue;
                }
                if (uc == 0xE2 && i + 2 < value.size() && static_cast<unsigned char>(value[i + 1]) == 0x80 &&
                        (static_cast<unsigned char>(value[i + 2]) == 0xA8 || static_cast<unsigned char>(value[i + 2]) == 0xA9)) {
                    product += (static_cast<unsigned char>(value[i + 2]) == 0xA8) ? "\\u2028" : "\\u2029";
                    i += 2;
                    continue;
                }
                product += c;
            }
            const char quoteChar = (singleQuotes < doubleQuotes) ? '\'' : '"';
            std::string result;
            result.reserve(product.size() + 2);
            result += quoteChar;
            for (char c : product) {
                if (c == quoteChar) {
                    result += '\\';
                }
                result += c;
            }
            result += quoteChar;
            return result;
        }

        std::string SerializeKey(const std::string& key)
        {
            if (key.empty() || !IsAsciiIdStart(key[0])) {
                return QuoteString(key);
            }
            for (char c : key) {
                if (!IsAsciiIdContinue(c)) {
                    return QuoteString(key);
                }
            }
            return key;
        }

        void Serialize(const JsonValue& v, std::string& out, const std::string& indent)
        {
            switch (v.type) {
                case JsonValue::Type::Null:
                    out += "null";
                    break;
                case JsonValue::Type::Bool:
                    out += v.b ? "true" : "false";
                    break;
                case JsonValue::Type::Number:
                    out += FormatJsNumber(v.num);
                    break;
                case JsonValue::Type::String:
                    out += QuoteString(v.str);
                    break;
                case JsonValue::Type::Array: {
                    if (v.arr.empty()) {
                        out += "[]";
                        break;
                    }
                    const std::string inner = indent + "  ";
                    out += "[\n";
                    for (const auto& item : v.arr) {
                        out += inner;
                        Serialize(item, out, inner);
                        out += ",\n";
                    }
                    out += indent;
                    out += "]";
                    break;
                }
                case JsonValue::Type::Object: {
                    if (v.obj.empty()) {
                        out += "{}";
                        break;
                    }
                    const std::string inner = indent + "  ";
                    out += "{\n";
                    for (const auto& kv : v.obj) {
                        out += inner;
                        out += SerializeKey(kv.first);
                        out += ": ";
                        Serialize(kv.second, out, inner);
                        out += ",\n";
                    }
                    out += indent;
                    out += "}";
                    break;
                }
            }
        }
    }

    bool ParseJson5(const std::string& utf8, JsonValue& out, std::string& error)
    {
        if (!IsValidUtf8(utf8)) {
            error = "the file is not valid UTF-8";
            return false;
        }
        Json5Parser parser(utf8);
        return parser.Parse(out, error);
    }

    std::string StringifyJson5(const JsonValue& value)
    {
        std::string out;
        Serialize(value, out, "");
        return out;
    }

    std::string FormatJsNumber(double v)
    {
        if (std::isnan(v)) {
            return "NaN";
        }
        if (std::isinf(v)) {
            return v < 0 ? "-Infinity" : "Infinity";
        }
        if (v == 0.0) {
            return "0"; // also -0
        }
        char buf[64];
        const auto res = std::to_chars(buf, buf + sizeof(buf), std::fabs(v), std::chars_format::scientific);
        const std::string sci(buf, res.ptr);
        const size_t ePos = sci.find('e');
        std::string digits;
        for (size_t k = 0; k < ePos; k++) {
            if (sci[k] != '.') {
                digits += sci[k];
            }
        }
        const int exponent = std::stoi(sci.substr(ePos + 1));
        const int k = static_cast<int>(digits.size());
        const int n = exponent + 1;

        std::string s;
        if (k <= n && n <= 21) {
            s = digits + std::string(n - k, '0');
        } else if (0 < n && n <= 21) {
            s = digits.substr(0, n) + "." + digits.substr(n);
        } else if (-6 < n && n <= 0) {
            s = "0." + std::string(-n, '0') + digits;
        } else {
            s = digits.substr(0, 1);
            if (k > 1) {
                s += "." + digits.substr(1);
            }
            s += (n - 1 >= 0) ? "e+" : "e-";
            s += std::to_string(std::abs(n - 1));
        }
        return v < 0 ? "-" + s : s;
    }

    // ---------------------------------------------------------------------------------------------
    // Project

    std::wstring Project::ProjectPathForMedia(const std::wstring& mediaPath)
    {
        const size_t slash = mediaPath.find_last_of(L"\\/");
        const std::wstring dir = (slash == std::wstring::npos) ? L"" : mediaPath.substr(0, slash + 1);
        const std::wstring base = (slash == std::wstring::npos) ? mediaPath : mediaPath.substr(slash + 1);
        // Node's path.parse(): a dot at position 0 starts a hidden name, not an extension.
        const size_t dot = base.find_last_of(L'.');
        const std::wstring stem = (dot == std::wstring::npos || dot == 0) ? base : base.substr(0, dot);
        return dir + stem + L"-proj.llc";
    }

    void Project::InitNew(const std::wstring& mediaPath)
    {
        const size_t slash = mediaPath.find_last_of(L"\\/");
        const std::wstring base = (slash == std::wstring::npos) ? mediaPath : mediaPath.substr(slash + 1);
        m_doc = JsonValue::MakeObject();
        m_doc.Set("version", JsonValue::MakeNumber(2));
        m_doc.Set("mediaFileName", JsonValue::MakeString(WideToUtf8(base)));
        m_doc.Set("cutSegments", JsonValue::MakeArray());
        segments.clear();
        upgradedFromV1 = false;
    }

    bool Project::ExtractSegments(const JsonValue& doc, std::vector<Segment>& out, int& version, std::string& error)
    {
        out.clear();
        if (doc.type != JsonValue::Type::Object) {
            error = "the project is not an object";
            return false;
        }
        const JsonValue* ver = doc.Find("version");
        if (!ver || ver->type != JsonValue::Type::Number) {
            error = "missing version";
            return false;
        }
        if (ver->num != 1.0 && ver->num != 2.0) {
            version = static_cast<int>(ver->num);
            error = "unsupported project version";
            return false;
        }
        version = static_cast<int>(ver->num);

        const JsonValue* media = doc.Find("mediaFileName");
        if (media && media->type != JsonValue::Type::String) {
            error = "mediaFileName is not a string";
            return false;
        }
        const JsonValue* segs = doc.Find("cutSegments");
        if (!segs || segs->type != JsonValue::Type::Array) {
            error = "cutSegments is missing or not a list";
            return false;
        }

        for (const JsonValue& item : segs->arr) {
            if (item.type != JsonValue::Type::Object) {
                error = "a segment is not an object";
                return false;
            }
            Segment seg;
            seg.raw = item;

            const JsonValue* start = item.Find("start");
            if (start) {
                if (start->type != JsonValue::Type::Number || !std::isfinite(start->num)) {
                    error = "a segment has an invalid start";
                    return false;
                }
                seg.start = start->num;
            } else if (version >= 2) {
                error = "a segment has no start";
                return false;
            }

            const JsonValue* end = item.Find("end");
            if (end) {
                if (end->type != JsonValue::Type::Number || !std::isfinite(end->num)) {
                    error = "a segment has an invalid end";
                    return false;
                }
                seg.end = end->num;
            }

            const JsonValue* name = item.Find("name");
            if (!name || name->type != JsonValue::Type::String) {
                error = "a segment has no name";
                return false;
            }
            seg.name = name->str;

            const JsonValue* selected = item.Find("selected");
            if (selected) {
                if (selected->type != JsonValue::Type::Bool) {
                    error = "a segment has an invalid selected flag";
                    return false;
                }
                seg.selected = selected->b;
            }
            out.push_back(std::move(seg));
        }
        return true;
    }

    Project::LoadStatus Project::Load(const std::string& bytes, std::string& error)
    {
        JsonValue doc;
        if (!ParseJson5(bytes, doc, error)) {
            return LoadStatus::ParseError;
        }
        std::vector<Segment> segs;
        int version = 0;
        if (!ExtractSegments(doc, segs, version, error)) {
            return (version > 2) ? LoadStatus::UnsupportedVersion : LoadStatus::InvalidSchema;
        }
        m_doc = std::move(doc);
        segments = std::move(segs);
        upgradedFromV1 = (version == 1);
        return LoadStatus::Ok;
    }

    namespace
    {
        // Updates a segment key in place, or inserts a missing one where LosslessCut would put it
        // (its saved key order is start, end, name, tags, selected).
        void SetSegmentKey(JsonValue& o, const char* key, JsonValue v)
        {
            if (JsonValue* existing = o.Find(key)) {
                *existing = std::move(v);
                return;
            }
            static const char* const order[] = { "start", "end", "name", "tags", "selected" };
            size_t insertAt = 0;
            for (const char* k : order) {
                if (strcmp(k, key) == 0) {
                    break;
                }
                for (size_t idx = 0; idx < o.obj.size(); idx++) {
                    if (o.obj[idx].first == k) {
                        insertAt = (std::max)(insertAt, idx + 1);
                    }
                }
            }
            o.obj.emplace(o.obj.begin() + insertAt, key, std::move(v));
        }
    }

    std::string Project::Serialize() const
    {
        JsonValue doc = m_doc;
        doc.Set("version", JsonValue::MakeNumber(2));
        JsonValue list = JsonValue::MakeArray();
        for (const Segment& seg : segments) {
            JsonValue o = (seg.raw.type == JsonValue::Type::Object) ? seg.raw : JsonValue::MakeObject();
            SetSegmentKey(o, "start", JsonValue::MakeNumber(seg.start));
            if (seg.end) {
                SetSegmentKey(o, "end", JsonValue::MakeNumber(*seg.end));
            } else {
                o.Erase("end");
            }
            SetSegmentKey(o, "name", JsonValue::MakeString(seg.name));
            SetSegmentKey(o, "selected", JsonValue::MakeBool(seg.selected));
            list.arr.push_back(std::move(o));
        }
        doc.Set("cutSegments", std::move(list));
        return StringifyJson5(doc);
    }

    bool Project::VerifySerialized(const std::string& bytes, std::string& error) const
    {
        JsonValue doc;
        if (!ParseJson5(bytes, doc, error)) {
            error = "self-check failed, the written data does not parse: " + error;
            return false;
        }
        std::vector<Segment> check;
        int version = 0;
        if (!ExtractSegments(doc, check, version, error) || version != 2) {
            error = "self-check failed, the written data is not a valid project: " + error;
            return false;
        }
        if (check.size() != segments.size()) {
            error = "self-check failed, segment count mismatch";
            return false;
        }
        for (size_t k = 0; k < check.size(); k++) {
            if (!check[k].SameContent(segments[k])) {
                error = "self-check failed, segment " + std::to_string(k + 1) + " differs";
                return false;
            }
        }
        // Everything outside cutSegments must come through untouched as well.
        JsonValue expected = m_doc;
        expected.Set("version", JsonValue::MakeNumber(2));
        expected.Erase("cutSegments");
        doc.Erase("cutSegments");
        if (!(doc == expected)) {
            error = "self-check failed, project properties differ";
            return false;
        }
        return true;
    }

    // ---------------------------------------------------------------------------------------------
    // File IO

    std::wstring Utf8ToWide(const std::string& s)
    {
        if (s.empty()) {
            return {};
        }
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
        return w;
    }

    std::string WideToUtf8(const std::wstring& w)
    {
        if (w.empty()) {
            return {};
        }
        const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string s(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
        return s;
    }

    namespace FileIO
    {
        namespace
        {
            std::wstring LastErrorText(DWORD err)
            {
                LPWSTR buf = nullptr;
                FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
                std::wstring text = buf ? buf : L"";
                if (buf) {
                    LocalFree(buf);
                }
                while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ' || text.back() == L'.')) {
                    text.pop_back();
                }
                return text + L" (" + std::to_wstring(err) + L")";
            }

            bool Exists(const std::wstring& path)
            {
                return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
            }
        }

        Stamp GetStamp(const std::wstring& path)
        {
            Stamp st;
            WIN32_FILE_ATTRIBUTE_DATA fad;
            if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) && !(fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                st.exists = true;
                st.size = (static_cast<ULONGLONG>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
                st.lastWrite = fad.ftLastWriteTime;
            }
            return st;
        }

        bool ReadAll(const std::wstring& path, std::string& out, DWORD& winError)
        {
            out.clear();
            winError = ERROR_SUCCESS;
            HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                winError = GetLastError();
                return false;
            }
            LARGE_INTEGER size;
            bool ok = !!GetFileSizeEx(h, &size);
            if (ok && size.QuadPart > 64LL * 1024 * 1024) {
                winError = ERROR_FILE_TOO_LARGE;
                ok = false;
            }
            if (ok) {
                out.resize(static_cast<size_t>(size.QuadPart));
                size_t done = 0;
                while (done < out.size()) {
                    DWORD read = 0;
                    if (!ReadFile(h, &out[done], static_cast<DWORD>(std::min<size_t>(out.size() - done, 1 << 20)), &read, nullptr)) {
                        winError = GetLastError();
                        ok = false;
                        break;
                    }
                    if (read == 0) {
                        out.resize(done); // file shrank while reading
                        break;
                    }
                    done += read;
                }
            } else if (winError == ERROR_SUCCESS) {
                winError = GetLastError();
            }
            CloseHandle(h);
            if (!ok) {
                out.clear();
            }
            return ok;
        }

        bool AtomicWrite(const std::wstring& path, const std::string& bytes, std::wstring& error)
        {
            const std::wstring tmp = path + L".mpchc-tmp";

            HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                error = L"cannot create a temporary file: " + LastErrorText(GetLastError());
                return false;
            }
            bool ok = true;
            size_t done = 0;
            while (ok && done < bytes.size()) {
                DWORD written = 0;
                if (!WriteFile(h, bytes.data() + done, static_cast<DWORD>(std::min<size_t>(bytes.size() - done, 1 << 20)), &written, nullptr) || written == 0) {
                    error = L"cannot write the temporary file: " + LastErrorText(GetLastError());
                    ok = false;
                }
                done += written;
            }
            if (ok && !FlushFileBuffers(h)) {
                error = L"cannot flush the temporary file: " + LastErrorText(GetLastError());
                ok = false;
            }
            CloseHandle(h);
            if (!ok) {
                DeleteFileW(tmp.c_str());
                return false;
            }

            if (Exists(path)) {
                DWORD lastErr = ERROR_SUCCESS;
                bool replaced = false;
                for (int attempt = 0; attempt < 5 && !replaced; attempt++) {
                    if (attempt) {
                        Sleep(60); // another program (LosslessCut) may have the file open for a moment
                    }
                    replaced = !!ReplaceFileW(path.c_str(), tmp.c_str(), nullptr,
                                              REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS, nullptr, nullptr);
                    if (!replaced) {
                        lastErr = GetLastError();
                        if (!Exists(path) && Exists(tmp)) {
                            break; // the target is gone, put the new content in place below
                        }
                    }
                }
                if (!replaced) {
                    // Some file systems don't implement ReplaceFile; a rename over the target is still atomic.
                    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                        error = L"cannot replace the project file: " + LastErrorText(lastErr != ERROR_SUCCESS ? lastErr : GetLastError());
                        DeleteFileW(tmp.c_str());
                        return false;
                    }
                }
            } else if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
                error = L"cannot create the project file: " + LastErrorText(GetLastError());
                DeleteFileW(tmp.c_str());
                return false;
            }

            std::string check;
            DWORD readErr;
            if (!ReadAll(path, check, readErr)) {
                error = L"cannot read the project file back: " + LastErrorText(readErr);
                return false;
            }
            if (check != bytes) {
                error = L"the project file did not read back identically (another program may have written it)";
                return false;
            }
            return true;
        }

        bool Backup(const std::wstring& path, const std::wstring& backupDir, int maxKeep, std::wstring& error)
        {
            if (!Exists(path)) {
                return true;
            }
            const int dirRes = SHCreateDirectoryExW(nullptr, backupDir.c_str(), nullptr);
            if (dirRes != ERROR_SUCCESS && dirRes != ERROR_ALREADY_EXISTS && dirRes != ERROR_FILE_EXISTS) {
                error = L"cannot create the backup folder: " + LastErrorText(dirRes);
                return false;
            }

            const size_t slash = path.find_last_of(L"\\/");
            const std::wstring dir = (slash == std::wstring::npos) ? L"" : path.substr(0, slash);
            std::wstring name = (slash == std::wstring::npos) ? path : path.substr(slash + 1);
            if (name.size() > 4 && _wcsicmp(name.c_str() + name.size() - 4, L".llc") == 0) {
                name.resize(name.size() - 4);
            }
            // The folder hash keeps same-named projects from different folders apart.
            ULONG hash = 2166136261u;
            for (wchar_t c : dir) {
                hash = (hash ^ static_cast<ULONG>(towlower(c))) * 16777619u;
            }
            wchar_t hashText[16];
            swprintf_s(hashText, L"%08lx", hash);
            const std::wstring prefix = name + L"." + hashText + L".";

            SYSTEMTIME st;
            GetLocalTime(&st);
            wchar_t stamp[32];
            swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            const std::wstring dest = backupDir + L"\\" + prefix + stamp + L".llc";
            if (!CopyFileW(path.c_str(), dest.c_str(), FALSE)) {
                error = L"cannot back up the project file: " + LastErrorText(GetLastError());
                return false;
            }

            std::vector<std::wstring> existing;
            WIN32_FIND_DATAW fd;
            HANDLE hFind = FindFirstFileW((backupDir + L"\\" + prefix + L"*.llc").c_str(), &fd);
            if (hFind != INVALID_HANDLE_VALUE) {
                do {
                    existing.emplace_back(fd.cFileName);
                } while (FindNextFileW(hFind, &fd));
                FindClose(hFind);
            }
            std::sort(existing.begin(), existing.end());
            for (size_t k = 0; k + maxKeep < existing.size(); k++) {
                DeleteFileW((backupDir + L"\\" + existing[k]).c_str());
            }
            return true;
        }
    }
}
