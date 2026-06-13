#pragma once

#include <string>

#include "nabla/config.hpp"
#include "nabla/field.hpp"
#include "nabla/solver.hpp"

// File-based IPC helpers. The contract between the orchestration backend and
// the solver core is plain text on disk:
//
//   backend  --writes-->  spec file  (key = value)  --read_config-->  core
//   core     --writes-->  result file (key = value) --parsed by-->    backend
//
// This keeps the two layers decoupled and lets the core stay a self-contained
// binary that never has to ship to clients. See ../CLAUDE.md.
namespace nabla::io {

// Parse a `key = value` spec. Lines may carry `#` comments. Unknown keys are
// ignored; missing keys retain their SimConfig defaults.
[[nodiscard]] SimConfig parse_config(const std::string& text);

// Read and parse a spec file. Throws std::runtime_error if it cannot be opened.
[[nodiscard]] SimConfig read_config(const std::string& path);

// Write a machine-readable `key = value` result summary for the backend.
void write_result(const std::string& path, const SolveResult& result);

// Dump the raw field as whitespace-separated rows (used by /validation).
void write_field(const std::string& path, const Field2D& field);

}  // namespace nabla::io
