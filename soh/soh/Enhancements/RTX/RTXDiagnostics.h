#pragma once
// RTXDiagnostics.h — Comprehensive diagnostic logging for the RTX pipeline.
//
// Provides two key diagnostic functions:
//   RTX_DumpDiagnostics() — Logs the full RTX pipeline state: DX12 device,
//     TLAS, shaders, pipeline state object, texture manager, GI system, etc.
//   RTX_LogFrameStats() — Logs per-frame info: triangle counts, BLAS instances,
//     dispatch status, denoise passes, and accumulation state.
//
// Usage: Include this header from any RTX .cpp file and call the functions.
// All output goes through RTX_DIAG() (see RTXDiagLog.h) which writes to:
//   - OutputDebugString() on Windows (visible in VS Output / DebugView)
//   - printf() to stdout (visible if running with a console)
//   - logs/rtx_diag.log (always captured)
//
// These are standalone functions (not methods on any class) so they can be
// called from hooks, debug UIs, or crash handlers without coupling.

#ifndef RTX_DIAGNOSTICS_H
#define RTX_DIAGNOSTICS_H

#include <cstdint>

#ifdef ENABLE_DX12_RTX

// Forward declarations — we do NOT include the full headers here to keep
// this header lightweight. The .cpp file includes everything it needs.

namespace RTX {
    class DX12Device;
    class DXRPipeline;
    class AccelerationStructure;
    class RTXRenderer;
    class RTXManager;
    class GISystem;
    class TextureManager;
    class SceneGeometryExtractor;
}

// ============================================================================
// Primary Diagnostic Functions
// ============================================================================

// Dump a comprehensive snapshot of the entire RTX pipeline state.
// Logs: DX12 device status, raytracing support, swap chain, command queue,
//       descriptor heaps, DXR state object, root signatures, shader tables,
//       output buffers, TLAS/BLAS status, texture manager cache stats,
//       GI system state, scene config, and active scene info.
//
// Safe to call at any time, including before initialization (will report
// "not initialized" for uninitialized components).
void RTX_DumpDiagnostics();

// Log per-frame statistics for the RTX renderer.
// Logs: number of triangles extracted, number of BLAS instances,
//       whether raytracing was dispatched, denoise pass count,
//       accumulation frame count, camera movement, texture cache stats,
//       and frame timing.
//
// Parameters:
//   frameNumber — the current frame number (for log identification).
//                 Pass 0 to auto-increment an internal counter.
void RTX_LogFrameStats(uint32_t frameNumber = 0);

// Dump diagnostics to a specific file path (in addition to the normal log).
// Useful for generating a snapshot file that can be shared for debugging.
void RTX_DumpDiagnosticsToFile(const char* filePath);

// Quick one-line status check — returns a human-readable status string.
// Example: "RTX OK: DX12+DXR active, 1220 tris, 1 BLAS instance, dispatching"
// Example: "RTX WARN: DX12 init but no TLAS (no geometry loaded)"
// Example: "RTX ERR: DX12 device not initialized"
// The returned pointer is to a static buffer and is valid until the next call.
const char* RTX_GetStatusLine();

#else // !ENABLE_DX12_RTX

// No-op stubs when RTX is not enabled
static inline void RTX_DumpDiagnostics() {}
static inline void RTX_LogFrameStats(unsigned int frameNumber = 0) { (void)frameNumber; }
static inline void RTX_DumpDiagnosticsToFile(const char* filePath) { (void)filePath; }
static inline const char* RTX_GetStatusLine() { return "RTX not compiled (ENABLE_DX12_RTX=OFF)"; }

#endif // ENABLE_DX12_RTX

#endif // RTX_DIAGNOSTICS_H
