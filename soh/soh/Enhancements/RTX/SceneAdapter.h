/**
 * SceneAdapter.h — Phase 9 compatibility header
 *
 * This file is referenced by the Phase 9 build system plan (RTX_PLAN.md §9.1)
 * under the name "SceneAdapter". The actual N64 display list geometry extraction
 * and scene conversion logic lives in SceneGeometryExtractor.h.
 * This header re-exports SceneGeometryExtractor.h for compatibility with the planned naming.
 */

#pragma once
#ifndef SCENE_ADAPTER_H
#define SCENE_ADAPTER_H

#ifdef ENABLE_DX12_RTX
#include "SceneGeometryExtractor.h"
#endif // ENABLE_DX12_RTX

#endif // SCENE_ADAPTER_H
