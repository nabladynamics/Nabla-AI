#include "nabla/field.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>

namespace nabla {

Field2D::Field2D(std::size_t nx, std::size_t ny, double value)
    : nx_(nx), ny_(ny), data_(nx * ny, value) {
  if (nx == 0 || ny == 0) {
    throw std::invalid_argument("Field2D dimensions must be non-zero");
  }
}

void Field2D::fill(double value) {
  std::fill(data_.begin(), data_.end(), value);
}

double Field2D::max() const {
  return *std::max_element(data_.begin(), data_.end());
}

double Field2D::min() const {
  return *std::min_element(data_.begin(), data_.end());
}

double Field2D::mean() const {
  const double sum = std::accumulate(data_.begin(), data_.end(), 0.0);
  return sum / static_cast<double>(data_.size());
}

}  // namespace nabla
