// json.h - a small JSON value, parser and writer for the on-screen controls'
// layout files. No dependency beyond the standard library, so it links into
// SDL-free unit tests. Object keys keep insertion order for stable output.
#pragma once

#include <string>
#include <utility>
#include <vector>

namespace controls {

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::pair<std::string, Json>> o; // insertion order kept for stable output

    // The member's value, or nullptr if this is not an object or the key is absent.
    const Json *get(const char *key) const;
    // A member read as a number, or fallback if absent or not a number.
    double num(const char *key, double fallback) const;
    // A member read as a string, or fallback if absent or not a string.
    std::string str(const char *key, const char *fallback) const;
    // A member read as a bool, or fallback if absent or not a bool.
    bool boolean(const char *key, bool fallback) const;
    // Sets a member, replacing it in place if already present, else appending.
    Json &set(const std::string &key, Json v);

    static Json number(double v);
    static Json string(std::string v);
    static Json boolean_value(bool v);
    static Json array();
    static Json object();
};

// Parses `text` into `*out`. On success, returns true and leaves `*error`
// untouched. On failure, `*out` becomes Null, `*error` becomes
// "line N: <what>", and returns false.
bool json_parse(const std::string &text, Json *out, std::string *error);

// Writes `v` as indented JSON text (2 spaces per `indent` levels by default).
std::string json_write(const Json &v, int indent = 2);

} // namespace controls
