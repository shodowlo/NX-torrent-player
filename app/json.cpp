#include "json.hpp"

#include <cstring>

// A JSON reader sized to what this app actually receives: flat "key": value
// lookups and arrays of objects, from APIs whose shape we know (Stremio,
// GitHub). It is not a parser -- it does no validation and has no model of the
// document -- but it keeps a real JSON library out of a build that cross-
// compiles for the Switch.

namespace json
{

std::string str(const std::string& body, const char* key, size_t from)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t k        = body.find(pat, from);
    if (k == std::string::npos) return "";
    size_t colon = body.find(':', k + pat.size());
    if (colon == std::string::npos) return "";
    size_t q1 = body.find('"', colon);
    if (q1 == std::string::npos) return "";

    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    std::string out;
    for (size_t i = q1 + 1; i < body.size(); i++)
    {
        if (body[i] == '"') break;
        if (body[i] != '\\' || i + 1 >= body.size())
        {
            out += body[i];
            continue;
        }

        char esc = body[++i];
        switch (esc)
        {
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u':
            {
                // \uXXXX. Previously the backslash was dropped and the escape
                // letter kept verbatim, so "&" (sent as &) surfaced as the
                // literal text "u0026".
                if (i + 4 >= body.size()) { out += esc; break; }
                int cp = 0, ok = 1;
                for (int d = 1; d <= 4; d++)
                {
                    int h = hex(body[i + d]);
                    if (h < 0) { ok = 0; break; }
                    cp = cp * 16 + h;
                }
                if (!ok) { out += esc; break; }
                i += 4;

                // Surrogate pair: the high half alone is not a character.
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 < body.size() &&
                    body[i + 1] == '\\' && body[i + 2] == 'u')
                {
                    int lo = 0, ok2 = 1;
                    for (int d = 3; d <= 6; d++)
                    {
                        int h = hex(body[i + d]);
                        if (h < 0) { ok2 = 0; break; }
                        lo = lo * 16 + h;
                    }
                    if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF)
                    {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }

                // Encode as UTF-8; the UI's text stack expects it.
                if (cp < 0x80)
                    out += (char)cp;
                else if (cp < 0x800)
                {
                    out += (char)(0xC0 | (cp >> 6));
                    out += (char)(0x80 | (cp & 0x3F));
                }
                else if (cp < 0x10000)
                {
                    out += (char)(0xE0 | (cp >> 12));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                }
                else
                {
                    out += (char)(0xF0 | (cp >> 18));
                    out += (char)(0x80 | ((cp >> 12) & 0x3F));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: out += esc; break;  // \" \\ \/ and anything unknown
        }
    }
    return out;
}

// A literal find of "\"key\":true" silently fails the moment a server pretty-
// prints, and a filter that never matches is invisible -- hence the whitespace
// skip.
bool boolean(const std::string& body, const char* key, bool dflt)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t k        = body.find(pat);
    if (k == std::string::npos) return dflt;
    size_t colon = body.find(':', k + pat.size());
    if (colon == std::string::npos) return dflt;
    size_t v = colon + 1;
    while (v < body.size() && (body[v] == ' ' || body[v] == '\t')) v++;
    return body.compare(v, 4, "true") == 0;
}

std::string escape(const std::string& s)
{
    std::string out;
    for (char c : s)
    {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

long integer(const std::string& body, const char* key, long dflt)
{
    std::string pat = std::string("\"") + key + "\"";
    size_t k        = body.find(pat);
    if (k == std::string::npos) return dflt;
    size_t colon = body.find(':', k + pat.size());
    if (colon == std::string::npos) return dflt;
    size_t v = colon + 1;
    while (v < body.size() && (body[v] == ' ' || body[v] == '\t')) v++;
    if (v >= body.size()) return dflt;
    bool neg = body[v] == '-';
    if (neg) v++;
    if (v >= body.size() || body[v] < '0' || body[v] > '9') return dflt;
    long n = 0;
    while (v < body.size() && body[v] >= '0' && body[v] <= '9')
        n = n * 10 + (body[v++] - '0');
    return neg ? -n : n;
}

// Splits the top-level objects out of the array at `key` by brace depth, so each
// item can be read on its own. Depth-counting (rather than searching for keys
// across the whole body) is what keeps nested objects -- "state", "behaviorHints"
// -- from being mistaken for items, and it skips braces inside strings.
std::vector<std::string> objects(const std::string& body, const char* key)
{
    std::vector<std::string> out;
    std::string pat = std::string("\"") + key + "\"";
    size_t r        = body.find(pat);
    if (r == std::string::npos) return out;
    size_t open = body.find('[', r);
    if (open == std::string::npos) return out;

    int depth      = 0;
    bool inStr     = false;
    size_t objFrom = std::string::npos;
    for (size_t i = open + 1; i < body.size(); i++)
    {
        char c = body[i];
        if (inStr)
        {
            if (c == '\\') { i++; continue; }
            if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '{')
        {
            if (depth == 0) objFrom = i;
            depth++;
        }
        else if (c == '}')
        {
            depth--;
            if (depth == 0 && objFrom != std::string::npos)
            {
                out.push_back(body.substr(objFrom, i - objFrom + 1));
                objFrom = std::string::npos;
            }
        }
        else if (c == ']' && depth == 0)
        {
            break;  // end of the result array
        }
    }
    return out;
}

// Collects the plain strings of a JSON array like "types":["movie","series"].
std::vector<std::string> strings(const std::string& body, const char* key)
{
    std::vector<std::string> out;
    std::string pat = std::string("\"") + key + "\"";
    size_t k        = body.find(pat);
    if (k == std::string::npos) return out;
    size_t open = body.find('[', k);
    if (open == std::string::npos) return out;
    bool inStr = false;
    std::string cur;
    for (size_t i = open + 1; i < body.size(); i++)
    {
        char c = body[i];
        if (inStr)
        {
            if (c == '\\' && i + 1 < body.size()) { cur += body[++i]; continue; }
            if (c == '"') { out.push_back(cur); cur.clear(); inStr = false; continue; }
            cur += c;
        }
        else if (c == '"') inStr = true;
        else if (c == ']') break;
        else if (c == '{' || c == '[') return out;  // not a flat string array
    }
    return out;
}

} // namespace json
