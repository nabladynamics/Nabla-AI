#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "nabla/field.hpp"

TEST_CASE("Field2D initializes with a constant value", "[field]") {
  const nabla::Field2D f(4, 3, 2.5);
  REQUIRE(f.nx() == 4);
  REQUIRE(f.ny() == 3);
  REQUIRE(f.size() == 12);
  REQUIRE(f.min() == 2.5);
  REQUIRE(f.max() == 2.5);
  REQUIRE(f.mean() == 2.5);
}

TEST_CASE("Field2D indexing is row-major and mutable", "[field]") {
  nabla::Field2D f(3, 3, 0.0);
  f.at(1, 2) = 7.0;
  REQUIRE(f.at(1, 2) == 7.0);
  REQUIRE(f.max() == 7.0);
  REQUIRE(f.min() == 0.0);
}

TEST_CASE("Field2D::fill overwrites every cell", "[field]") {
  nabla::Field2D f(2, 2, 1.0);
  f.fill(-3.0);
  REQUIRE(f.mean() == -3.0);
}

TEST_CASE("Field2D rejects zero dimensions", "[field]") {
  REQUIRE_THROWS_AS(nabla::Field2D(0, 4), std::invalid_argument);
  REQUIRE_THROWS_AS(nabla::Field2D(4, 0), std::invalid_argument);
}
