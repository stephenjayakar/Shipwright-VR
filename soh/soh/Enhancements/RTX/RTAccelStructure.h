/**
 * RTAccelStructure.h — Phase 9 compatibility header
 *
 * This file is referenced by the Phase 9 build system plan (RTX_PLAN.md §9.1)
 * under the name "RTAccelStructure". The actual BLAS/TLAS acceleration
 * structure management lives in AccelerationStructure.h.
 * This header re-exports AccelerationStructure.h for compatibility with the planned naming.
 */

#pragma once
#ifndef RT_ACCEL_STRUCTURE_H
#define RT_ACCEL_STRUCTURE_H

#ifdef ENABLE_DX12_RTX
#include "AccelerationStructure.h"
#endif // ENABLE_DX12_RTX

#endif // RT_ACCEL_STRUCTURE_H
