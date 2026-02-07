#ifdef ENABLE_DX12_RTX

#include "GISystem.h"
#include "RTXSceneConfig.h"
#include <cstring>
#include <cmath>
#include <algorithm>

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
    , m_colorSigma(DEFAULT_COLOR_SIGMA)
    , m_normalSigma(DEFAULT_NORMAL_SIGMA)
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
    if (!device) {
        return false;
    }

    m_device = device;
    m_accumulationFrameCount = 0;
    m_maxAccumulationFrames = DEFAULT_MAX_ACCUMULATION_FRAMES;
    m_cameraMoved = false;
    m_giIntensity = 1.0f;
    m_colorSigma = DEFAULT_COLOR_SIGMA;
    m_normalSigma = DEFAULT_NORMAL_SIGMA;
    m_probeDensity = DEFAULT_PROBE_DENSITY;
    m_bounceCount = 1;
    m_probes.clear();
    m_gridX = m_gridY = m_gridZ = 0;
    m_nextProbeToUpdate = 0;
    m_probesPerFrame = MAX_PROBES_PER_FRAME;
    m_globalFrameCounter = 0;
    memset(m_prevViewMatrix, 0, sizeof(m_prevViewMatrix));
    m_initialized = true;

    return true;
}

void GISystem::Shutdown() {
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
        return 0;
    }

    // Detect camera movement by comparing view matrices.
    // Use a small epsilon to handle floating-point noise from interpolation.
    m_cameraMoved = false;
    for (int i = 0; i < 16; i++) {
        if (fabsf(currentViewMatrix[i] - m_prevViewMatrix[i]) > 1e-5f) {
            m_cameraMoved = true;
            break;
        }
    }

    if (m_cameraMoved) {
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
    m_accumulationFrameCount = 0;
    memset(m_prevViewMatrix, 0, sizeof(m_prevViewMatrix));
}

// ============================================================================
// Scene Config Application
// ============================================================================

void GISystem::ApplySceneConfig(const SceneConfig& config) {
    // Apply GI intensity from the scene config.
    m_giIntensity = config.giIntensity;

    // Apply probe density from scene config
    m_probeDensity = config.probeDensity;

    // Apply bounce count
    m_bounceCount = config.maxBounces;

    // Scale denoise parameters based on GI intensity.
    // Higher GI intensity: slightly increase color sigma to allow more color
    // variation through the denoiser (preserving GI bounce color).
    // Lower GI intensity: tighter sigma for more aggressive denoising.
    m_colorSigma = DEFAULT_COLOR_SIGMA * (0.5f + 0.5f * config.giIntensity);
    m_normalSigma = DEFAULT_NORMAL_SIGMA;

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
}

// ============================================================================
// Denoise Constants
// ============================================================================

DenoiseConstants GISystem::GetDenoiseConstants(int pass) const {
    DenoiseConstants constants = {};

    // A-trous wavelet filter uses exponentially increasing step sizes.
    // Pass 0: stepSize=1 (3x3 effective kernel)
    // Pass 1: stepSize=2 (5x5 effective kernel with gaps)
    // Pass 2: stepSize=4 (9x9 effective kernel with gaps)
    // Together they cover a large spatial footprint efficiently.
    switch (pass) {
        case 0: constants.stepSize = 1; break;
        case 1: constants.stepSize = 2; break;
        case 2: constants.stepSize = 4; break;
        default: constants.stepSize = 1; break;
    }

    constants.colorSigma = m_colorSigma;
    constants.normalSigma = m_normalSigma;
    constants._pad = 0.0f;

    return constants;
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
