#pragma once
#ifndef DX12_CONTEXT_H
#define DX12_CONTEXT_H

#ifdef ENABLE_DX12_RTX

// DX12Context.h — Compatibility header
// Maps DX12Context to DX12Device for code that references the Phase 6 task-spec naming.
// The actual implementation lives in DX12Device.h / DX12Device.cpp.

#include "DX12Device.h"

namespace RTX {

// Phase 6 task-spec uses "DX12Context" as the device context class name.
// Our implementation uses DX12Device. Provide a type alias for compatibility.
using DX12Context = DX12Device;

} // namespace RTX

// Also provide the alias in the SOH::RTX namespace for callers using that convention.
namespace SOH {
namespace RTX {
    using DX12Context = ::RTX::DX12Device;
} // namespace RTX
} // namespace SOH

#endif // ENABLE_DX12_RTX
#endif // DX12_CONTEXT_H
