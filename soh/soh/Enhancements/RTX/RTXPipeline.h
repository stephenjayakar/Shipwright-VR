/**
 * RTXPipeline.h — Phase 9 compatibility header
 *
 * This file is referenced by the Phase 9 build system plan (RTX_PLAN.md §9.1)
 * under the name "RTXPipeline.h". The actual DXR pipeline state object,
 * root signature, and shader table implementation lives in DXRPipeline.h.
 * This header re-exports DXRPipeline.h for compatibility with the planned naming.
 */

#pragma once
#ifndef RTX_PIPELINE_H
#define RTX_PIPELINE_H

#ifdef ENABLE_DX12_RTX
#include "DXRPipeline.h"
#endif // ENABLE_DX12_RTX

#endif // RTX_PIPELINE_H
