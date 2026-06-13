#pragma once

#include <string>

#include "nabla/mesh/octree.hpp"

// HDF5 checkpoint I/O — a bit-exact, full-state round-trip of an Octree.
//
// HDF5 is licensed under the permissive (BSD-style) HDF5 license, so it is an
// acceptable core dependency. Support is optional at build time: if the build
// was configured without HDF5, hdf5Available() returns false and the read/write
// functions throw.
namespace nabla::io {

bool hdf5Available();

// Throws std::runtime_error on I/O error or if HDF5 support is not compiled in.
void writeCheckpoint(const mesh::Octree& tree, const std::string& path);
mesh::Octree readCheckpoint(const std::string& path);

}  // namespace nabla::io
