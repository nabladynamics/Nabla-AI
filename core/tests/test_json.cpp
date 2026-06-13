#include <string>

#include <catch2/catch_test_macros.hpp>

#include "nabla/io/json.hpp"

TEST_CASE("JSON parser handles objects, arrays, numbers, bools", "[json]") {
  const auto v = nabla::json::parse(
      R"({"name":"case","re":500,"flag":true,"arr":[1,2,3],"nested":{"x":-1.5e2}})");
  REQUIRE(v.isObject());
  REQUIRE(v.string("name", "") == "case");
  REQUIRE(v.number("re", 0) == 500.0);
  REQUIRE(v.boolean("flag", false));
  REQUIRE(v.find("arr")->arr.size() == 3);
  REQUIRE(v.find("nested")->number("x", 0) == -150.0);
}

TEST_CASE("JSON backslash-u escapes decode to UTF-8 (paths with non-ASCII)", "[json]") {
  // Python's json.dumps escapes non-ASCII by default; filesystem paths such as
  // "Marti" + combining acute arrive as backslash-u escapes. Regression: these
  // used to be passed through literally, mangling solver run directories.
  const std::string bs = "\\";  // single backslash, kept out of literals on purpose

  // u00ed -> i-acute (UTF-8 C3 AD)
  const auto a = nabla::json::parse("{\"p\":\"Mart" + bs + "u00ed\"}");
  REQUIRE(a.string("p", "") == "Mart\xc3\xad");

  // 'i' + combining acute u0301 (UTF-8 CC 81) - decomposed form used by APFS
  const auto b = nabla::json::parse("{\"p\":\"Marti" + bs + "u0301\"}");
  REQUIRE(b.string("p", "") == "Marti\xcc\x81");

  // Surrogate pair ud83d ude00 -> U+1F600 (UTF-8 F0 9F 98 80)
  const auto c = nabla::json::parse("{\"p\":\"" + bs + "ud83d" + bs + "ude00\"}");
  REQUIRE(c.string("p", "") == "\xf0\x9f\x98\x80");

  // Raw UTF-8 passes through untouched.
  const auto d = nabla::json::parse("{\"p\":\"Mart\xc3\xad\"}");
  REQUIRE(d.string("p", "") == "Mart\xc3\xad");
}
