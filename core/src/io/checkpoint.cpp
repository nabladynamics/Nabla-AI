#include "nabla/io/checkpoint.hpp"

#ifdef NABLA_HAVE_HDF5
#include <hdf5.h>
#endif

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace nabla::io {

bool hdf5Available() {
#ifdef NABLA_HAVE_HDF5
  return true;
#else
  return false;
#endif
}

#ifdef NABLA_HAVE_HDF5
namespace {

void check(herr_t status, const char* what) {
  if (status < 0) {
    throw std::runtime_error(std::string("HDF5 error: ") + what);
  }
}

void writeDataset1D(hid_t loc, const char* name, hid_t type, hsize_t n, const void* data) {
  const hsize_t dims[1] = {n};
  const hid_t space = H5Screate_simple(1, dims, nullptr);
  const hid_t dset =
      H5Dcreate2(loc, name, type, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (n > 0) {
    check(H5Dwrite(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, data), "H5Dwrite");
  }
  H5Dclose(dset);
  H5Sclose(space);
}

template <class T>
std::vector<T> readDataset1D(hid_t loc, const char* name, hid_t type) {
  const hid_t dset = H5Dopen2(loc, name, H5P_DEFAULT);
  if (dset < 0) {
    throw std::runtime_error(std::string("HDF5: missing dataset ") + name);
  }
  const hid_t space = H5Dget_space(dset);
  hsize_t dims[1] = {0};
  H5Sget_simple_extent_dims(space, dims, nullptr);
  std::vector<T> out(dims[0]);
  if (dims[0] > 0) {
    check(H5Dread(dset, type, H5S_ALL, H5S_ALL, H5P_DEFAULT, out.data()), "H5Dread");
  }
  H5Sclose(space);
  H5Dclose(dset);
  return out;
}

void writeAttrArray(hid_t loc, const char* name, const double* v, hsize_t n) {
  const hsize_t dims[1] = {n};
  const hid_t space = H5Screate_simple(1, dims, nullptr);
  const hid_t attr =
      H5Acreate2(loc, name, H5T_NATIVE_DOUBLE, space, H5P_DEFAULT, H5P_DEFAULT);
  check(H5Awrite(attr, H5T_NATIVE_DOUBLE, v), "H5Awrite array");
  H5Aclose(attr);
  H5Sclose(space);
}

void writeAttrInt(hid_t loc, const char* name, int val) {
  const hid_t space = H5Screate(H5S_SCALAR);
  const hid_t attr = H5Acreate2(loc, name, H5T_NATIVE_INT, space, H5P_DEFAULT, H5P_DEFAULT);
  check(H5Awrite(attr, H5T_NATIVE_INT, &val), "H5Awrite int");
  H5Aclose(attr);
  H5Sclose(space);
}

void readAttrArray(hid_t loc, const char* name, double* v) {
  const hid_t attr = H5Aopen(loc, name, H5P_DEFAULT);
  check(H5Aread(attr, H5T_NATIVE_DOUBLE, v), "H5Aread array");
  H5Aclose(attr);
}

int readAttrInt(hid_t loc, const char* name) {
  int val = 0;
  const hid_t attr = H5Aopen(loc, name, H5P_DEFAULT);
  check(H5Aread(attr, H5T_NATIVE_INT, &val), "H5Aread int");
  H5Aclose(attr);
  return val;
}

// Create a group that tracks link creation order, so iteration on read returns
// fields in the exact order they were written (keeps u,v,w,p in slots 0..3).
hid_t createOrderedGroup(hid_t loc, const char* name) {
  const hid_t gcpl = H5Pcreate(H5P_GROUP_CREATE);
  H5Pset_link_creation_order(gcpl, H5P_CRT_ORDER_TRACKED | H5P_CRT_ORDER_INDEXED);
  const hid_t g = H5Gcreate2(loc, name, H5P_DEFAULT, gcpl, H5P_DEFAULT);
  H5Pclose(gcpl);
  return g;
}

std::vector<std::string> orderedMemberNames(hid_t group) {
  std::vector<std::string> names;
  H5G_info_t gi;
  H5Gget_info(group, &gi);
  for (hsize_t i = 0; i < gi.nlinks; ++i) {
    const ssize_t len = H5Lget_name_by_idx(group, ".", H5_INDEX_CRT_ORDER, H5_ITER_INC, i,
                                           nullptr, 0, H5P_DEFAULT);
    if (len <= 0) {
      continue;
    }
    std::string nm(static_cast<std::size_t>(len), '\0');
    H5Lget_name_by_idx(group, ".", H5_INDEX_CRT_ORDER, H5_ITER_INC, i, nm.data(),
                       static_cast<std::size_t>(len) + 1, H5P_DEFAULT);
    names.push_back(nm);
  }
  return names;
}

}  // namespace
#endif  // NABLA_HAVE_HDF5

void writeCheckpoint([[maybe_unused]] const mesh::Octree& tree,
                     [[maybe_unused]] const std::string& path) {
#ifndef NABLA_HAVE_HDF5
  throw std::runtime_error("writeCheckpoint: this build has no HDF5 support");
#else
  const mesh::OctreeState s = tree.dumpState();
  const hid_t file = H5Fcreate(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  if (file < 0) {
    throw std::runtime_error("writeCheckpoint: cannot create " + path);
  }

  const double origin[3] = {s.origin.x, s.origin.y, s.origin.z};
  const double extent[3] = {s.extent.x, s.extent.y, s.extent.z};
  writeAttrArray(file, "origin", origin, 3);
  writeAttrArray(file, "extent", extent, 3);
  writeAttrInt(file, "maxLevel", s.maxLevel);

  const hsize_t n = s.morton.size();
  writeDataset1D(file, "morton", H5T_NATIVE_UINT64, n, s.morton.data());
  writeDataset1D(file, "level", H5T_NATIVE_UINT8, n, s.level.data());
  writeDataset1D(file, "mask", H5T_NATIVE_UINT8, n, s.mask.data());

  const hid_t gs = createOrderedGroup(file, "scalars");
  for (std::size_t f = 0; f < s.scalarNames.size(); ++f) {
    writeDataset1D(gs, s.scalarNames[f].c_str(), H5T_NATIVE_DOUBLE, n, s.scalars[f].data());
  }
  H5Gclose(gs);

  const hid_t gl = createOrderedGroup(file, "labels");
  for (std::size_t f = 0; f < s.labelNames.size(); ++f) {
    writeDataset1D(gl, s.labelNames[f].c_str(), H5T_NATIVE_UINT8, n, s.labels[f].data());
  }
  H5Gclose(gl);

  H5Fclose(file);
#endif
}

mesh::Octree readCheckpoint([[maybe_unused]] const std::string& path) {
#ifndef NABLA_HAVE_HDF5
  throw std::runtime_error("readCheckpoint: this build has no HDF5 support");
#else
  const hid_t file = H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
  if (file < 0) {
    throw std::runtime_error("readCheckpoint: cannot open " + path);
  }
  mesh::OctreeState s;
  double origin[3] = {0, 0, 0};
  double extent[3] = {0, 0, 0};
  readAttrArray(file, "origin", origin);
  readAttrArray(file, "extent", extent);
  s.origin = {origin[0], origin[1], origin[2]};
  s.extent = {extent[0], extent[1], extent[2]};
  s.maxLevel = readAttrInt(file, "maxLevel");

  s.morton = readDataset1D<uint64_t>(file, "morton", H5T_NATIVE_UINT64);
  s.level = readDataset1D<uint8_t>(file, "level", H5T_NATIVE_UINT8);
  s.mask = readDataset1D<uint8_t>(file, "mask", H5T_NATIVE_UINT8);

  const hid_t gs = H5Gopen2(file, "scalars", H5P_DEFAULT);
  if (gs >= 0) {
    for (const std::string& nm : orderedMemberNames(gs)) {
      s.scalarNames.push_back(nm);
      s.scalars.push_back(readDataset1D<double>(gs, nm.c_str(), H5T_NATIVE_DOUBLE));
    }
    H5Gclose(gs);
  }
  const hid_t gl = H5Gopen2(file, "labels", H5P_DEFAULT);
  if (gl >= 0) {
    for (const std::string& nm : orderedMemberNames(gl)) {
      s.labelNames.push_back(nm);
      s.labels.push_back(readDataset1D<uint8_t>(gl, nm.c_str(), H5T_NATIVE_UINT8));
    }
    H5Gclose(gl);
  }

  H5Fclose(file);
  return mesh::Octree(s);
#endif
}

}  // namespace nabla::io
