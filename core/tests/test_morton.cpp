#include <cstdint>
#include <random>

#include <catch2/catch_test_macros.hpp>

#include "nabla/mesh/morton.hpp"

using namespace nabla::mesh;

TEST_CASE("Morton encode/decode round-trips across the full 21-bit range", "[morton]") {
  std::mt19937_64 rng(0xC0FFEE);
  std::uniform_int_distribution<uint32_t> dist(0, kMortonMaxCoord);
  for (int trial = 0; trial < 20000; ++trial) {
    const uint32_t x = dist(rng);
    const uint32_t y = dist(rng);
    const uint32_t z = dist(rng);
    const uint64_t code = mortonEncode(x, y, z);
    uint32_t dx, dy, dz;
    mortonDecode(code, dx, dy, dz);
    REQUIRE(dx == x);
    REQUIRE(dy == y);
    REQUIRE(dz == z);
  }
}

TEST_CASE("Morton encodes the boundary corner values", "[morton]") {
  uint32_t x, y, z;
  mortonDecode(mortonEncode(0, 0, 0), x, y, z);
  REQUIRE((x == 0 && y == 0 && z == 0));
  mortonDecode(mortonEncode(kMortonMaxCoord, kMortonMaxCoord, kMortonMaxCoord), x, y, z);
  REQUIRE((x == kMortonMaxCoord && y == kMortonMaxCoord && z == kMortonMaxCoord));
}

TEST_CASE("Morton interleaves axes independently", "[morton]") {
  // A pure-x coordinate occupies bits 0,3,6,... only.
  REQUIRE(mortonEncode(1, 0, 0) == 0x1ULL);
  REQUIRE(mortonEncode(0, 1, 0) == 0x2ULL);
  REQUIRE(mortonEncode(0, 0, 1) == 0x4ULL);
}
