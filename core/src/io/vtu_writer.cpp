#include "nabla/io/vtu_writer.hpp"

#include <fstream>
#include <stdexcept>

namespace nabla::io {

void writeVtu(const mesh::Octree& tree, const std::string& path) {
  std::ofstream f(path);
  if (!f) {
    throw std::runtime_error("writeVtu: cannot open file: " + path);
  }
  f.precision(10);

  const std::size_t n = tree.cellCount();
  const std::size_t nPoints = n * 8;

  f << "<?xml version=\"1.0\"?>\n";
  f << "<VTKFile type=\"UnstructuredGrid\" version=\"1.0\" "
       "byte_order=\"LittleEndian\" header_type=\"UInt64\">\n";
  f << "  <UnstructuredGrid>\n";
  f << "    <Piece NumberOfPoints=\"" << nPoints << "\" NumberOfCells=\"" << n << "\">\n";

  // ---- points: 8 corners per cell, VTK_HEXAHEDRON ordering ----
  f << "      <Points>\n";
  f << "        <DataArray type=\"Float64\" NumberOfComponents=\"3\" format=\"ascii\">\n";
  for (std::size_t i = 0; i < n; ++i) {
    const mesh::Vec3 c = tree.cellCenter(i);
    const mesh::Vec3 s = tree.cellSize(i);
    const double x0 = c.x - 0.5 * s.x, x1 = c.x + 0.5 * s.x;
    const double y0 = c.y - 0.5 * s.y, y1 = c.y + 0.5 * s.y;
    const double z0 = c.z - 0.5 * s.z, z1 = c.z + 0.5 * s.z;
    // 0..7 in VTK hexahedron order.
    f << x0 << ' ' << y0 << ' ' << z0 << ' ' << x1 << ' ' << y0 << ' ' << z0 << ' '
      << x1 << ' ' << y1 << ' ' << z0 << ' ' << x0 << ' ' << y1 << ' ' << z0 << ' '
      << x0 << ' ' << y0 << ' ' << z1 << ' ' << x1 << ' ' << y0 << ' ' << z1 << ' '
      << x1 << ' ' << y1 << ' ' << z1 << ' ' << x0 << ' ' << y1 << ' ' << z1 << '\n';
  }
  f << "        </DataArray>\n      </Points>\n";

  // ---- cells ----
  f << "      <Cells>\n";
  f << "        <DataArray type=\"Int64\" Name=\"connectivity\" format=\"ascii\">\n";
  for (std::size_t i = 0; i < nPoints; ++i) {
    f << i << ((i + 1) % 8 == 0 ? '\n' : ' ');
  }
  f << "        </DataArray>\n";
  f << "        <DataArray type=\"Int64\" Name=\"offsets\" format=\"ascii\">\n";
  for (std::size_t i = 1; i <= n; ++i) {
    f << (i * 8) << ((i % 16 == 0) ? '\n' : ' ');
  }
  f << "\n        </DataArray>\n";
  f << "        <DataArray type=\"UInt8\" Name=\"types\" format=\"ascii\">\n";
  for (std::size_t i = 0; i < n; ++i) {
    f << "12" << ((i + 1) % 32 == 0 ? '\n' : ' ');  // 12 = VTK_HEXAHEDRON
  }
  f << "\n        </DataArray>\n      </Cells>\n";

  // ---- cell data ----
  f << "      <CellData Scalars=\"level\">\n";

  f << "        <DataArray type=\"Int32\" Name=\"level\" format=\"ascii\">\n";
  for (std::size_t i = 0; i < n; ++i) {
    f << tree.level(i) << ((i + 1) % 32 == 0 ? '\n' : ' ');
  }
  f << "\n        </DataArray>\n";

  f << "        <DataArray type=\"UInt8\" Name=\"mask\" format=\"ascii\">\n";
  for (std::size_t i = 0; i < n; ++i) {
    f << static_cast<int>(tree.mask(i)) << ((i + 1) % 32 == 0 ? '\n' : ' ');
  }
  f << "\n        </DataArray>\n";

  for (const std::string& name : tree.scalarNames()) {
    const std::vector<double>& field = tree.scalar(name);
    f << "        <DataArray type=\"Float64\" Name=\"" << name << "\" format=\"ascii\">\n";
    for (std::size_t i = 0; i < n; ++i) {
      f << field[i] << ((i + 1) % 8 == 0 ? '\n' : ' ');
    }
    f << "\n        </DataArray>\n";
  }

  for (const std::string& name : tree.labelNames()) {
    const std::vector<uint8_t>& field = tree.label(name);
    f << "        <DataArray type=\"UInt8\" Name=\"" << name << "\" format=\"ascii\">\n";
    for (std::size_t i = 0; i < n; ++i) {
      f << static_cast<int>(field[i]) << ((i + 1) % 32 == 0 ? '\n' : ' ');
    }
    f << "\n        </DataArray>\n";
  }

  f << "      </CellData>\n";
  f << "    </Piece>\n  </UnstructuredGrid>\n</VTKFile>\n";
}

}  // namespace nabla::io
