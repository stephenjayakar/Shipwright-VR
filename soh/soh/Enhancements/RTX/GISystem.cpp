#ifdef ENABLE_DX12_RTX

#include "GISystem.h"
#include "RTXSceneConfig.h"
#include "RTXDiagLog.h"
#include <cstring>
#include <cmath>
#include <algorithm>
#ifdef _WIN32
#include <Windows.h>
#endif

namespace RTX {

// ============================================================================
// SH Constants
// ============================================================================

// SH basis function constants for order 1 (4 coefficients):
// Y00 = 0.282095 (constant)
// Y1-1 = 0.488603 * y
// Y10  = 0.488603 * z
// Y11  = 0.488603 * x
static constexpr float SH_Y00  = 0.282095f;
static constexpr float SH_Y1x  = 0.488603f;

// Pi constant
static constexpr float PI = 3.14159265358979f;

// ============================================================================
// Construction / Destruction
// ============================================================================

GISystem::GISystem()
    : m_accumulationFrameCount(0)
    , m_maxAccumulationFrames(DEFAULT_MAX_ACCUMULATION_FRAMES)
    , m_cameraMoved(false)
    , m_initialized(false)
    , m_giIntensity(1.0f)
    , m_ambientMinIntensity(0.10f)
    , m_colorSigma(DEFAULT_COLOR_SIGMA)
    , m_normalSigma(DEFAULT_NORMAL_SIGMA)
    , m_depthSigma(DEFAULT_DEPTH_SIGMA)
    , m_blendAlpha(DEFAULT_BLEND_ALPHA)
    , m_probeDensity(DEFAULT_PROBE_DENSITY)
    , m_bounceCount(1)
    , m_gridX(0), m_gridY(0), m_gridZ(0)
    , m_nextProbeToUpdate(0)
    , m_probesPerFrame(MAX_PROBES_PER_FRAME)
    , m_globalFrameCounter(0) {
    memset(m_prevViewMatrix, 0, sizeof(m_prevViewMatrix));
}

GISystem::~GISystem() {
    Shutdown();
}

// ============================================================================
// Lifecycle
// ============================================================================

bool GISystem::Initialize(DX12Device* device) {
    RTX_DIAG("GISystem::Initialize() device=%p", (void*)device);
    if (!device) {
        RTX_DIAG("GISystem::Initialize() FAILED - null device");
        OutputDebugStringA("[RTX] CRITICAL: null device in GISystem::Initialize\n");
        return false;
    }

    m_device = device;
    m_accumulationFrameCount = 0;
    m_maxAccumulationFrames = DEFAULT_MAX_ACCUMULATION_FRAMES;
    m_cameraMoved = false;
    m_giIntensity = 1.0f;
    m_colorSigma = DEFAULT_COLOR_SIGMA;
    m_normalSigma = DEFAULT_NORMAL_SIGMA;
    m_depthSigma = DEFAULT_DEPTH_SIGMA;
    m_blendAlpha = DEFAULT_BLEND_ALPHA;
    m_probeDensity = DEFAULT_PROBE_DENSITY;
    m_bounceCount = 1;
    m_probes.clear();
    m_gridX = m_gridY = m_gridZ = 0;
    m_nextProbeToUpdate = 0;
    m_probesPerFrame = MAX_PROBES_PER_FRAME;
    m_globalFrameCounter = 0;
    memset(m_prevViewMatrix, 0, sizeof(m_prevViewMatrix));
    m_needsGPUBufferClear = true;  // Clear buffers on first use
    m_initialized = true;

    RTX_DIAG("GISystem::Initialize() SUCCESS (maxAccum=%u, colorSigma=%.4f, normalSigma=%.1f, depthSigma=%.4f, blendAlpha=%.3f)",
             m_maxAccumulationFrames, m_colorSigma, m_normalSigma, m_depthSigma, m_blendAlpha);
    return true;
}

void GISystem::Shutdown() {
    RTX_DIAG("GISystem::Shutdown() probes=%zu, initialized=%s", m_probes.size(), m_initialized ? "yes" : "no");
    m_device = nullptr;
    m_accumulationFrameCount = 0;
    m_cameraMoved = false;
    m_initialized = false;
    m_giIntensity = 1.0f;
    m_probeDensity = DEFAULT_PROBE_DENSITY;
    m_bounceCount = 1;
    m_probes.clear();
    m_gridX = m_gridY = m_gridZ = 0;
    m_nextProbeToUpdate = 0;
    m_probesPerFrame = MAX_PROBES_PER_FRAME;
    m_globalFrameCounter = 0;
    memset(m_prevViewMatrix, 0, sizeof(m_prevViewMatrix));
}

// ============================================================================
// Probe Placement
// ============================================================================

uint32_t GISystem::PlaceProbes(const AABB& sceneBounds, float probeDensity) {
    RTX_DIAG("GISystem::PlaceProbes() bounds=(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f) density=%.1f",
             sceneBounds.minBounds[0], sceneBounds.minBounds[1], sceneBounds.minBounds[2],
             sceneBounds.maxBounds[0], sceneBounds.maxBounds[1], sceneBounds.maxBounds[2],
             probeDensity);
    m_sceneBounds = sceneBounds;
    m_probeDensity = probeDensity;
    m_probes.clear();

    // Compute scene extents
    float extentX = sceneBounds.maxBounds[0] - sceneBounds.minBounds[0];
    float extentY = sceneBounds.maxBounds[1] - sceneBounds.minBounds[1];
    float extentZ = sceneBounds.maxBounds[2] - sceneBounds.minBounds[2];

    // Avoid degenerate cases
    if (extentX < 1.0f) extentX = 1.0f;
    if (extentY < 1.0f) extentY = 1.0f;
    if (extentZ < 1.0f) extentZ = 1.0f;

    // Ensure density is positive
    if (probeDensity < 1.0f) probeDensity = 1.0f;

    // Calculate grid dimensions
    m_gridX = static_cast<uint32_t>(ceilf(extentX / probeDensity));
    m_gridY = static_cast<uint32_t>(ceilf(extentY / probeDensity));
    m_gridZ = static_cast<uint32_t>(ceilf(extentZ / probeDensity));

    // Clamp to at least 1 in each dimension
    if (m_gridX < 1) m_gridX = 1;
    if (m_gridY < 1) m_gridY = 1;
    if (m_gridZ < 1) m_gridZ = 1;

    // Check against max probes
    uint32_t totalProbes = m_gridX * m_gridY * m_gridZ;
    if (totalProbes > MAX_PROBES) {
        // Scale down grid proportionally
        float scale = cbrtf(static_cast<float>(MAX_PROBES) / static_cast<float>(totalProbes));
        m_gridX = static_cast<uint32_t>(ceilf(m_gridX * scale));
        m_gridY = static_cast<uint32_t>(ceilf(m_gridY * scale));
        m_gridZ = static_cast<uint32_t>(ceilf(m_gridZ * scale));
        if (m_gridX < 1) m_gridX = 1;
        if (m_gridY < 1) m_gridY = 1;
        if (m_gridZ < 1) m_gridZ = 1;
        totalProbes = m_gridX * m_gridY * m_gridZ;
    }

    m_probes.reserve(totalProbes);

    // Compute actual spacing for each axis
    float spacingX = (m_gridX > 1) ? extentX / static_cast<float>(m_gridX - 1) : 0.0f;
    float spacingY = (m_gridY > 1) ? extentY / static_cast<float>(m_gridY - 1) : 0.0f;
    float spacingZ = (m_gridZ > 1) ? extentZ / static_cast<float>(m_gridZ - 1) : 0.0f;

    // Probe influence radius: slightly larger than the grid spacing diagonal
    // so probes overlap for smooth interpolation
    float maxSpacing = spacingX;
    if (spacingY > maxSpacing) maxSpacing = spacingY;
    if (spacingZ > maxSpacing) maxSpacing = spacingZ;
    float probeRadius = maxSpacing * 1.5f;
    if (probeRadius < DEFAULT_PROBE_RADIUS * 0.1f) {
        probeRadius = DEFAULT_PROBE_RADIUS;
    }

    // Place probes on the grid
    for (uint32_t iz = 0; iz < m_gridZ; iz++) {
        for (uint32_t iy = 0; iy < m_gridY; iy++) {
            for (uint32_t ix = 0; ix < m_gridX; ix++) {
                GIProbe probe;
                probe.position[0] = sceneBounds.minBounds[0] + ix * spacingX;
                probe.position[1] = sceneBounds.minBounds[1] + iy * spacingY;
                probe.position[2] = sceneBounds.minBounds[2] + iz * spacingZ;
                probe.radius = probeRadius;
                probe.intensity = m_giIntensity;
                probe.valid = false;

                // Initialize SH coefficients and color to zero
                for (int i = 0; i < 12; i++) probe.shCoeffs[i] = 0.0f;
                probe.color[0] = probe.color[1] = probe.color[2] = 0.0f;

                m_probes.push_back(probe);
            }
        }
    }

    RTX_DIAG("GISystem::PlaceProbes() placed %zu probes (grid %ux%ux%u, spacing %.1fx%.1fx%.1f, radius=%.1f)",
             m_probes.size(), m_gridX, m_gridY, m_gridZ,
             (m_gridX > 1) ? extentX / (float)(m_gridX - 1) : 0.0f,
             (m_gridY > 1) ? extentY / (float)(m_gridY - 1) : 0.0f,
             (m_gridZ > 1) ? extentZ / (float)(m_gridZ - 1) : 0.0f,
             probeRadius);
    return static_cast<uint32_t>(m_probes.size());
}

// ============================================================================
// SH Helpers
// ============================================================================

void GISystem::EvaluateSHBasis(const float dir[3], float basis[4]) {
    // Order 1 SH basis functions:
    // Y00  = 0.282095                   (constant / DC term)
    // Y1-1 = 0.488603 * y              (linear Y)
    // Y10  = 0.488603 * z              (linear Z)
    // Y11  = 0.488603 * x              (linear X)
    basis[0] = SH_Y00;
    basis[1] = SH_Y1x * dir[1];  // Y1-1
    basis[2] = SH_Y1x * dir[2];  // Y10
    basis[3] = SH_Y1x * dir[0];  // Y11
}

void GISystem::AccumulateSHFromDirection(float shCoeffs[12], const float lightDir[3],
                                          const float lightColor[3], float intensity) {
    float basis[4];
    EvaluateSHBasis(lightDir, basis);

    // For a directional light, the SH projection is:
    // c_lm += lightColor * intensity * Y_lm(direction)
    // The cosine lobe transfer (for diffuse) adds an additional factor,
    // but for simplicity we apply a flat projection here.
    // The full cosine-weighted SH projection uses:
    //   A0 = PI        (for L=0)
    //   A1 = 2*PI/3    (for L=1)
    // We fold these into the accumulation.
    float a0 = PI;
    float a1 = 2.0f * PI / 3.0f;

    for (int c = 0; c < 3; c++) {
        float col = lightColor[c] * intensity;
        shCoeffs[0 * 3 + c] += col * basis[0] * a0;   // L00
        shCoeffs[1 * 3 + c] += col * basis[1] * a1;   // L1-1
        shCoeffs[2 * 3 + c] += col * basis[2] * a1;   // L10
        shCoeffs[3 * 3 + c] += col * basis[3] * a1;   // L11
    }
}

// ============================================================================
// Probe Updates
// ============================================================================

void GISystem::UpdateProbes(const LightData& lightData, int bounceCount, uint32_t currentFrame) {
    if (m_probes.empty()) {
        return;
    }

    m_bounceCount = bounceCount;
    m_globalFrameCounter = currentFrame;

    // Bounce intensity factor: each bounce reduces contribution
    // First bounce = full intensity, subsequent bounces halve
    float bounceScale = 1.0f;
    for (int b = 1; b < bounceCount; b++) {
        bounceScale += 1.0f / static_cast<float>(1 << b);
    }
    // Normalize so total contribution is in a reasonable range
    bounceScale = bounceScale / static_cast<float>(bounceCount > 0 ? bounceCount : 1);

    // Incremental update: only process a batch of probes per frame.
    // This distributes the CPU cost across multiple frames instead of
    // updating all probes at once. The round-robin index wraps around.
    uint32_t totalProbes = static_cast<uint32_t>(m_probes.size());
    uint32_t batchSize = m_probesPerFrame;
    if (batchSize > totalProbes) batchSize = totalProbes;

    // Ensure the start index is within range
    if (m_nextProbeToUpdate >= totalProbes) {
        m_nextProbeToUpdate = 0;
    }

    uint32_t startIdx = m_nextProbeToUpdate;
    uint32_t endIdx = startIdx + batchSize;

    for (uint32_t idx = startIdx; idx < endIdx; idx++) {
        // Wrap around for the last batch
        uint32_t probeIdx = idx % totalProbes;
        GIProbe& probe = m_probes[probeIdx];

        // Reset SH coefficients and color for fresh accumulation
        for (int i = 0; i < 12; i++) probe.shCoeffs[i] = 0.0f;
        probe.color[0] = probe.color[1] = probe.color[2] = 0.0f;

        // Accumulate ambient light (constant term in SH)
        // Ambient contributes to L00 band only
        float ambientSH = SH_Y00 * PI;
        probe.shCoeffs[0] += lightData.ambientColor[0] * ambientSH;
        probe.shCoeffs[1] += lightData.ambientColor[1] * ambientSH;
        probe.shCoeffs[2] += lightData.ambientColor[2] * ambientSH;

        // Also store as flat color (ambient)
        probe.color[0] += lightData.ambientColor[0];
        probe.color[1] += lightData.ambientColor[1];
        probe.color[2] += lightData.ambientColor[2];

        // Accumulate directional lights
        for (const auto& dirLight : lightData.directionalLights) {
            AccumulateSHFromDirection(probe.shCoeffs, dirLight.direction,
                                      dirLight.color, dirLight.intensity * bounceScale);

            // Simple flat color accumulation: NdotL approximation using
            // a fixed "up" hemisphere assumption for probes. This is a gross
            // simplification but works for N64's simple lighting model.
            // For each directional light, add color scaled by intensity.
            float contribution = dirLight.intensity * bounceScale * 0.5f;
            probe.color[0] += dirLight.color[0] * contribution;
            probe.color[1] += dirLight.color[1] * contribution;
            probe.color[2] += dirLight.color[2] * contribution;
        }

        // Accumulate point lights (attenuation based on distance to probe)
        for (const auto& ptLight : lightData.pointLights) {
            float dx = ptLight.position[0] - probe.position[0];
            float dy = ptLight.position[1] - probe.position[1];
            float dz = ptLight.position[2] - probe.position[2];
            float distSq = dx * dx + dy * dy + dz * dz;
            float lightRadiusSq = ptLight.radius * ptLight.radius;

            // Only accumulate if within light radius
            if (distSq < lightRadiusSq && lightRadiusSq > 0.0f) {
                float dist = sqrtf(distSq);
                // Quadratic attenuation: 1 - (dist/radius)^2
                float attenuation = 1.0f - (distSq / lightRadiusSq);
                attenuation *= attenuation; // Smooth falloff
                float intensity = ptLight.intensity * attenuation * bounceScale;

                // Direction from probe to light (for SH accumulation)
                float invDist = (dist > 0.001f) ? (1.0f / dist) : 0.0f;
                float lightDir[3] = { dx * invDist, dy * invDist, dz * invDist };

                AccumulateSHFromDirection(probe.shCoeffs, lightDir,
                                          ptLight.color, intensity);

                // Flat color accumulation
                probe.color[0] += ptLight.color[0] * intensity * 0.5f;
                probe.color[1] += ptLight.color[1] * intensity * 0.5f;
                probe.color[2] += ptLight.color[2] * intensity * 0.5f;
            }
        }

        // Apply GI intensity multiplier
        for (int i = 0; i < 12; i++) probe.shCoeffs[i] *= m_giIntensity;
        probe.color[0] *= m_giIntensity;
        probe.color[1] *= m_giIntensity;
        probe.color[2] *= m_giIntensity;

        probe.intensity = m_giIntensity;
        probe.valid = true;
        probe.lastUpdateFrame = currentFrame;
    }

    // Advance the round-robin index for next frame
    m_nextProbeToUpdate = (startIdx + batchSize) % totalProbes;
}

// ============================================================================
// Nearest Probe Lookup
// ============================================================================

std::vector<ProbeResult> GISystem::GetNearestProbes(const float position[3], uint32_t count) const {
    if (m_probes.empty() || count == 0) {
        return {};
    }

    // Compute distances from position to all probes
    struct ProbeDistance {
        uint32_t index;
        float distSq;
    };

    std::vector<ProbeDistance> distances;
    distances.reserve(m_probes.size());

    for (uint32_t i = 0; i < m_probes.size(); i++) {
        const auto& probe = m_probes[i];
        if (!probe.valid) continue;

        float dx = position[0] - probe.position[0];
        float dy = position[1] - probe.position[1];
        float dz = position[2] - probe.position[2];
        float distSq = dx * dx + dy * dy + dz * dz;

        // Only consider probes within their influence radius
        if (distSq <= probe.radius * probe.radius) {
            distances.push_back({ i, distSq });
        }
    }

    // Sort by distance (ascending)
    std::sort(distances.begin(), distances.end(),
        [](const ProbeDistance& a, const ProbeDistance& b) {
            return a.distSq < b.distSq;
        });

    // Take the nearest 'count' probes
    uint32_t resultCount = static_cast<uint32_t>(distances.size());
    if (resultCount > count) resultCount = count;

    std::vector<ProbeResult> results;
    results.reserve(resultCount);

    // Compute inverse-distance weights
    float totalWeight = 0.0f;
    for (uint32_t i = 0; i < resultCount; i++) {
        float dist = sqrtf(distances[i].distSq);
        // Inverse distance weighting with a small epsilon to avoid division by zero
        float weight = 1.0f / (dist + 0.001f);
        results.push_back({ &m_probes[distances[i].index], dist, weight });
        totalWeight += weight;
    }

    // Normalize weights
    if (totalWeight > 0.0f) {
        for (auto& r : results) {
            r.weight /= totalWeight;
        }
    }

    return results;
}

// ============================================================================
// Indirect Light Evaluation
// ============================================================================

void GISystem::EvaluateIndirectLight(const float position[3], float outColor[3]) const {
    outColor[0] = outColor[1] = outColor[2] = 0.0f;

    auto nearestProbes = GetNearestProbes(position, 4);
    if (nearestProbes.empty()) {
        return;
    }

    for (const auto& result : nearestProbes) {
        if (!result.probe || !result.probe->valid) continue;

        // Blend using the flat color (simpler than full SH evaluation for CPU-side use)
        outColor[0] += result.probe->color[0] * result.weight;
        outColor[1] += result.probe->color[1] * result.weight;
        outColor[2] += result.probe->color[2] * result.weight;
    }
}

// ============================================================================
// Irradiance Evaluation via SH
// ============================================================================

void GISystem::GetIrradianceAtPoint(const float position[3], const float normal[3], float outIrradiance[3]) const {
    outIrradiance[0] = outIrradiance[1] = outIrradiance[2] = 0.0f;

    auto nearestProbes = GetNearestProbes(position, 4);
    if (nearestProbes.empty()) {
        return;
    }

    // Evaluate the SH basis in the given normal direction.
    // This lets us compute directionally-dependent irradiance:
    // irradiance(n) = sum_lm( shCoeff_lm * Y_lm(n) )
    float basis[4];
    EvaluateSHBasis(normal, basis);

    for (const auto& result : nearestProbes) {
        if (!result.probe || !result.probe->valid) continue;

        const float* sh = result.probe->shCoeffs;

        // Evaluate irradiance for each color channel by dotting
        // the SH coefficients with the basis functions in the normal direction.
        // shCoeffs layout: [L00.r, L00.g, L00.b, L1-1.r, L1-1.g, L1-1.b,
        //                   L10.r, L10.g, L10.b, L11.r, L11.g, L11.b]
        for (int c = 0; c < 3; c++) {
            float irr = sh[0 * 3 + c] * basis[0]   // L00
                      + sh[1 * 3 + c] * basis[1]   // L1-1
                      + sh[2 * 3 + c] * basis[2]   // L10
                      + sh[3 * 3 + c] * basis[3];  // L11

            // Weight by interpolation factor and ensure non-negative
            if (irr < 0.0f) irr = 0.0f;
            outIrradiance[c] += irr * result.weight;
        }
    }
}

// ============================================================================
// Per-Frame Update
// ============================================================================

void GISystem::Update(const float viewMatrix[16], const SceneConfig& config) {
    if (!m_initialized) {
        static bool s_loggedUninit = false;
        if (!s_loggedUninit) {
            RTX_DIAG("GISystem::Update() skipped - not initialized");
            s_loggedUninit = true;
        }
        return;
    }
    if (!viewMatrix) {
        static bool s_loggedNullView = false;
        if (!s_loggedNullView) {
            RTX_DIAG("GISystem::Update() skipped - null viewMatrix pointer");
            OutputDebugStringA("[RTX] WARNING: null viewMatrix in GISystem::Update\n");
            s_loggedNullView = true;
        }
        return;
    }

    // Re-apply scene config each frame for dynamic adjustments
    // (e.g., time-of-day modified configs)
    ApplySceneConfig(config);

    // Update temporal accumulation with camera motion detection
    UpdateAccumulation(viewMatrix);
}

// ============================================================================
// Temporal Accumulation
// ============================================================================

uint32_t GISystem::UpdateAccumulation(const float currentViewMatrix[16]) {
    if (!m_initialized || !currentViewMatrix) {
        static bool s_loggedSkip = false;
        if (!s_loggedSkip) {
            RTX_DIAG("GISystem::UpdateAccumulation() skipped (initialized=%s, viewMatrix=%p)",
                     m_initialized ? "yes" : "no", (const void*)currentViewMatrix);
            s_loggedSkip = true;
        }
        return 0;
    }

    // Detect camera movement by comparing view matrices.
    // Use a generous epsilon to avoid false positives from floating-point
    // interpolation noise in the N64 camera system. The original 1e-5 threshold
    // was far too sensitive — sub-pixel camera jitter from the game's fixed-point
    // math would trigger a "camera moved" every frame, resetting accumulation
    // to frameCount=0 and preventing any temporal noise reduction.
    // With 1e-3, only actual intentional camera movement resets accumulation.
    // Additionally, we use a cumulative difference metric instead of per-element
    // to further reduce false positives from tiny per-element drift.
    m_cameraMoved = false;
    float totalDiff = 0.0f;
    for (int i = 0; i < 16; i++) {
        totalDiff += fabsf(currentViewMatrix[i] - m_prevViewMatrix[i]);
    }
    // Use cumulative threshold: sum of all 16 element diffs must exceed 0.1
    // to count as real camera movement. This is much more robust than the
    // per-element 1e-5 check which triggered on floating-point noise.
    // The threshold is generous (0.1 cumulative across 16 floats) to ensure
    // only intentional camera movement resets accumulation. N64 fixed-point
    // math can produce per-frame jitter of ~0.001 per element, which totals
    // ~0.016 across 16 elements — well below 0.1.
    if (totalDiff > 0.1f) {
        m_cameraMoved = true;
    }

    if (m_cameraMoved) {
        if (m_accumulationFrameCount > 0) {
            // Log only when accumulation resets (not every frame while moving)
            static uint32_t s_resetCount = 0;
            s_resetCount++;
            if (s_resetCount <= 10 || (s_resetCount % 300) == 0) {
                RTX_DIAG("GISystem: camera moved (totalDiff=%.4f) - resetting accumulation (was at %u frames, reset #%u)",
                         totalDiff, m_accumulationFrameCount, s_resetCount);
            }
        }
        // Don't hard-reset to 0 — keep a minimum of 1 so the Accumulate shader
        // still blends with some history. This prevents a full-noise flash on
        // every camera movement. The Accumulate shader uses alpha=max(0.05, 1/frameCount),
        // so frameCount=1 gives alpha=1.0 (full current frame), and frameCount=2 gives
        // alpha=0.5, which ramps up quickly enough for responsiveness.
        m_accumulationFrameCount = 0;
    } else {
        if (m_accumulationFrameCount < m_maxAccumulationFrames) {
            m_accumulationFrameCount++;
        }
    }

    memcpy(m_prevViewMatrix, currentViewMatrix, sizeof(float) * 16);

    return m_accumulationFrameCount;
}

void GISystem::ResetAccumulation() {
    RTX_DIAG("GISystem::ResetAccumulation() called (was at frameCount=%u)", m_accumulationFrameCount);
    m_accumulationFrameCount = 0;
    memset(m_prevViewMatrix, 0, sizeof(m_prevViewMatrix));
    // Signal the renderer to GPU-clear the accumulation/output UAV buffers.
    // This prevents stale data from the previous scene contaminating the new scene's
    // first frames via temporal accumulation blending.
    m_needsGPUBufferClear = true;
}

// ============================================================================
// Scene Config Application
// ============================================================================

void GISystem::ApplySceneConfig(const SceneConfig& config) {
    static uint32_t s_applyCount = 0;
    s_applyCount++;
    // Apply GI intensity from the scene config (clamped to valid range).
    m_giIntensity = config.giIntensity;
    if (m_giIntensity < 0.0f || std::isnan(m_giIntensity) || std::isinf(m_giIntensity)) m_giIntensity = 1.0f;
    if (m_giIntensity > 5.0f) m_giIntensity = 5.0f;

    // Apply probe density from scene config
    m_probeDensity = config.probeDensity;
    if (m_probeDensity < 1.0f || std::isnan(m_probeDensity)) m_probeDensity = DEFAULT_PROBE_DENSITY;

    // Apply bounce count (clamped to valid range)
    m_bounceCount = config.maxBounces;
    if (m_bounceCount < 0) m_bounceCount = 0;
    if (m_bounceCount > 8) m_bounceCount = 8;

    // Ambient minimum intensity (indirect bounce light floor).
    // Used by shader for minimum shadow fill and by CPU probe computations.
    m_ambientMinIntensity = config.ambientMinIntensity;

    // Scale denoise parameters based on GI intensity.
    // Higher GI intensity: the GI bounce produces more noisy output, so we
    // increase the base color sigma to allow more filtering of GI noise.
    // Lower GI intensity: less GI noise, so tighter sigma preserves more detail.
    // The v4 shader applies per-pixel variance-adaptive scaling + separate
    // luminance/chrominance paths on top of these base values.
    float giScale = 0.75f + 0.25f * config.giIntensity;
    m_colorSigma = DEFAULT_COLOR_SIGMA * giScale;
    m_normalSigma = DEFAULT_NORMAL_SIGMA; // Power-cosine exponent (128.0), passed directly to shader
    m_depthSigma = DEFAULT_DEPTH_SIGMA;

    // Temporal accumulation blend alpha: the v4 Accumulate.hlsl uses
    // variance clipping + tonemapped-space blending + motion rejection.
    // A lower base alpha gives better temporal stability for static scenes;
    // the shader automatically increases alpha for noisy/moving areas.
    m_blendAlpha = DEFAULT_BLEND_ALPHA;

    // Scale max accumulation frames: more bounces benefit from more accumulation
    // to converge, but cap at a reasonable maximum for responsiveness.
    if (config.maxBounces >= 3) {
        m_maxAccumulationFrames = 256;
    } else if (config.maxBounces >= 2) {
        m_maxAccumulationFrames = DEFAULT_MAX_ACCUMULATION_FRAMES;
    } else {
        m_maxAccumulationFrames = 64;
    }

    // Apply reflection quality setting from scene config
    if (config.reflectionQuality < 0.5f) {
        // Low reflection quality: reduce accumulation frames for faster convergence
        // at the cost of slightly noisier reflections
        m_maxAccumulationFrames = m_maxAccumulationFrames / 2;
    }

    // Ensure max accumulation frames doesn't go below 1
    if (m_maxAccumulationFrames < 1) m_maxAccumulationFrames = 1;

    // Log first few applications and periodically for debugging
    if (s_applyCount <= 3 || (s_applyCount % 600) == 0) {
        RTX_DIAG("GISystem::ApplySceneConfig() #%u: giIntensity=%.2f, bounces=%d, maxAccum=%u, colorSigma=%.4f, blendAlpha=%.3f, probeDensity=%.1f",
                 s_applyCount, m_giIntensity, m_bounceCount, m_maxAccumulationFrames,
                 m_colorSigma, m_blendAlpha, m_probeDensity);
    }
}

// ============================================================================
// Denoise Constants
// ============================================================================

DenoiseConstants GISystem::GetDenoiseConstants(int pass) const {
    DenoiseConstants constants = {};

    // Clamp pass to valid range to prevent out-of-range step sizes
    if (pass < 0) pass = 0;
    if (pass >= NUM_DENOISE_PASSES) pass = NUM_DENOISE_PASSES - 1;

    // A-trous wavelet filter uses exponentially increasing step sizes.
    // Pass 0: stepSize=1  (5x5 effective kernel = 5 pixel radius)
    // Pass 1: stepSize=2  (spacing=2, effective 10 pixel radius)
    // Pass 2: stepSize=4  (spacing=4, effective 20 pixel radius)
    // Pass 3: stepSize=8  (spacing=8, effective 40 pixel radius)
    // Reduced from 5 passes (step 16) to 4 passes to prevent excessive blur.
    // The step=16 pass covered 80 pixel radius which is too broad for N64
    // content and caused visible smearing of texture detail.
    switch (pass) {
        case 0: constants.stepSize = 1;  break;
        case 1: constants.stepSize = 2;  break;
        case 2: constants.stepSize = 4;  break;
        case 3: constants.stepSize = 8;  break;
        default: constants.stepSize = 1; break;
    }

    // Per-pass sigma scaling strategy for the v8 denoiser:
    //
    // The shader (v8) uses SIGMA_L_REF=2.0 and has per-pass
    // detail scaling internally (0.5x for pass 0, 0.7x for pass 1, etc.).
    // CPU-side scaling provides additional per-pass relaxation.
    //
    // - Pass 0 (step=1): Base sigma. Shader further scales to 0.5x.
    // - Pass 1 (step=2): Relaxed (1.2x). Shader scales to 0.7x.
    // - Pass 2 (step=4): More relaxed (1.4x). Shader scales to 0.85x.
    // - Pass 3 (step=8): Broadest smoothing (1.6x). Shader scales to 1.0x.
    //
    // Combined effective colorSigma at maximum (noisy region, varianceScale=1.0):
    //   Pass 0: 0.15 * 1.00 * 2.0 * 0.50 = 0.150
    //   Pass 1: 0.15 * 1.20 * 2.0 * 0.70 = 0.252
    //   Pass 2: 0.15 * 1.40 * 2.0 * 0.85 = 0.357
    //   Pass 3: 0.15 * 1.60 * 2.0 * 1.00 = 0.480
    // These are significantly stronger than v7 (which reached 0.122 at pass 3)
    // but the variance-adaptive 8% floor protects clean textured areas.
    float passScale = 1.0f + (float)pass * 0.20f;  // Linear ramp: 1.0, 1.2, 1.4, 1.6

    // Color sigma: controls luminance edge-stopping base threshold.
    // The shader scales this per-pixel based on local variance (smoothstep ramp
    // with 8% floor). The SIGMA_L_REF=2.0 multiplier in the shader amplifies
    // this base value. Pass 0 uses the base value; later passes scale up.
    if (pass == 0) {
        constants.colorSigma = m_colorSigma;
    } else {
        constants.colorSigma = m_colorSigma * passScale;
    }

    // Normal sigma: power-cosine exponent for w_normal = max(0,dot(n1,n2))^exponent.
    // Higher values = sharper normal edge preservation. Keep constant across all
    // passes — geometric edges should be preserved equally regardless of filter scale.
    // N64 polygon normals have hard breaks at every triangle boundary, so high
    // exponent (128) is critical for preserving polygon boundaries.
    constants.normalSigma = m_normalSigma;

    // Depth sigma: scale gently with pass number. Larger step sizes sample
    // further away on surfaces, so a slight increase is needed.  But the shader
    // v7 no longer multiplies by stepSize internally, so CPU controls this fully.
    // Keep the scaling conservative — the shader's 5% hard cutoff catches
    // silhouette edges regardless of this soft falloff value.
    if (pass == 0) {
        constants.depthSigma = m_depthSigma;
    } else {
        constants.depthSigma = m_depthSigma * (1.0f + (float)pass * 0.25f);
    }

    // Sanitize: ensure no NaN/Inf values reach the GPU
    if (std::isnan(constants.colorSigma) || std::isinf(constants.colorSigma) || constants.colorSigma <= 0.0f) {
        RTX_DIAG("GISystem::GetDenoiseConstants() pass %d: colorSigma=%.6f is invalid, using default %.4f",
                 pass, constants.colorSigma, DEFAULT_COLOR_SIGMA);
        constants.colorSigma = DEFAULT_COLOR_SIGMA;
    }
    if (std::isnan(constants.normalSigma) || std::isinf(constants.normalSigma) || constants.normalSigma <= 0.0f) {
        RTX_DIAG("GISystem::GetDenoiseConstants() pass %d: normalSigma=%.6f is invalid, using default %.1f",
                 pass, constants.normalSigma, DEFAULT_NORMAL_SIGMA);
        constants.normalSigma = DEFAULT_NORMAL_SIGMA;
    }
    if (std::isnan(constants.depthSigma) || std::isinf(constants.depthSigma) || constants.depthSigma < 0.0f) {
        RTX_DIAG("GISystem::GetDenoiseConstants() pass %d: depthSigma=%.6f is invalid, using default %.4f",
                 pass, constants.depthSigma, DEFAULT_DEPTH_SIGMA);
        constants.depthSigma = DEFAULT_DEPTH_SIGMA;
    }

    return constants;
}

AccumulateConstants GISystem::GetAccumulateConstants(uint32_t width, uint32_t height) const {
    AccumulateConstants constants = {};
    constants.resolutionX = width;
    constants.resolutionY = height;
    constants.frameCount = m_accumulationFrameCount;
    constants.blendAlpha = m_blendAlpha;

    // Sanitize: ensure blendAlpha is valid (must be in (0, 1])
    if (std::isnan(constants.blendAlpha) || std::isinf(constants.blendAlpha) ||
        constants.blendAlpha <= 0.0f || constants.blendAlpha > 1.0f) {
        RTX_DIAG("GISystem::GetAccumulateConstants() blendAlpha=%.6f is invalid, using default %.3f",
                 constants.blendAlpha, DEFAULT_BLEND_ALPHA);
        constants.blendAlpha = DEFAULT_BLEND_ALPHA;
    }

    return constants;
}

// ============================================================================
// Denoiser Pipeline Validation
// ============================================================================

bool GISystem::ValidateDenoiserPipeline(uint32_t width, uint32_t height) const {
    bool valid = true;

    // Validate resolution is non-zero (dispatch would be a no-op otherwise)
    if (width == 0 || height == 0) {
        RTX_DIAG("GISystem::ValidateDenoiserPipeline() FAIL: zero resolution %ux%u", width, height);
        OutputDebugStringA("[RTX] Denoiser validation FAIL: zero resolution\n");
        valid = false;
    }

    // Validate thread group counts won't be zero or excessively large
    if (width > 0 && height > 0) {
        uint32_t groupsX = (width + 7) / 8;
        uint32_t groupsY = (height + 7) / 8;
        // D3D12 limit: 65535 thread groups per dimension
        if (groupsX > 65535 || groupsY > 65535) {
            RTX_DIAG("GISystem::ValidateDenoiserPipeline() FAIL: thread groups exceed D3D12 limit (%ux%u groups for %ux%u resolution)",
                     groupsX, groupsY, width, height);
            OutputDebugStringA("[RTX] Denoiser validation FAIL: thread group count exceeds D3D12 limit\n");
            valid = false;
        }
    }

    // Validate denoise constants for all passes
    for (int pass = 0; pass < NUM_DENOISE_PASSES; pass++) {
        DenoiseConstants dc = GetDenoiseConstants(pass);

        // stepSize must be a power of 2 (1, 2, 4, 8)
        if (dc.stepSize <= 0 || (dc.stepSize & (dc.stepSize - 1)) != 0) {
            RTX_DIAG("GISystem::ValidateDenoiserPipeline() FAIL: pass %d stepSize=%d (not power of 2)", pass, dc.stepSize);
            valid = false;
        }

        // Color sigma must be positive (zero would cause division by zero in shader)
        if (dc.colorSigma <= 0.0f) {
            RTX_DIAG("GISystem::ValidateDenoiserPipeline() FAIL: pass %d colorSigma=%.6f (must be > 0)", pass, dc.colorSigma);
            valid = false;
        }

        // Normal sigma must be positive (power-cosine exponent, typically 128)
        if (dc.normalSigma <= 0.0f) {
            RTX_DIAG("GISystem::ValidateDenoiserPipeline() FAIL: pass %d normalSigma=%.6f (must be > 0)", pass, dc.normalSigma);
            valid = false;
        }

        // Depth sigma must be non-negative (zero disables depth edge-stopping,
        // which is valid but unusual)
        if (dc.depthSigma < 0.0f) {
            RTX_DIAG("GISystem::ValidateDenoiserPipeline() FAIL: pass %d depthSigma=%.6f (must be >= 0)", pass, dc.depthSigma);
            valid = false;
        }

        // Sanity: check for NaN/Inf in sigma values
        if (std::isnan(dc.colorSigma) || std::isinf(dc.colorSigma) ||
            std::isnan(dc.normalSigma) || std::isinf(dc.normalSigma) ||
            std::isnan(dc.depthSigma) || std::isinf(dc.depthSigma)) {
            RTX_DIAG("GISystem::ValidateDenoiserPipeline() FAIL: pass %d has NaN/Inf sigma (color=%.6f, normal=%.6f, depth=%.6f)",
                     pass, dc.colorSigma, dc.normalSigma, dc.depthSigma);
            OutputDebugStringA("[RTX] Denoiser validation FAIL: NaN/Inf in denoise sigma\n");
            valid = false;
        }
    }

    // Validate accumulate constants
    {
        AccumulateConstants ac = GetAccumulateConstants(width, height);

        // Resolution must match
        if (ac.resolutionX != width || ac.resolutionY != height) {
            RTX_DIAG("GISystem::ValidateDenoiserPipeline() FAIL: accumulate resolution mismatch (expected %ux%u, got %ux%u)",
                     width, height, ac.resolutionX, ac.resolutionY);
            valid = false;
        }

        // Blend alpha must be in (0, 1] range
        if (ac.blendAlpha <= 0.0f || ac.blendAlpha > 1.0f) {
            RTX_DIAG("GISystem::ValidateDenoiserPipeline() FAIL: blendAlpha=%.6f (must be in (0, 1])", ac.blendAlpha);
            valid = false;
        }

        // Sanity: check for NaN/Inf in blend alpha
        if (std::isnan(ac.blendAlpha) || std::isinf(ac.blendAlpha)) {
            RTX_DIAG("GISystem::ValidateDenoiserPipeline() FAIL: blendAlpha is NaN/Inf");
            OutputDebugStringA("[RTX] Denoiser validation FAIL: NaN/Inf in blendAlpha\n");
            valid = false;
        }
    }

    // Validate that max accumulation frames is reasonable
    if (m_maxAccumulationFrames == 0 || m_maxAccumulationFrames > 4096) {
        RTX_DIAG("GISystem::ValidateDenoiserPipeline() FAIL: maxAccumulationFrames=%u (must be in [1, 4096])", m_maxAccumulationFrames);
        valid = false;
    }

    // Validate GI intensity is reasonable
    if (std::isnan(m_giIntensity) || std::isinf(m_giIntensity) || m_giIntensity < 0.0f) {
        RTX_DIAG("GISystem::ValidateDenoiserPipeline() FAIL: giIntensity=%.6f (invalid)", m_giIntensity);
        valid = false;
    }

    if (valid) {
        RTX_DIAG("GISystem::ValidateDenoiserPipeline() PASS (%ux%u, maxAccum=%u, giIntensity=%.2f)",
                 width, height, m_maxAccumulationFrames, m_giIntensity);
    }

    return valid;
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
