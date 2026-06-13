# Catch2 is fetched at configure time. Catch2 is licensed under the Boost
# Software License 1.0 — a permissive, non-copyleft license. It is a TEST-ONLY
# dependency and is never linked into the shipped nabla_core / nabla_solve
# artifacts, so it does not affect the "no GPL in the shipped core" rule.
include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.5.4
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(Catch2)

add_executable(nabla_tests
  test_field.cpp
  test_solver.cpp
  test_io.cpp
  test_morton.cpp
  test_octree.cpp
  test_checkpoint.cpp
  test_stl.cpp
  test_features.cpp
  test_geometry.cpp
  test_flow.cpp
  test_octree_poisson.cpp
  test_adaptive.cpp
  test_json.cpp)
target_link_libraries(nabla_tests PRIVATE nabla_core Catch2::Catch2WithMain)

# Register every Catch2 TEST_CASE with CTest so `ctest` runs them individually.
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(nabla_tests)
