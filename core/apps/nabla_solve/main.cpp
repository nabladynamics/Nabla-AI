#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <ostream>
#include <string>
#include <string_view>

#include "nabla/adaptive/run.hpp"
#include "nabla/config.hpp"
#include "nabla/field.hpp"
#include "nabla/flow/case.hpp"
#include "nabla/geometry/ingest.hpp"
#include "nabla/io.hpp"
#include "nabla/solver.hpp"
#include "nabla/version.hpp"

namespace {

void print_version() {
  std::cout << "nabla_solve " << nabla::kVersionString << " (git " << nabla::kGitSha << ")\n";
}

void print_usage(std::ostream& os) {
  os << "Nabla AI \xE2\x80\x94 physics-adaptive CFD solver core\n\n"
        "Usage:\n"
        "  nabla_solve --version\n"
        "  nabla_solve --input <spec> [--output <result>] [--field <dump>]\n"
        "  nabla_solve ingest <file.stl> [--case <name>] [options]\n\n"
        "Solve options:\n"
        "  -i, --input  <path>   simulation spec file (key = value)\n"
        "  -o, --output <path>   result summary file (default: stdout)\n"
        "  -f, --field  <path>   dump the final field to this file\n"
        "  -v, --version         print version and exit\n"
        "  -h, --help            show this help and exit\n\n"
        "ingest options:\n"
        "      --case <name>        case template (default: wall-mounted-cube)\n"
        "      --out-vtu <path>     meshed domain output (default: <stem>.vtu)\n"
        "      --report <path>      geometry report JSON (default: <stem>.geometry.json)\n"
        "      --resolution <n>     surface cells across the feature scale (default: 8)\n"
        "      --base-level <n>     uniform far-field level (default: 3)\n"
        "      --max-level <n>      octree level cap (default: 11)\n"
        "      --edge-extra <n>     extra refinement levels at sharp edges (default: 2)\n"
        "      --sharp-angle <deg>  dihedral threshold for sharp edges (default: 30)\n";
}

// Stage-1 geometry ingestion subcommand.
int run_ingest(int argc, char** argv) {
  nabla::geometry::IngestOptions opt;
  std::string stl;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto val = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "error: missing value for " << name << '\n';
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--case") {
      opt.caseName = val("--case");
    } else if (a == "--out-vtu") {
      opt.vtuPath = val("--out-vtu");
    } else if (a == "--report") {
      opt.reportPath = val("--report");
    } else if (a == "--resolution") {
      opt.resolution = std::stod(val("--resolution"));
    } else if (a == "--base-level") {
      opt.baseLevel = std::stoi(val("--base-level"));
    } else if (a == "--max-level") {
      opt.maxLevel = std::stoi(val("--max-level"));
    } else if (a == "--edge-extra") {
      opt.edgeExtraLevels = std::stoi(val("--edge-extra"));
    } else if (a == "--sharp-angle") {
      opt.sharpAngleDeg = std::stod(val("--sharp-angle"));
    } else if (!a.empty() && a.front() == '-') {
      std::cerr << "error: unknown ingest option '" << a << "'\n";
      return 2;
    } else {
      stl = std::string(a);
    }
  }
  if (stl.empty()) {
    std::cerr << "error: ingest requires an STL path\n"
                 "usage: nabla_solve ingest <file.stl> [--case wall-mounted-cube]\n";
    return 2;
  }
  const std::filesystem::path p(stl);
  const std::string stem = p.stem().string();
  if (opt.vtuPath.empty()) {
    opt.vtuPath = stem + ".vtu";
  }
  if (opt.reportPath.empty()) {
    opt.reportPath = stem + ".geometry.json";
  }

  try {
    const nabla::geometry::IngestSummary s = nabla::geometry::runIngest(stl, opt);
    std::cout << "Nabla AI ingest \xE2\x80\x94 case " << opt.caseName << '\n'
              << "  cube height h : " << s.cubeHeight << '\n'
              << "  cells         : " << s.cells << " (inside " << s.insideSolid
              << ", cut " << s.cut << ", fluid " << s.fluid << ")\n"
              << "  levels        : " << s.minLevel << " .. " << s.maxLevel << '\n'
              << "  watertight    : " << (s.watertight ? "yes" : "NO (reported)") << '\n'
              << "  sharp edges   : " << s.sharpEdges << ",  corners: " << s.corners << '\n'
              << "  2:1 balanced  : " << (s.balanced ? "yes" : "NO") << '\n'
              << "  wrote VTU     : " << s.vtuPath << "  (open in ParaView)\n"
              << "  wrote report  : " << s.reportPath << '\n';
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
    return 1;
  }
}

void apply_dirichlet(nabla::Field2D& field, double boundary_temp) {
  const std::size_t nx = field.nx();
  const std::size_t ny = field.ny();
  for (std::size_t i = 0; i < nx; ++i) {
    field.at(i, 0) = boundary_temp;
    field.at(i, ny - 1) = boundary_temp;
  }
  for (std::size_t j = 0; j < ny; ++j) {
    field.at(0, j) = boundary_temp;
    field.at(nx - 1, j) = boundary_temp;
  }
}

}  // namespace

// Navier–Stokes run subcommand: nabla_solve run case.json [--restart ckpt]
int run_case_cli(int argc, char** argv) {
  std::string caseFile;
  std::string restart;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    if (a == "--restart") {
      if (i + 1 >= argc) {
        std::cerr << "error: missing value for --restart\n";
        return 2;
      }
      restart = argv[++i];
    } else if (!a.empty() && a.front() == '-') {
      std::cerr << "error: unknown run option '" << a << "'\n";
      return 2;
    } else {
      caseFile = std::string(a);
    }
  }
  if (caseFile.empty()) {
    std::cerr << "error: run requires a case JSON path\n"
                 "usage: nabla_solve run <case.json> [--restart <checkpoint>]\n";
    return 2;
  }
  try {
    nabla::flow::CaseSpec spec = nabla::flow::loadCase(caseFile);
    if (!restart.empty()) {
      spec.run.restartFrom = restart;
    }
    const nabla::flow::RunSummary s = nabla::flow::runCase(spec);
    std::cout << "Nabla AI run \xE2\x80\x94 case " << spec.name << '\n'
              << "  cells         : " << s.cells << '\n'
              << "  steps         : " << s.steps << "  (t = " << s.time << ")\n"
              << "  momentum res  : " << s.momentumResidual << '\n'
              << "  continuity    : " << s.continuityResidual << '\n'
              << "  mass error    : " << s.massError << '\n'
              << "  Cd, Cl        : " << s.cd << ", " << s.cl << '\n'
              << "  steady        : " << (s.steady ? "yes" : "no") << '\n'
              << "  run dir       : " << s.runDir
              << "  (diagnostics.jsonl, *.vtu, *.ckpt)\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
    return 1;
  }
}

// Adaptive (physics-aware AMR + audited model classification) subcommand.
int run_adapt_cli(int argc, char** argv) {
  nabla::adaptive::AdaptiveRunOptions o;
  for (int i = 2; i < argc; ++i) {
    const std::string_view a = argv[i];
    const auto val = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "error: missing value for " << name << '\n';
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--res") {
      o.resolution = std::stoi(val("--res"));
    } else if (a == "--re") {
      o.reynolds = std::stod(val("--re"));
    } else if (a == "--max-level") {
      o.octreeMaxLevel = std::stoi(val("--max-level"));
    } else if (a == "--base-level") {
      o.octreeBaseLevel = std::stoi(val("--base-level"));
    } else if (a == "--warmup") {
      o.warmupSteps = std::stoi(val("--warmup"));
    } else if (a == "--steps") {
      o.adaptiveSteps = std::stoi(val("--steps"));
    } else if (a == "--run-dir") {
      o.runDir = val("--run-dir");
    } else {
      std::cerr << "error: unknown adapt option '" << a << "'\n";
      return 2;
    }
  }
  try {
    const nabla::adaptive::AdaptiveRunResult r = nabla::adaptive::runAdaptiveCube(o);
    std::cout << "Nabla AI adapt \xE2\x80\x94 wall-mounted cube (adaptivity ON)\n"
              << "  Cd, Cl            : " << r.cd << ", " << r.cl << '\n'
              << "  adaptive cells    : " << r.adaptiveCells << '\n'
              << "  uniform-fine cells: " << r.uniformFineCells << '\n'
              << "  cell speedup      : " << r.cellSpeedup << "x\n"
              << "  audit             : " << r.audit.steps << " steps, " << r.audit.refinements
              << " refines, " << r.audit.coarsenings << " coarsens, " << r.audit.modelChanges
              << " model changes, " << r.audit.rejects << " rejections\n"
              << "  run dir           : " << r.runDir
              << "  (audit.jsonl, audit_summary.md, efficiency.txt, adaptive_final.vtu)\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
    return 1;
  }
}

int main(int argc, char** argv) {
  // Subcommand dispatch.
  if (argc >= 2 && std::string_view(argv[1]) == "ingest") {
    return run_ingest(argc, argv);
  }
  if (argc >= 2 && std::string_view(argv[1]) == "run") {
    return run_case_cli(argc, argv);
  }
  if (argc >= 2 && std::string_view(argv[1]) == "adapt") {
    return run_adapt_cli(argc, argv);
  }

  std::string input;
  std::string output;
  std::string field_out;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    const auto take_value = [&](const char* name) -> std::string {
      if (i + 1 >= argc) {
        std::cerr << "error: missing value for " << name << '\n';
        std::exit(2);
      }
      return argv[++i];
    };

    if (arg == "-v" || arg == "--version") {
      print_version();
      return 0;
    }
    if (arg == "-h" || arg == "--help") {
      print_usage(std::cout);
      return 0;
    }
    if (arg == "-i" || arg == "--input") {
      input = take_value("--input");
    } else if (arg == "-o" || arg == "--output") {
      output = take_value("--output");
    } else if (arg == "-f" || arg == "--field") {
      field_out = take_value("--field");
    } else {
      std::cerr << "error: unknown argument '" << arg << "'\n\n";
      print_usage(std::cerr);
      return 2;
    }
  }

  if (input.empty()) {
    std::cerr << "error: no --input spec provided\n\n";
    print_usage(std::cerr);
    return 2;
  }

  try {
    const nabla::SimConfig cfg = nabla::io::read_config(input);
    nabla::Field2D field(cfg.nx, cfg.ny, cfg.initial_temp);
    apply_dirichlet(field, cfg.boundary_temp);

    const nabla::DiffusionSolver solver(cfg);
    const nabla::SolveResult result = solver.solve(field);

    if (!result.stable) {
      std::cerr << "warning: explicit scheme is unstable (stability number "
                << result.stability << " > " << nabla::DiffusionSolver::kStableLimit
                << ")\n";
    }

    if (output.empty()) {
      std::cout << "steps_run = " << result.steps_run << '\n'
                << "final_max = " << result.final_max << '\n'
                << "final_mean = " << result.final_mean << '\n'
                << "stability = " << result.stability << '\n'
                << "stable = " << (result.stable ? "true" : "false") << '\n';
    } else {
      nabla::io::write_result(output, result);
    }

    if (!field_out.empty()) {
      nabla::io::write_field(field_out, field);
    }
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << '\n';
    return 1;
  }

  return 0;
}
