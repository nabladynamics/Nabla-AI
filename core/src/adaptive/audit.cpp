#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>

#include "nabla/adaptive/adaptive.hpp"
#include "nabla/version.hpp"

namespace nabla::adaptive {

AuditTrail::AuditTrail(std::string path) : path_(std::move(path)) {
  auto f = std::make_shared<std::ofstream>(path_, std::ios::trunc);
  if (!*f) {
    throw std::runtime_error("AuditTrail: cannot open " + path_);
  }
  out_ = f;
  // Build provenance first, so the audit trail is traceable to an exact build.
  emit(std::string{"{\"event\":\"meta\",\"solver\":\"nabla_solve\",\"solver_version\":\""} +
       kVersionString + "\",\"git_sha\":\"" + kGitSha + "\"}");
}
AuditTrail::~AuditTrail() = default;

void AuditTrail::emit(const std::string& json) {
  auto* f = static_cast<std::ofstream*>(out_.get());
  (*f) << json << '\n';
  f->flush();
}

namespace {
std::string esc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '"' || c == '\\') {
      o.push_back('\\');
    }
    o.push_back(c);
  }
  return o;
}
}  // namespace

void AuditTrail::beginStep(int step, double t, double dt) {
  step_ = step;
  ++totals_.steps;
  std::ostringstream os;
  os << "{\"event\":\"step_begin\",\"step\":" << step << ",\"t\":" << t << ",\"dt\":" << dt
     << "}";
  emit(os.str());
}

void AuditTrail::metrics(std::size_t cells, double cd, double cl) {
  std::ostringstream os;
  os << "{\"event\":\"metrics\",\"step\":" << step_ << ",\"cells\":" << cells
     << ",\"cd\":" << cd << ",\"cl\":" << cl << "}";
  emit(os.str());
}

void AuditTrail::refinements(std::size_t count, const std::string& reason) {
  totals_.refinements += count;
  std::ostringstream os;
  os << "{\"event\":\"refine\",\"step\":" << step_ << ",\"count\":" << count
     << ",\"reason\":\"" << esc(reason) << "\"}";
  emit(os.str());
}

void AuditTrail::coarsenings(std::size_t count, const std::string& reason) {
  totals_.coarsenings += count;
  std::ostringstream os;
  os << "{\"event\":\"coarsen\",\"step\":" << step_ << ",\"count\":" << count
     << ",\"reason\":\"" << esc(reason) << "\"}";
  emit(os.str());
}

void AuditTrail::modelChange(std::uint64_t cellMorton, Mode from, Mode to,
                             const std::string& reason) {
  ++totals_.modelChanges;
  std::ostringstream os;
  os << "{\"event\":\"model_change\",\"step\":" << step_ << ",\"cell\":" << cellMorton
     << ",\"from\":\"" << modeName(from) << "\",\"to\":\"" << modeName(to)
     << "\",\"reason\":\"" << esc(reason) << "\"}";
  emit(os.str());
}

void AuditTrail::regionAcceptance(const std::string& region, Mode mode, bool accepted,
                                  const std::string& reason) {
  if (accepted) {
    ++totals_.accepts;
  } else {
    ++totals_.rejects;
    ++totals_.promotions;  // rejection => promote to FULL_NS
  }
  std::ostringstream os;
  os << "{\"event\":\"acceptance\",\"step\":" << step_ << ",\"region\":\"" << esc(region)
     << "\",\"mode\":\"" << modeName(mode) << "\",\"accepted\":" << (accepted ? "true" : "false")
     << ",\"reason\":\"" << esc(reason) << "\"}";
  emit(os.str());
}

void AuditTrail::stepDecision(const std::string& decision, const std::string& detail) {
  std::ostringstream os;
  os << "{\"event\":\"step_decision\",\"step\":" << step_ << ",\"decision\":\""
     << esc(decision) << "\",\"detail\":\"" << esc(detail) << "\"}";
  emit(os.str());
}

void AuditTrail::endStep() {
  std::ostringstream os;
  os << "{\"event\":\"step_end\",\"step\":" << step_ << "}";
  emit(os.str());
}

void AuditTrail::writeSummary(const std::string& path) const {
  std::ofstream f(path);
  if (!f) {
    throw std::runtime_error("AuditTrail::writeSummary: cannot open " + path);
  }
  f << "# Nabla AI — Adaptive Decision Audit Summary\n\n";
  f << "Solver build: " << kVersionString << " (git " << kGitSha << ")\n\n";
  f << "| metric | count |\n| ------ | ----- |\n";
  f << "| steps | " << totals_.steps << " |\n";
  f << "| refinements | " << totals_.refinements << " |\n";
  f << "| coarsenings | " << totals_.coarsenings << " |\n";
  f << "| model-label changes | " << totals_.modelChanges << " |\n";
  f << "| reduced-model accepts | " << totals_.accepts << " |\n";
  f << "| reduced-model rejects | " << totals_.rejects << " |\n";
  f << "| promotions to FULL_NS | " << totals_.promotions << " |\n\n";
  f << "Per-event detail is in the accompanying `audit.jsonl`.\n";
}

// ---------------------------------------------------------------------------
// Efficiency monitor
// ---------------------------------------------------------------------------
void EfficiencyMonitor::record(std::size_t cells, double wallSeconds) {
  cells_.push_back(cells);
  wall_ += wallSeconds;
}
double EfficiencyMonitor::meanCells() const {
  if (cells_.empty()) {
    return 0.0;
  }
  return static_cast<double>(std::accumulate(cells_.begin(), cells_.end(), std::size_t{0})) /
         static_cast<double>(cells_.size());
}
double EfficiencyMonitor::cellSpeedup() const {
  const double m = meanCells();
  return m > 0.0 ? static_cast<double>(uniformFine_) / m : 0.0;
}
std::string EfficiencyMonitor::report() const {
  std::ostringstream os;
  os.setf(std::ios::fixed);
  os.precision(1);
  os << "uniform-fine cells = " << uniformFine_ << ", adaptive mean cells = " << meanCells()
     << ", cell speedup = " << cellSpeedup() << "x, wall = " << totalWallSeconds() << " s";
  return os.str();
}

}  // namespace nabla::adaptive
