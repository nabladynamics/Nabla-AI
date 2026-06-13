#include "nabla/flow/case.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>

#include "nabla/io/json.hpp"
#include "nabla/version.hpp"

namespace nabla::flow {
namespace {

void setWalls(UniformGrid& g, BoundaryType t) {
  for (auto& f : g.face) {
    f.type = t;
  }
}

}  // namespace

CaseSpec makeLidCavity(int n, double reynolds, double lidVelocity) {
  CaseSpec s;
  s.name = "lid-cavity";
  UniformGrid& g = s.grid;
  g.nx = n;
  g.ny = n;
  g.nz = 1;
  g.dx = 1.0 / n;
  g.dy = 1.0 / n;
  g.dz = 1.0 / n;
  g.origin = {0.0, 0.0, 0.0};
  setWalls(g, BoundaryType::Wall);
  g.face[4].type = BoundaryType::Periodic;  // z
  g.face[5].type = BoundaryType::Periodic;
  g.face[3].wallVelocity = {lidVelocity, 0.0, 0.0};  // moving lid on +y
  g.allocate();

  s.flow.reynolds = reynolds;
  s.flow.refLength = 1.0;
  s.flow.velocityScale = lidVelocity;
  s.flow.cfl = 0.8;
  s.flow.convection = ConvectionScheme::Central;
  s.run.maxSteps = 60000;
  s.run.steadyTol = 1e-6;
  return s;
}

CaseSpec makeChannel(int nx, int ny, double reynolds, double bulkVelocity) {
  CaseSpec s;
  s.name = "channel";
  UniformGrid& g = s.grid;
  const double Ly = 1.0;
  const double Lx = 6.0 * Ly;
  g.nx = nx;
  g.ny = ny;
  g.nz = 1;
  g.dx = Lx / nx;
  g.dy = Ly / ny;
  g.dz = g.dy;
  g.origin = {0.0, 0.0, 0.0};
  g.face[0].type = BoundaryType::Inlet;
  g.face[0].profile = InletProfile::Parabolic;
  g.face[0].inletSpeed = bulkVelocity;
  g.face[0].inletAxis = 0;
  g.face[1].type = BoundaryType::Outlet;
  g.face[2].type = BoundaryType::Wall;  // floor
  g.face[3].type = BoundaryType::Wall;  // ceiling
  g.face[4].type = BoundaryType::Periodic;
  g.face[5].type = BoundaryType::Periodic;
  g.allocate();

  s.flow.reynolds = reynolds;
  s.flow.refLength = Ly;
  s.flow.velocityScale = bulkVelocity;
  s.flow.cfl = 0.8;
  s.flow.convection = ConvectionScheme::Central;
  s.run.maxSteps = 40000;
  s.run.steadyTol = 1e-6;
  return s;
}

CaseSpec makeWallMountedCube(int resolution, double reynolds) {
  CaseSpec s;
  s.name = "wall-mounted-cube";
  const double h = 1.0;
  UniformGrid& g = s.grid;
  g.dx = h / resolution;
  g.dy = h / resolution;
  g.dz = h / resolution;
  g.nx = static_cast<int>(std::lround(14.0 * resolution));
  g.ny = static_cast<int>(std::lround(3.0 * resolution));
  g.nz = static_cast<int>(std::lround(6.4 * resolution));
  g.origin = {0.0, 0.0, 0.0};
  g.face[0].type = BoundaryType::Inlet;  // x-min uniform inflow
  g.face[0].profile = InletProfile::Uniform;
  g.face[0].inletSpeed = 1.0;
  g.face[0].inletAxis = 0;
  g.face[1].type = BoundaryType::Outlet;     // x-max
  g.face[2].type = BoundaryType::Wall;       // floor
  g.face[3].type = BoundaryType::Wall;       // ceiling
  g.face[4].type = BoundaryType::Periodic;   // spanwise
  g.face[5].type = BoundaryType::Periodic;
  g.allocate();

  // Cube: front face 3h from inlet, on the floor, centered in span.
  const double x0 = 3.0 * h, x1 = 4.0 * h;
  const double y0 = 0.0, y1 = h;
  const double zc = 3.2 * h, z0 = zc - 0.5 * h, z1 = zc + 0.5 * h;
  for (int k = 0; k < g.nz; ++k) {
    for (int j = 0; j < g.ny; ++j) {
      for (int i = 0; i < g.nx; ++i) {
        const V3 c = g.cellCenter(i, j, k);
        if (c.x >= x0 && c.x <= x1 && c.y >= y0 && c.y <= y1 && c.z >= z0 && c.z <= z1) {
          g.solid[g.cidx(i, j, k)] = 1;
        }
      }
    }
  }

  s.flow.reynolds = reynolds;   // Re_h = U h / nu
  s.flow.refLength = h;
  s.flow.velocityScale = 1.0;
  s.flow.refArea = h * h;       // frontal area
  s.flow.cfl = 0.7;
  s.flow.convection = ConvectionScheme::Weno5;
  s.run.maxSteps = 200;
  s.run.snapshotEvery = 50;
  s.run.checkpointEvery = 50;
  return s;
}

CaseSpec loadCase(const std::string& jsonPath) {
  const json::Value j = json::parseFile(jsonPath);
  const std::string type = j.string("type", "lid-cavity");
  const double Re = j.number("reynolds", 100.0);
  const int res = j.integer("resolution", 64);

  CaseSpec s;
  if (type == "lid-cavity") {
    s = makeLidCavity(res, Re, j.number("lid_velocity", 1.0));
  } else if (type == "channel" || type == "poiseuille") {
    s = makeChannel(j.integer("nx", 6 * res), res, Re, j.number("bulk_velocity", 1.0));
  } else if (type == "wall-mounted-cube") {
    s = makeWallMountedCube(res, Re);
  } else {
    throw std::runtime_error("loadCase: unknown case type '" + type + "'");
  }

  s.name = j.string("name", type);
  s.flow.cfl = j.number("cfl", s.flow.cfl);
  if (const json::Value* c = j.find("convection")) {
    if (c->isString() && c->str == "weno5") {
      s.flow.convection = ConvectionScheme::Weno5;
    } else if (c->isString() && c->str == "central") {
      s.flow.convection = ConvectionScheme::Central;
    }
  }
  s.run.runDir = j.string("run_dir", "run_" + type);
  s.run.maxSteps = j.integer("max_steps", s.run.maxSteps);
  s.run.snapshotEvery = j.integer("snapshot_every", s.run.snapshotEvery);
  s.run.checkpointEvery = j.integer("checkpoint_every", s.run.checkpointEvery);
  s.run.stopTime = j.number("stop_time", s.run.stopTime);
  s.run.steadyTol = j.number("steady_tol", s.run.steadyTol);
  s.run.restartFrom = j.string("restart_from", "");
  return s;
}

// ---------------------------------------------------------------------------
// VTU snapshot (hexahedral cells, ParaView-openable; 2D => one thin layer).
// ---------------------------------------------------------------------------
void writeGridVtu(const NSSolver& solver, const std::string& path) {
  const UniformGrid& g = solver.grid();
  std::ofstream f(path);
  if (!f) {
    throw std::runtime_error("writeGridVtu: cannot open " + path);
  }
  f.precision(8);
  const std::size_t ncell = g.cellCount();
  const std::size_t npts = ncell * 8;
  f << "<?xml version=\"1.0\"?>\n<VTKFile type=\"UnstructuredGrid\" version=\"1.0\" "
       "byte_order=\"LittleEndian\" header_type=\"UInt64\">\n  <UnstructuredGrid>\n";
  f << "    <Piece NumberOfPoints=\"" << npts << "\" NumberOfCells=\"" << ncell << "\">\n";
  f << "      <Points>\n        <DataArray type=\"Float64\" NumberOfComponents=\"3\" "
       "format=\"ascii\">\n";
  for (int k = 0; k < g.nz; ++k) {
    for (int j = 0; j < g.ny; ++j) {
      for (int i = 0; i < g.nx; ++i) {
        const double x0 = g.origin.x + i * g.dx, x1 = x0 + g.dx;
        const double y0 = g.origin.y + j * g.dy, y1 = y0 + g.dy;
        const double z0 = g.origin.z + k * g.dz, z1 = z0 + g.dz;
        f << x0 << ' ' << y0 << ' ' << z0 << ' ' << x1 << ' ' << y0 << ' ' << z0 << ' '
          << x1 << ' ' << y1 << ' ' << z0 << ' ' << x0 << ' ' << y1 << ' ' << z0 << ' '
          << x0 << ' ' << y0 << ' ' << z1 << ' ' << x1 << ' ' << y0 << ' ' << z1 << ' '
          << x1 << ' ' << y1 << ' ' << z1 << ' ' << x0 << ' ' << y1 << ' ' << z1 << '\n';
      }
    }
  }
  f << "        </DataArray>\n      </Points>\n      <Cells>\n";
  f << "        <DataArray type=\"Int64\" Name=\"connectivity\" format=\"ascii\">\n";
  for (std::size_t i = 0; i < npts; ++i) {
    f << i << ((i + 1) % 8 == 0 ? '\n' : ' ');
  }
  f << "        </DataArray>\n        <DataArray type=\"Int64\" Name=\"offsets\" "
       "format=\"ascii\">\n";
  for (std::size_t i = 1; i <= ncell; ++i) {
    f << i * 8 << ' ';
  }
  f << "\n        </DataArray>\n        <DataArray type=\"UInt8\" Name=\"types\" "
       "format=\"ascii\">\n";
  for (std::size_t i = 0; i < ncell; ++i) {
    f << "12 ";
  }
  f << "\n        </DataArray>\n      </Cells>\n      <CellData Scalars=\"speed\">\n";

  const auto dump = [&](const char* name, const std::vector<double>& a) {
    f << "        <DataArray type=\"Float64\" Name=\"" << name << "\" format=\"ascii\">\n";
    for (double x : a) {
      f << x << ' ';
    }
    f << "\n        </DataArray>\n";
  };
  const std::vector<double> u = solver.cellVelocity(0);
  const std::vector<double> v = solver.cellVelocity(1);
  const std::vector<double> w = solver.cellVelocity(2);
  std::vector<double> speed(ncell), solid(ncell);
  for (std::size_t c = 0; c < ncell; ++c) {
    speed[c] = std::sqrt(u[c] * u[c] + v[c] * v[c] + w[c] * w[c]);
    solid[c] = g.solid[c] ? 1.0 : 0.0;
  }
  dump("u", u);
  dump("v", v);
  dump("w", w);
  dump("p", solver.cellPressure());
  dump("speed", speed);
  dump("solid", solid);
  f << "      </CellData>\n    </Piece>\n  </UnstructuredGrid>\n</VTKFile>\n";
}

// ---------------------------------------------------------------------------
// Binary checkpoint (exact bit-for-bit restart, no external dependency).
// ---------------------------------------------------------------------------
namespace {
constexpr char kMagic[8] = {'N', 'B', 'F', 'C', 'K', '0', '1', '\0'};
template <class T>
void wr(std::ofstream& f, const T& v) {
  f.write(reinterpret_cast<const char*>(&v), sizeof(T));
}
template <class T>
void rd(std::ifstream& f, T& v) {
  f.read(reinterpret_cast<char*>(&v), sizeof(T));
}
void wrVec(std::ofstream& f, const std::vector<double>& a) {
  const uint64_t n = a.size();
  wr(f, n);
  if (n) {
    f.write(reinterpret_cast<const char*>(a.data()), static_cast<std::streamsize>(n * 8));
  }
}
std::vector<double> rdVec(std::ifstream& f) {
  uint64_t n = 0;
  rd(f, n);
  std::vector<double> a(n);
  if (n) {
    f.read(reinterpret_cast<char*>(a.data()), static_cast<std::streamsize>(n * 8));
  }
  return a;
}
}  // namespace

void writeCheckpoint(const NSSolver& solver, const std::string& path) {
  std::ofstream f(path, std::ios::binary);
  if (!f) {
    throw std::runtime_error("writeCheckpoint: cannot open " + path);
  }
  f.write(kMagic, 8);
  const UniformGrid& g = solver.grid();
  wr(f, g.nx);
  wr(f, g.ny);
  wr(f, g.nz);
  const int step = solver.stepIndex();
  const double t = solver.time();
  const double dt = solver.dtLast();
  wr(f, step);
  wr(f, t);
  wr(f, dt);
  wrVec(f, solver.uFace());
  wrVec(f, solver.vFace());
  wrVec(f, solver.wFace());
  wrVec(f, solver.pressure());
}

void readCheckpoint(NSSolver& solver, const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    throw std::runtime_error("readCheckpoint: cannot open " + path);
  }
  char magic[8];
  f.read(magic, 8);
  if (std::memcmp(magic, kMagic, 8) != 0) {
    throw std::runtime_error("readCheckpoint: bad magic in " + path);
  }
  int nx, ny, nz, step;
  double t, dt;
  rd(f, nx);
  rd(f, ny);
  rd(f, nz);
  const UniformGrid& g = solver.grid();
  if (nx != g.nx || ny != g.ny || nz != g.nz) {
    throw std::runtime_error("readCheckpoint: grid dimension mismatch");
  }
  rd(f, step);
  rd(f, t);
  rd(f, dt);
  std::vector<double> u = rdVec(f);
  std::vector<double> v = rdVec(f);
  std::vector<double> w = rdVec(f);
  std::vector<double> p = rdVec(f);
  solver.setState(std::move(u), std::move(v), std::move(w), std::move(p), step, t, dt);
}

// ---------------------------------------------------------------------------
// Run loop.
// ---------------------------------------------------------------------------
namespace {
// First line of every fresh diagnostics.jsonl: build provenance, so any result
// file is traceable to the exact solver build that produced it. Consumers key
// step records on the presence of "step" and skip this line.
void appendMeta(std::ofstream& f) {
  f << "{\"event\":\"meta\",\"solver\":\"nabla_solve\",\"solver_version\":\""
    << nabla::kVersionString << "\",\"git_sha\":\"" << nabla::kGitSha << "\"}\n";
}

void appendDiagnostics(std::ofstream& f, const StepReport& r, bool accepted) {
  f << "{\"step\":" << r.step << ",\"t\":" << r.t << ",\"dt\":" << r.dt
    << ",\"cfl\":" << r.cfl << ",\"momentum_residual\":" << r.momentumResidual
    << ",\"continuity_residual\":" << r.continuityResidual
    << ",\"divergence_max\":" << r.divergenceMax << ",\"mass_error\":" << r.massError
    << ",\"cd\":" << r.cd << ",\"cl\":" << r.cl << ",\"poisson_iters\":" << r.poissonIters
    << ",\"poisson_residual\":" << r.poissonResidual << ",\"cells\":" << r.cellCount
    << ",\"accepted\":" << (accepted ? "true" : "false") << "}\n";
}
}  // namespace

RunSummary runCase(const CaseSpec& spec) {
  auto poisson = std::make_shared<MgPoisson>(spec.flow.poissonTol, spec.flow.poissonMaxIters);
  NSSolver solver(spec.grid, spec.flow, poisson);
  solver.initialize();

  std::filesystem::create_directories(spec.run.runDir);
  const bool restart = !spec.run.restartFrom.empty();
  if (restart) {
    readCheckpoint(solver, spec.run.restartFrom);
  }

  const std::string diagPath = spec.run.runDir + "/diagnostics.jsonl";
  std::ofstream diag(diagPath, restart ? std::ios::app : std::ios::trunc);
  if (!restart) {
    appendMeta(diag);
  }

  RunSummary sum;
  sum.runDir = spec.run.runDir;
  sum.cells = solver.grid().cellCount();
  StepReport last;
  for (int s = 0; s < spec.run.maxSteps; ++s) {
    last = solver.step();
    appendDiagnostics(diag, last, last.accepted);
    diag.flush();
    if (spec.run.snapshotEvery > 0 && last.step % spec.run.snapshotEvery == 0) {
      writeGridVtu(solver, spec.run.runDir + "/snapshot_" + std::to_string(last.step) + ".vtu");
    }
    if (spec.run.checkpointEvery > 0 && last.step % spec.run.checkpointEvery == 0) {
      writeCheckpoint(solver,
                      spec.run.runDir + "/checkpoint_" + std::to_string(last.step) + ".ckpt");
    }
    if (spec.run.stopTime > 0.0 && solver.time() >= spec.run.stopTime) {
      break;
    }
    if (spec.run.steadyTol > 0.0 && last.step > 20 &&
        last.momentumResidual < spec.run.steadyTol) {
      sum.steady = true;
      break;
    }
  }

  writeGridVtu(solver, spec.run.runDir + "/final.vtu");
  writeCheckpoint(solver, spec.run.runDir + "/final.ckpt");

  sum.steps = last.step;
  sum.time = last.t;
  sum.momentumResidual = last.momentumResidual;
  sum.continuityResidual = last.continuityResidual;
  sum.massError = last.massError;
  sum.cd = last.cd;
  sum.cl = last.cl;
  return sum;
}

}  // namespace nabla::flow
