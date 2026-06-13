#pragma once

#include <cstddef>
#include <vector>

namespace nabla {

// Row-major 2D scalar field on a uniform grid. This is the fundamental state
// container the solver operates on (temperature, pressure, a velocity
// component, ...). Index (i, j): i is the x/column index, j the y/row index.
class Field2D {
 public:
  Field2D(std::size_t nx, std::size_t ny, double value = 0.0);

  [[nodiscard]] std::size_t nx() const noexcept { return nx_; }
  [[nodiscard]] std::size_t ny() const noexcept { return ny_; }
  [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

  [[nodiscard]] double& at(std::size_t i, std::size_t j) noexcept {
    return data_[index(i, j)];
  }
  [[nodiscard]] double at(std::size_t i, std::size_t j) const noexcept {
    return data_[index(i, j)];
  }

  void fill(double value);

  [[nodiscard]] double max() const;
  [[nodiscard]] double min() const;
  [[nodiscard]] double mean() const;

  [[nodiscard]] const std::vector<double>& data() const noexcept {
    return data_;
  }

 private:
  [[nodiscard]] std::size_t index(std::size_t i, std::size_t j) const noexcept {
    return j * nx_ + i;
  }

  std::size_t nx_;
  std::size_t ny_;
  std::vector<double> data_;
};

}  // namespace nabla
