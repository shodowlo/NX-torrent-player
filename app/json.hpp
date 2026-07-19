#pragma once

#include <string>
#include <vector>

// A JSON reader sized to what this app receives: flat "key": value lookups and
// arrays of objects, from APIs whose shape we already know (Stremio, GitHub).
// It is deliberately not a parser -- it validates nothing and builds no model of
// the document -- but it keeps a JSON library out of a build that has to cross-
// compile for the Switch.
//
// The trade-off to remember: a lookup finds the FIRST match anywhere in the
// text, nesting included. Scope the search with `from`, or pull the object out
// with objects() first, whenever a key could appear more than once.
namespace json
{

// The string at `key`, decoding escapes (including \uXXXX and surrogate pairs)
// to UTF-8. "" if absent. `from` scopes the search.
std::string str(const std::string& body, const char* key, size_t from = 0);

// The number at `key`, or `dflt`.
long integer(const std::string& body, const char* key, long dflt = 0);

// The bool at `key`, or `dflt`. Tolerates whitespace after the colon.
bool boolean(const std::string& body, const char* key, bool dflt = false);

// Escapes a value for embedding in a JSON string literal.
std::string escape(const std::string& s);

// The top-level objects of the array at `key`, each as its own text, split by
// brace depth (so nested objects are not mistaken for items).
std::vector<std::string> objects(const std::string& body, const char* key);

// The plain strings of an array like "types":["movie","series"].
std::vector<std::string> strings(const std::string& body, const char* key);

} // namespace json
