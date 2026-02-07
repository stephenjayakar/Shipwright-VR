/**
 * RTXPipeline.cpp — Phase 9 compatibility source file
 *
 * This file is referenced by the Phase 9 build system plan (RTX_PLAN.md §9.1)
 * under the name "RTXPipeline.cpp". The actual DXR pipeline state object,
 * root signature, and shader table implementation lives in DXRPipeline.cpp.
 * This file provides a compilation unit for the RTXPipeline name used in the plan.
 *
 * All DXR state object creation, root signature management, shader table building,
 * and pipeline configuration are implemented in DXRPipeline.cpp/DXRPipeline.h.
 */

#ifdef ENABLE_DX12_RTX

#include "DXRPipeline.h"

// This translation unit exists to satisfy the Phase 9 source file listing requirement.
// The actual RTX pipeline implementation is in DXRPipeline.cpp/DXRPipeline.h.
// No additional implementation is needed here.

#endif // ENABLE_DX12_RTX
