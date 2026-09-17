// controls_tests.cpp - the on-screen controls: json, layouts, router, pad, binding, editor.
#include "../controls/json.h"

#include <cmath>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(c)                                                                                   \
    do {                                                                                           \
        if (!(c)) {                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                           \
            ++g_failures;                                                                          \
        }                                                                                          \
    } while (0)

using namespace controls;

static void test_json_round_trip() {
    Json v;
    std::string err;
    CHECK(json_parse(R"({"a": 1, "b": [true, false, null], "c": "x\"y\\né", "d": -2.5e1})", &v,
                     &err));
    CHECK(v.type == Json::Object);
    CHECK(v.num("a", 0) == 1);
    CHECK(v.get("b") && v.get("b")->a.size() == 3 && v.get("b")->a[0].b);
    CHECK(v.str("c", "") == "x\"y\\n\xc3\xa9");
    CHECK(v.num("d", 0) == -25);
    Json again;
    CHECK(json_parse(json_write(v), &again, &err));
    CHECK(json_write(again) == json_write(v));
    CHECK(json_write(Json::number(3)) == "3");
}

static void test_json_errors_name_the_line() {
    Json v;
    std::string err;
    CHECK(!json_parse("{\n\"a\": 1,\n\"b\" 2}", &v, &err));
    CHECK(err.rfind("line 3:", 0) == 0);
    CHECK(!json_parse("[1, 2", &v, &err));
    CHECK(!json_parse("{} trailing", &v, &err));
    CHECK(v.num("missing", 7) == 7); // a failed parse leaves a usable value
}

int main() {
    test_json_round_trip();
    test_json_errors_name_the_line();
    if (g_failures) {
        fprintf(stderr, "%d failures\n", g_failures);
        return 1;
    }
    printf("controls_tests: all passed\n");
    return 0;
}
