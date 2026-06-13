#include "nabla/geometry/stl_reader.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace nabla::geometry {
namespace {

// ---- raw readers (produce an unwelded soup of triangles) ------------------

bool looksBinary(std::ifstream& f, std::uintmax_t fileSize) {
  // A binary STL is exactly 80 + 4 + 50*nTris bytes. Use that exact identity —
  // far more reliable than sniffing for the word "solid".
  if (fileSize < 84) {
    return false;
  }
  f.seekg(80, std::ios::beg);
  uint32_t nTris = 0;
  f.read(reinterpret_cast<char*>(&nTris), 4);
  f.seekg(0, std::ios::beg);
  const std::uintmax_t expected = 84ull + 50ull * static_cast<std::uintmax_t>(nTris);
  return expected == fileSize;
}

TriMesh readBinary(std::ifstream& f) {
  f.seekg(80, std::ios::beg);
  uint32_t nTris = 0;
  f.read(reinterpret_cast<char*>(&nTris), 4);

  TriMesh m;
  m.vertices.reserve(static_cast<std::size_t>(nTris) * 3);
  m.tris.reserve(nTris);
  for (uint32_t t = 0; t < nTris; ++t) {
    float buf[12];
    f.read(reinterpret_cast<char*>(buf), sizeof(buf));
    if (!f) {
      throw std::runtime_error("STL: truncated binary triangle data");
    }
    uint16_t attr = 0;
    f.read(reinterpret_cast<char*>(&attr), 2);
    const auto base = static_cast<uint32_t>(m.vertices.size());
    for (int v = 0; v < 3; ++v) {
      m.vertices.push_back({static_cast<double>(buf[3 + v * 3 + 0]),
                            static_cast<double>(buf[3 + v * 3 + 1]),
                            static_cast<double>(buf[3 + v * 3 + 2])});
    }
    m.tris.push_back({base, base + 1, base + 2});
  }
  return m;
}

TriMesh readAscii(std::ifstream& f) {
  f.seekg(0, std::ios::beg);
  TriMesh m;
  std::string token;
  std::vector<Vec3> pending;
  while (f >> token) {
    if (token == "vertex") {
      Vec3 v;
      if (!(f >> v.x >> v.y >> v.z)) {
        throw std::runtime_error("STL: malformed ASCII vertex");
      }
      pending.push_back(v);
      if (pending.size() == 3) {
        const auto base = static_cast<uint32_t>(m.vertices.size());
        m.vertices.push_back(pending[0]);
        m.vertices.push_back(pending[1]);
        m.vertices.push_back(pending[2]);
        m.tris.push_back({base, base + 1, base + 2});
        pending.clear();
      }
    }
  }
  if (m.tris.empty()) {
    throw std::runtime_error("STL: no triangles found (not a valid ASCII STL?)");
  }
  return m;
}

// ---- cleaning -------------------------------------------------------------

struct VKey {
  int64_t x, y, z;
  bool operator==(const VKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct VKeyHash {
  std::size_t operator()(const VKey& k) const {
    std::size_t h = 1469598103934665603ull;
    for (int64_t c : {k.x, k.y, k.z}) {
      h = (h ^ static_cast<std::size_t>(c)) * 1099511628211ull;
    }
    return h;
  }
};

// Weld vertices that fall in the same quantization cell (tolerance-based).
TriMesh weld(const TriMesh& in, double tol, std::size_t& weldedOut) {
  TriMesh out;
  const double inv = 1.0 / tol;
  std::unordered_map<VKey, uint32_t, VKeyHash> map;
  map.reserve(in.vertices.size());
  std::vector<uint32_t> remap(in.vertices.size());
  for (std::size_t i = 0; i < in.vertices.size(); ++i) {
    const Vec3& v = in.vertices[i];
    const VKey key{static_cast<int64_t>(std::llround(v.x * inv)),
                   static_cast<int64_t>(std::llround(v.y * inv)),
                   static_cast<int64_t>(std::llround(v.z * inv))};
    const auto it = map.find(key);
    if (it == map.end()) {
      const auto idx = static_cast<uint32_t>(out.vertices.size());
      map.emplace(key, idx);
      out.vertices.push_back(v);
      remap[i] = idx;
    } else {
      remap[i] = it->second;
    }
  }
  out.tris.reserve(in.tris.size());
  for (const Tri& t : in.tris) {
    out.tris.push_back({remap[t[0]], remap[t[1]], remap[t[2]]});
  }
  weldedOut = in.vertices.size() - out.vertices.size();
  return out;
}

// Drop triangles with a repeated vertex index or (near) zero area.
TriMesh dropDegenerate(const TriMesh& in, double areaTol, std::size_t& removedOut) {
  TriMesh out;
  out.vertices = in.vertices;
  out.tris.reserve(in.tris.size());
  std::size_t removed = 0;
  for (std::size_t f = 0; f < in.tris.size(); ++f) {
    const Tri& t = in.tris[f];
    if (t[0] == t[1] || t[1] == t[2] || t[0] == t[2] || in.faceArea(f) <= areaTol) {
      ++removed;
      continue;
    }
    out.tris.push_back(t);
  }
  removedOut = removed;
  return out;
}

struct EKey {
  uint32_t a, b;
  bool operator==(const EKey& o) const { return a == o.a && b == o.b; }
};
struct EKeyHash {
  std::size_t operator()(const EKey& k) const {
    return (static_cast<std::size_t>(k.a) << 32) ^ k.b;
  }
};

void watertightness(const TriMesh& m, std::size_t& boundary, std::size_t& nonManifold,
                    double& minEdge) {
  std::unordered_map<EKey, int, EKeyHash> count;
  count.reserve(m.tris.size() * 3);
  minEdge = std::numeric_limits<double>::infinity();
  const auto edge = [&](uint32_t i, uint32_t j) {
    const EKey k{std::min(i, j), std::max(i, j)};
    ++count[k];
    minEdge = std::min(minEdge, norm(m.vertices[i] - m.vertices[j]));
  };
  for (const Tri& t : m.tris) {
    edge(t[0], t[1]);
    edge(t[1], t[2]);
    edge(t[2], t[0]);
  }
  boundary = 0;
  nonManifold = 0;
  for (const auto& [k, c] : count) {
    if (c == 1) {
      ++boundary;
    } else if (c > 2) {
      ++nonManifold;
    }
  }
  if (!std::isfinite(minEdge)) {
    minEdge = 0.0;
  }
}

}  // namespace

StlReadResult readStl(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    throw std::runtime_error("STL: cannot open file: " + path);
  }
  f.seekg(0, std::ios::end);
  const auto fileSize = static_cast<std::uintmax_t>(f.tellg());
  f.seekg(0, std::ios::beg);
  if (fileSize == 0) {
    throw std::runtime_error("STL: empty file: " + path);
  }

  StlReadResult result;
  const bool binary = looksBinary(f, fileSize);
  result.format = binary ? StlFormat::Binary : StlFormat::Ascii;
  TriMesh raw = binary ? readBinary(f) : readAscii(f);

  CleanReport& r = result.report;
  r.rawTriangles = raw.tris.size();
  r.rawVertices = raw.vertices.size();

  const Aabb box = raw.boundingBox();
  const double diag = norm(box.hi - box.lo);
  const double weldTol = (diag > 0.0 ? diag : 1.0) * 1e-6;
  const double areaTol = (diag > 0.0 ? diag * diag : 1.0) * 1e-12;

  TriMesh welded = weld(raw, weldTol, r.weldedVertices);
  TriMesh clean = dropDegenerate(welded, areaTol, r.removedDegenerate);

  r.vertices = clean.vertices.size();
  r.triangles = clean.tris.size();
  watertightness(clean, r.boundaryEdges, r.nonManifoldEdges, r.minEdgeLength);
  r.watertight = (r.boundaryEdges == 0 && r.nonManifoldEdges == 0 && r.triangles > 0);
  r.bboxLo = box.lo;
  r.bboxHi = box.hi;
  r.characteristicLength = diag;

  result.mesh = std::move(clean);
  return result;
}

void writeBinaryStl(const TriMesh& mesh, const std::string& path) {
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    throw std::runtime_error("STL: cannot write: " + path);
  }
  char header[80] = {0};
  std::memcpy(header, "nabla-binary-stl", 16);
  f.write(header, 80);
  auto nTris = static_cast<uint32_t>(mesh.tris.size());
  f.write(reinterpret_cast<const char*>(&nTris), 4);
  for (std::size_t fi = 0; fi < mesh.tris.size(); ++fi) {
    const Vec3 n = mesh.faceNormal(fi);
    const float nf[3] = {static_cast<float>(n.x), static_cast<float>(n.y),
                         static_cast<float>(n.z)};
    f.write(reinterpret_cast<const char*>(nf), 12);
    for (int v = 0; v < 3; ++v) {
      const Vec3& p = mesh.vertices[mesh.tris[fi][static_cast<std::size_t>(v)]];
      const float pf[3] = {static_cast<float>(p.x), static_cast<float>(p.y),
                           static_cast<float>(p.z)};
      f.write(reinterpret_cast<const char*>(pf), 12);
    }
    const uint16_t attr = 0;
    f.write(reinterpret_cast<const char*>(&attr), 2);
  }
}

void writeAsciiStl(const TriMesh& mesh, const std::string& path) {
  std::ofstream f(path);
  if (!f) {
    throw std::runtime_error("STL: cannot write: " + path);
  }
  f.precision(9);
  f << "solid nabla\n";
  for (std::size_t fi = 0; fi < mesh.tris.size(); ++fi) {
    const Vec3 n = mesh.faceNormal(fi);
    f << "  facet normal " << n.x << ' ' << n.y << ' ' << n.z << "\n    outer loop\n";
    for (int v = 0; v < 3; ++v) {
      const Vec3& p = mesh.vertices[mesh.tris[fi][static_cast<std::size_t>(v)]];
      f << "      vertex " << p.x << ' ' << p.y << ' ' << p.z << '\n';
    }
    f << "    endloop\n  endfacet\n";
  }
  f << "endsolid nabla\n";
}

}  // namespace nabla::geometry
