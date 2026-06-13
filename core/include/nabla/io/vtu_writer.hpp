#pragma once

#include <string>

#include "nabla/mesh/octree.hpp"

namespace nabla::io {

// Write the octree as a VTK XML UnstructuredGrid (.vtu) of hexahedral cells,
// one hexahedron per leaf. All registered scalar fields, the refinement level,
// the solid mask, and any uint8 labels are emitted as cell data. The result
// opens directly in ParaView. No external VTK dependency — the XML is written
// by hand (ASCII), so this stays permissively licensed.
void writeVtu(const mesh::Octree& tree, const std::string& path);

}  // namespace nabla::io
