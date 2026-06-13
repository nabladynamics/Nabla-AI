#pragma once

#include "nabla/geometry/features.hpp"
#include "nabla/geometry/solid_geometry.hpp"
#include "nabla/mesh/octree.hpp"

namespace nabla::geometry {

// Label names written into the octree's registries by classification.
inline constexpr const char* kWallNoSlipLabel = "wall_noslip";    // uint8, 1 on CUT cells
inline constexpr const char* kNearSharpEdgeScalar = "near_sharp_edge";  // double, 1.0 near a sharp edge

// Classify every leaf as FLUID / CUT / INSIDE_SOLID. CUT cells (cells the
// surface passes through) are tagged with the no-slip wall flag. Cells touching
// a sharp edge get near_sharp_edge = 1 (for visualization / BC tagging).
void classifyOntoOctree(mesh::Octree& tree, const SolidGeometry& solid,
                        const FeatureSet& features);

// Geometry-driven refinement. Refines CUT cells until their largest dimension
// is <= surfaceTarget; cells touching a sharp edge are refined further, down to
// edgeTarget (sharp edges are separation points and must not be rounded).
// Returns the number of refinement passes performed.
int geometryRefine(mesh::Octree& tree, const SolidGeometry& solid,
                   const FeatureSet& features, double surfaceTarget, double edgeTarget);

}  // namespace nabla::geometry
