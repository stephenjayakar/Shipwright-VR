/**
 * DX12Context.cpp — Phase 9 compatibility source file
 *
 * This file is referenced by the Phase 9 build system plan (RTX_PLAN.md §9.1)
 * under the name "DX12Context.cpp". The actual DX12 device/context implementation
 * lives in DX12Device.cpp. This file provides the DX12Context type alias and
 * ensures the DX12Context.h compatibility header is compiled as a translation unit.
 *
 * All DX12 device initialization, swap chain creation, command queue management,
 * and resource allocation are implemented in DX12Device.cpp/DX12Device.h.
 * DX12Context is a type alias for DX12Device (see DX12Context.h).
 */

#ifdef ENABLE_DX12_RTX

#include "DX12Context.h"

// This translation unit exists to satisfy the Phase 9 source file listing requirement.
// The DX12Context type alias is defined in DX12Context.h and maps to DX12Device.
// No additional implementation is needed here — all functionality is in DX12Device.cpp.

#endif // ENABLE_DX12_RTX
