#include "nabla/geometry/geometry_report.hpp"

#include "nabla/version.hpp"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace nabla::geometry {
namespace {

// --- a tiny JSON value model (avoids a third-party JSON dependency) ---------
struct JVal {
  enum class T { Null, Bool, Num, Str, Arr, Obj } t = T::Null;
  bool b = false;
  double num = 0.0;
  std::string str;
  std::vector<JVal> arr;
  std::vector<std::pair<std::string, JVal>> obj;
};

JVal jnum(double v) {
  JVal j;
  j.t = JVal::T::Num;
  j.num = v;
  return j;
}
JVal jint(long long v) { return jnum(static_cast<double>(v)); }
JVal jbool(bool v) {
  JVal j;
  j.t = JVal::T::Bool;
  j.b = v;
  return j;
}
JVal jstr(std::string s) {
  JVal j;
  j.t = JVal::T::Str;
  j.str = std::move(s);
  return j;
}
JVal jarr() {
  JVal j;
  j.t = JVal::T::Arr;
  return j;
}
JVal jobj() {
  JVal j;
  j.t = JVal::T::Obj;
  return j;
}
JVal jvec3(Vec3 v) {
  JVal a = jarr();
  a.arr = {jnum(v.x), jnum(v.y), jnum(v.z)};
  return a;
}

std::string numStr(double v) {
  char buf[64];
  if (std::isfinite(v) && v == std::floor(v) && std::abs(v) < 1e15) {
    std::snprintf(buf, sizeof(buf), "%.0f", v);
  } else if (!std::isfinite(v)) {
    std::snprintf(buf, sizeof(buf), "0");  // JSON has no inf/nan
  } else {
    std::snprintf(buf, sizeof(buf), "%.10g", v);
  }
  return buf;
}

void escape(std::ostream& os, const std::string& s) {
  os << '"';
  for (char c : s) {
    switch (c) {
      case '"': os << "\\\""; break;
      case '\\': os << "\\\\"; break;
      case '\n': os << "\\n"; break;
      case '\t': os << "\\t"; break;
      default: os << c;
    }
  }
  os << '"';
}

void serialize(std::ostream& os, const JVal& v, int indent) {
  const std::string pad(static_cast<std::size_t>(indent) * 2, ' ');
  const std::string pad1(static_cast<std::size_t>(indent + 1) * 2, ' ');
  switch (v.t) {
    case JVal::T::Null: os << "null"; break;
    case JVal::T::Bool: os << (v.b ? "true" : "false"); break;
    case JVal::T::Num: os << numStr(v.num); break;
    case JVal::T::Str: escape(os, v.str); break;
    case JVal::T::Arr: {
      if (v.arr.empty()) {
        os << "[]";
        break;
      }
      // Compact inline for arrays of pure numbers (e.g., points); else block.
      bool simple = true;
      for (const JVal& e : v.arr) {
        if (e.t != JVal::T::Num) {
          simple = false;
          break;
        }
      }
      if (simple) {
        os << "[";
        for (std::size_t i = 0; i < v.arr.size(); ++i) {
          os << (i ? ", " : "") << numStr(v.arr[i].num);
        }
        os << "]";
      } else {
        os << "[\n";
        for (std::size_t i = 0; i < v.arr.size(); ++i) {
          os << pad1;
          serialize(os, v.arr[i], indent + 1);
          os << (i + 1 < v.arr.size() ? ",\n" : "\n");
        }
        os << pad << "]";
      }
      break;
    }
    case JVal::T::Obj: {
      if (v.obj.empty()) {
        os << "{}";
        break;
      }
      os << "{\n";
      for (std::size_t i = 0; i < v.obj.size(); ++i) {
        os << pad1;
        escape(os, v.obj[i].first);
        os << ": ";
        serialize(os, v.obj[i].second, indent + 1);
        os << (i + 1 < v.obj.size() ? ",\n" : "\n");
      }
      os << pad << "}";
      break;
    }
  }
}

}  // namespace

void writeGeometryReport(const std::string& path, const std::string& stlPath,
                         const StlReadResult& stl, const FeatureSet& features,
                         const DomainInfo& domain, const MeshStats& stats) {
  const CleanReport& r = stl.report;

  JVal root = jobj();
  root.obj.emplace_back("case", jstr(domain.caseName));
  root.obj.emplace_back("schema", jstr("nabla.geometry_report/1"));
  root.obj.emplace_back("solver_version", jstr(nabla::kVersionString));
  root.obj.emplace_back("git_sha", jstr(nabla::kGitSha));

  JVal stlNode = jobj();
  stlNode.obj.emplace_back("path", jstr(stlPath));
  stlNode.obj.emplace_back(
      "format", jstr(stl.format == StlFormat::Binary ? "binary" : "ascii"));
  stlNode.obj.emplace_back("triangles_raw", jint(static_cast<long long>(r.rawTriangles)));
  root.obj.emplace_back("stl", std::move(stlNode));

  JVal clean = jobj();
  clean.obj.emplace_back("vertices", jint(static_cast<long long>(r.vertices)));
  clean.obj.emplace_back("triangles", jint(static_cast<long long>(r.triangles)));
  clean.obj.emplace_back("welded_vertices", jint(static_cast<long long>(r.weldedVertices)));
  clean.obj.emplace_back("removed_degenerate",
                         jint(static_cast<long long>(r.removedDegenerate)));
  clean.obj.emplace_back("watertight", jbool(r.watertight));
  clean.obj.emplace_back("boundary_edges", jint(static_cast<long long>(r.boundaryEdges)));
  clean.obj.emplace_back("non_manifold_edges",
                         jint(static_cast<long long>(r.nonManifoldEdges)));
  root.obj.emplace_back("cleaning", std::move(clean));

  JVal bbox = jobj();
  bbox.obj.emplace_back("min", jvec3(r.bboxLo));
  bbox.obj.emplace_back("max", jvec3(r.bboxHi));
  bbox.obj.emplace_back("size", jvec3(r.bboxHi - r.bboxLo));
  root.obj.emplace_back("bounding_box", std::move(bbox));
  root.obj.emplace_back("characteristic_length", jnum(r.characteristicLength));
  root.obj.emplace_back("smallest_feature", jnum(features.smallestFeature));

  JVal feat = jobj();
  JVal sharp = jobj();
  sharp.obj.emplace_back("count", jint(static_cast<long long>(features.sharpEdges.size())));
  sharp.obj.emplace_back("angle_threshold_deg", jnum(features.sharpAngleDeg));
  sharp.obj.emplace_back("min_length", jnum(features.minSharpEdgeLength));
  sharp.obj.emplace_back("max_length", jnum(features.maxSharpEdgeLength));
  sharp.obj.emplace_back("total_length", jnum(features.totalSharpEdgeLength));
  JVal samples = jarr();
  const std::size_t maxSamples = 200;
  for (std::size_t i = 0; i < features.sharpEdges.size() && i < maxSamples; ++i) {
    const SharpEdge& se = features.sharpEdges[i];
    JVal e = jobj();
    e.obj.emplace_back("a", jvec3(se.a));
    e.obj.emplace_back("b", jvec3(se.b));
    e.obj.emplace_back("dihedral_deg", jnum(se.dihedralDeg));
    e.obj.emplace_back("length", jnum(se.length));
    samples.arr.push_back(std::move(e));
  }
  sharp.obj.emplace_back("samples", std::move(samples));
  feat.obj.emplace_back("sharp_edges", std::move(sharp));

  JVal corners = jobj();
  corners.obj.emplace_back("count", jint(static_cast<long long>(features.corners.size())));
  JVal locs = jarr();
  for (const Vec3& c : features.corners) {
    locs.arr.push_back(jvec3(c));
  }
  corners.obj.emplace_back("locations", std::move(locs));
  feat.obj.emplace_back("corners", std::move(corners));

  JVal curv = jobj();
  curv.obj.emplace_back("min", jnum(features.curvatureMin));
  curv.obj.emplace_back("max", jnum(features.curvatureMax));
  curv.obj.emplace_back("mean", jnum(features.curvatureMean));
  feat.obj.emplace_back("curvature", std::move(curv));
  root.obj.emplace_back("features", std::move(feat));

  JVal dom = jobj();
  dom.obj.emplace_back("origin", jvec3(domain.origin));
  dom.obj.emplace_back("extent", jvec3(domain.extent));
  dom.obj.emplace_back("cube_height_h", jnum(domain.cubeHeight));
  root.obj.emplace_back("domain", std::move(dom));

  JVal ms = jobj();
  ms.obj.emplace_back("cells", jint(static_cast<long long>(stats.cells)));
  ms.obj.emplace_back("inside_solid", jint(static_cast<long long>(stats.insideSolid)));
  ms.obj.emplace_back("cut", jint(static_cast<long long>(stats.cut)));
  ms.obj.emplace_back("fluid", jint(static_cast<long long>(stats.fluid)));
  ms.obj.emplace_back("min_level", jint(stats.minLevel));
  ms.obj.emplace_back("max_level", jint(stats.maxLevel));
  ms.obj.emplace_back("balanced", jbool(stats.balanced));
  ms.obj.emplace_back("surface_target_size", jnum(stats.surfaceTarget));
  ms.obj.emplace_back("edge_target_size", jnum(stats.edgeTarget));
  root.obj.emplace_back("mesh", std::move(ms));

  std::ofstream f(path);
  if (!f) {
    throw std::runtime_error("geometry report: cannot write " + path);
  }
  serialize(f, root, 0);
  f << "\n";
}

}  // namespace nabla::geometry
