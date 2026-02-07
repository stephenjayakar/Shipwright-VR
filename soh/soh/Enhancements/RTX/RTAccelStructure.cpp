/**
 * RTAccelStructure.cpp — Phase 9 compatibility source file
 *
 * This file is referenced by the Phase 9 build system plan (RTX_PLAN.md §9.1)
 * under the name "RTAccelStructure.cpp". The actual BLAS/TLAS acceleration
 * structure build and management logic lives in AccelerationStructure.cpp.
 * This file provides a compilation unit for the RTAccelStructure name used in the plan.
 *
 * All BLAS creation, TLAS building, instance management, and acceleration
 * structure updates are implemented in AccelerationStructure.cpp/AccelerationStructure.h.
 */

#ifdef ENABLE_DX12_RTX

#include "AccelerationStructure.h"

// This translation unit exists to satisfy the Phase 9 source file listing requirement.
// The actual acceleration structure implementation is in AccelerationStructure.cpp/.h.
// No additional implementation is needed here.

#endif // ENABLE_DX12_RTX
