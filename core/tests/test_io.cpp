#include <string>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "nabla/config.hpp"
#include "nabla/io.hpp"

TEST_CASE("parse_config reads known keys and ignores the rest", "[io]") {
  const std::string spec =
      "# sample spec\n"
      "nx = 32\n"
      "ny = 48\n"
      "steps = 250\n"
      "diffusivity = 0.2  # trailing comment\n"
      "unknown_key = 5\n"
      "\n"
      "boundary_temp = -1.5\n";

  const nabla::SimConfig cfg = nabla::io::parse_config(spec);
  REQUIRE(cfg.nx == 32);
  REQUIRE(cfg.ny == 48);
  REQUIRE(cfg.steps == 250);
  REQUIRE(cfg.diffusivity == Catch::Approx(0.2));
  REQUIRE(cfg.boundary_temp == Catch::Approx(-1.5));
}

TEST_CASE("parse_config keeps defaults for missing keys", "[io]") {
  const nabla::SimConfig defaults;
  const nabla::SimConfig cfg = nabla::io::parse_config("steps = 5\n");
  REQUIRE(cfg.steps == 5);
  REQUIRE(cfg.nx == defaults.nx);
  REQUIRE(cfg.dt == Catch::Approx(defaults.dt));
}
