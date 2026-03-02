/**
 * SceneAdapter.cpp — Phase 9 compatibility source file
 *
 * This file is referenced by the Phase 9 build system plan (RTX_PLAN.md §9.1)
 * under the name "SceneAdapter.cpp". The actual N64 display list geometry
 * extraction and scene adaptation logic lives in SceneGeometryExtractor.cpp.
 * This file provides a compilation unit for the SceneAdapter name used in the plan.
 *
 * All N64 vertex/triangle extraction, display list parsing, coordinate
 * transformation, and BLAS-ready geometry generation are implemented in
 * SceneGeometryExtractor.cpp/SceneGeometryExtractor.h.
 */

#ifdef ENABLE_DX12_RTX

#include "SceneGeometryExtractor.h"

// This translation unit exists to satisfy the Phase 9 source file listing requirement.
// The actual scene adapter implementation is in SceneGeometryExtractor.cpp/.h.
// No additional implementation is needed here.

#endif // ENABLE_DX12_RTX
