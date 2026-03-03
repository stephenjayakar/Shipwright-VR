#pragma once
#ifndef GI_SYSTEM_H
#define GI_SYSTEM_H

#ifdef ENABLE_DX12_RTX

#include "DX12Device.h"
#include "RTXTypes.h"
#include <cstdint>
#include <vector>

// Forward declare SceneConfig to avoid circular include
namespace RTX {
    struct SceneConfig;
}

namespace RTX {

// GI probe: a point in the scene that accumulates indirect illumination.
// Probes are placed on a grid within the scene AABB and each accumulates
// a simplified spherical harmonics (SH) or flat color representation of
// incoming indirect light from directional and point light sources.
struct GIProbe {
    float position[3];      // World-space position of the probe
    float radius;           // Influence radius (world units)

    // Spherical harmonics coefficients (order 1, 4 bands: L00, L1-1, L10, L11)
    // Each band has RGB channels, so 4 * 3 = 12 floats total.
    // SH order-1 is sufficient for low-frequency indirect light in N64 scenes.
    float shCoeffs[12];     // [L00.r, L00.g, L00.b, L1-1.r, L1-1.g, L1-1.b,
                            //  L10.r, L10.g, L10.b, L11.r, L11.g, L11.b]

    // Flat accumulated color (simplified fallback when SH is not needed)
    float color[3];         // Accumulated indirect RGB color [0, ...]
    float intensity;        // Overall intensity multiplier

    // Metadata
    bool     valid;            // True if this probe has been updated at least once
    uint32_t lastUpdateFrame;  // Frame number when this probe was last updated

    GIProbe() : radius(100.0f), intensity(1.0f), valid(false), lastUpdateFrame(0) {
        position[0] = position[1] = position[2] = 0.0f;
        color[0] = color[1] = color[2] = 0.0f;
        for (int i = 0; i < 12; i++) shCoeffs[i] = 0.0f;
    }
};

// Axis-aligned bounding box for scene bounds
struct AABB {
    float minBounds[3];
    float maxBounds[3];

    AABB() {
        minBounds[0] = minBounds[1] = minBounds[2] = 0.0f;
        maxBounds[0] = maxBounds[1] = maxBounds[2] = 0.0f;
    }

    AABB(float minX, float minY, float minZ, float maxX, float maxY, float maxZ) {
        minBounds[0] = minX; minBounds[1] = minY; minBounds[2] = minZ;
        maxBounds[0] = maxX; maxBounds[1] = maxY; maxBounds[2] = maxZ;
    }
};

// Directional light data for probe updates
struct DirectionalLightData {
    float direction[3];     // Normalized light direction (pointing toward light source)
    float color[3];         // Light color RGB [0, 1]
    float intensity;        // Light intensity multiplier
};

// Point light data for probe updates
struct PointLightData {
    float position[3];      // World-space position
    float color[3];         // Light color RGB [0, 1]
    float intensity;        // Light intensity multiplier
    float radius;           // Attenuation radius (world units)
};

// Combined light data passed to UpdateProbes
struct LightData {
    std::vector<DirectionalLightData> directionalLights;
    std::vector<PointLightData> pointLights;
    float ambientColor[3];  // Scene ambient light

    LightData() {
        ambientColor[0] = ambientColor[1] = ambientColor[2] = 0.0f;
    }
};

// Result from GetNearestProbes: probe pointer + distance for weighted interpolation
struct ProbeResult {
    const GIProbe* probe;
    float distance;         // Distance from query position to probe
    float weight;           // Interpolation weight (inverse distance, normalized)
};

// GISystem manages CPU-side global illumination through light probes and
// temporal accumulation/denoise parameter management.
//
// The probe system provides a sparse grid of GI probes within the scene AABB.
// Each probe accumulates simplified SH or color data from directional and point
// lights. At query time, the nearest probes to a given position are returned
// with interpolation weights for smooth indirect lighting.
//
// The actual GPU GI work is done by:
//   - RayGen.hlsl: temporal accumulation blending (uses frameCount from SceneConstants)
//   - ClosestHit.hlsl: 1-bounce diffuse GI ray
//   - Denoise.hlsl: edge-aware A-trous wavelet spatial denoiser (compute shader)
//   - DXRPipeline: dispatches denoise compute passes
//
// GISystem is owned by RTXRenderer and called each frame.

class GISystem {
public:
    GISystem();
    ~GISystem();

    // Initialize the GI system. Called once during RTXRenderer::Initialize.
    bool Initialize(DX12Device* device);

    // Shut down and release resources.
    void Shutdown();

    // === Probe Management ===

    // Place probes on a grid within the given scene AABB.
    // probeDensity: approximate spacing between probes (in world units).
    //               Lower values = more probes = higher quality but more memory.
    //               Default is derived from SceneConfig::probeDensity.
    // Returns the number of probes placed.
    uint32_t PlaceProbes(const AABB& sceneBounds, float probeDensity = 200.0f);

    // Update probes incrementally with the given light data.
    // Each frame, only a batch of probes is updated (rotating through the full set)
    // to avoid processing all probes every frame. The batch size is controlled by
    // m_probesPerFrame (default: MAX_PROBES_PER_FRAME).
    // bounceCount: number of conceptual bounces to simulate (scales intensity).
    // currentFrame: the current frame number (used for lastUpdateFrame tracking).
    void UpdateProbes(const LightData& lightData, int bounceCount = 1, uint32_t currentFrame = 0);

    // Get the nearest probes to a given world-space position.
    // Returns up to 'count' probes sorted by distance, with interpolation weights.
    std::vector<ProbeResult> GetNearestProbes(const float position[3], uint32_t count = 4) const;

    // Get all probes (read-only). Useful for debug visualization.
    const std::vector<GIProbe>& GetProbes() const { return m_probes; }

    // Alias for GetProbes() — returns the probe buffer for GPU upload or inspection.
    const std::vector<GIProbe>& GetProbeBuffer() const { return m_probes; }

    // Get the number of active probes.
    uint32_t GetProbeCount() const { return static_cast<uint32_t>(m_probes.size()); }

    // Evaluate interpolated indirect light at a world-space position.
    // Uses GetNearestProbes and blends their flat color contributions.
    // Result is stored in outColor[3].
    void EvaluateIndirectLight(const float position[3], float outColor[3]) const;

    // Evaluate interpolated irradiance at a world-space position using SH evaluation.
    // Uses GetNearestProbes and interpolates SH coefficients, then evaluates
    // them in the given normal direction to produce directionally-dependent irradiance.
    // normal: the surface normal direction at the query point (normalized).
    // outIrradiance: output RGB irradiance [0, ...].
    void GetIrradianceAtPoint(const float position[3], const float normal[3], float outIrradiance[3]) const;

    // === Temporal Accumulation ===

    // Called each frame to update GI system state.
    // viewMatrix: current camera view matrix (row-major, 16 floats).
    // sceneConfig: current per-scene config (for dynamic adjustments).
    // Internally calls ApplySceneConfig() and UpdateAccumulation().
    void Update(const float viewMatrix[16], const SceneConfig& sceneConfig);

    // Called each frame to update temporal accumulation state.
    // Returns the current accumulation frame count for SceneConstants.
    // Automatically detects camera movement and resets when needed.
    uint32_t UpdateAccumulation(const float currentViewMatrix[16]);

    // Reset accumulation (e.g., on scene load or forced reset).
    // Also sets a flag so the renderer knows to GPU-clear the accumulation UAV.
    void ResetAccumulation();

    // Check and clear the "needs GPU buffer clear" flag.
    // Returns true if the accumulation UAV buffers should be cleared to zero
    // on the GPU side (via ClearUnorderedAccessViewFloat). After returning true,
    // the flag is cleared so subsequent calls return false until the next reset.
    bool NeedsGPUBufferClear() {
        bool needed = m_needsGPUBufferClear;
        m_needsGPUBufferClear = false;
        return needed;
    }

    // Apply per-scene GI configuration from RTXSceneConfig.
    // This adjusts denoise sigma values, max accumulation frames, probe density,
    // and GI intensity based on the scene's settings.
    void ApplySceneConfig(const SceneConfig& config);

    // Get denoise parameters for a given pass (0-3 for 4 A-trous passes).
    DenoiseConstants GetDenoiseConstants(int pass) const;

    // Get temporal accumulation parameters.
    AccumulateConstants GetAccumulateConstants(uint32_t width, uint32_t height) const;

    // Get blend alpha for EMA temporal accumulation.
    float GetBlendAlpha() const { return m_blendAlpha; }

    // Validate that the denoiser pipeline parameters are sane.
    // Returns true if all denoise/accumulate constants are within valid ranges.
    // Logs diagnostic warnings for any issues detected.
    // Called from RTXRenderer::DispatchAndPresent() on first frame and periodically
    // to catch configuration drift from bad scene configs.
    bool ValidateDenoiserPipeline(uint32_t width, uint32_t height) const;

    // === Accessors ===

    uint32_t GetAccumulationFrameCount() const { return m_accumulationFrameCount; }
    uint32_t GetMaxAccumulationFrames() const { return m_maxAccumulationFrames; }
    bool CameraMovedThisFrame() const { return m_cameraMoved; }
    float GetGIIntensity() const { return m_giIntensity; }
    float GetAmbientMinIntensity() const { return m_ambientMinIntensity; }
    float GetProbeDensity() const { return m_probeDensity; }
    int GetBounceCount() const { return m_bounceCount; }
    const AABB& GetSceneBounds() const { return m_sceneBounds; }

    // Maximum frames to accumulate before capping.
    // Higher values = cleaner image but slower convergence when moving.
    static constexpr uint32_t DEFAULT_MAX_ACCUMULATION_FRAMES = 24;

    // Denoise parameters — tuned for v8 A-trous wavelet filter with:
    //   - Separate luminance/chrominance filtering via YCoCg (shader applies 2.5x relaxation on chroma)
    //   - Per-pixel variance estimation with smoothstep ramp (shader scales sigma adaptively, 8% floor)
    //   - Sobel-based depth gradient for robust slope handling (8% hard cutoff)
    //   - Power-cosine normal edge-stopping: w_normal = max(0,dot(n1,n2))^exponent
    //   - Robust firefly suppression using mean+k*sigma clamping (extended to pass 2)
    //   - SIGMA_L_REF=2.0 in shader for stronger baseline noise reduction
    // These base values are passed to the shader per-pass with per-pass scaling (see GetDenoiseConstants).
    static constexpr float DEFAULT_COLOR_SIGMA = 0.03f;    // Tighter denoise to avoid smeared terrain detail.
    static constexpr float DEFAULT_NORMAL_SIGMA = 128.0f;  // Normal edge-stopping: power-cosine exponent (higher = sharper edges, 128 for N64 hard polygon breaks)
    static constexpr float DEFAULT_DEPTH_SIGMA = 0.030f;   // Depth edge-stopping: lower = tighter (gradient-aware in shader, hard cutoff at 5%)
    static constexpr int NUM_DENOISE_PASSES = 1;           // Single pass to avoid smear while keeping basic cleanup.

    // Temporal accumulation parameters — v8 Accumulate.hlsl uses:
    //   - YCoCg-space variance clipping (mean±1.25σ) for tight history rejection
    //   - Tonemapped-space blending for perceptually uniform interpolation
    //   - Motion-aware rejection via clamped-history distance measurement
    //   - Gentler fast convergence ramp (first 20 frames, less aggressive than v7)
    //   - Variance-adaptive: noisy areas get 30% alpha (strong history retention)
    //   - Lower convergence floor (0.03) for smoother steady-state
    // This base alpha is the starting point; shader adapts per-pixel.
    static constexpr float DEFAULT_BLEND_ALPHA = 0.45f;    // Strong current-frame weight for less ghosting/flashing.

    // Probe defaults
    static constexpr float DEFAULT_PROBE_DENSITY = 200.0f;    // World units between probes
    static constexpr float DEFAULT_PROBE_RADIUS = 300.0f;     // Influence radius per probe
    static constexpr uint32_t MAX_PROBES = 4096;              // Maximum number of probes
    static constexpr uint32_t MAX_PROBES_PER_FRAME = 64;      // Max probes to update per frame (incremental)

private:
    // Stored for potential future use (e.g., GPU-side probe buffers).
    DX12Device* m_device = nullptr;

    // === Probe Grid ===
    std::vector<GIProbe> m_probes;
    AABB m_sceneBounds;
    float m_probeDensity = DEFAULT_PROBE_DENSITY;
    int m_bounceCount = 1;
    uint32_t m_gridX = 0, m_gridY = 0, m_gridZ = 0; // Grid dimensions

    // === Incremental probe update ===
    uint32_t m_nextProbeToUpdate = 0;                  // Round-robin index for incremental updates
    uint32_t m_probesPerFrame = MAX_PROBES_PER_FRAME;  // How many probes to update each frame
    uint32_t m_globalFrameCounter = 0;                 // Global frame counter for lastUpdateFrame

    // === Temporal accumulation ===
    uint32_t m_accumulationFrameCount = 0;
    uint32_t m_maxAccumulationFrames = DEFAULT_MAX_ACCUMULATION_FRAMES;
    float m_prevViewMatrix[16] = {};
    bool m_cameraMoved = false;
    bool m_initialized = false;
    bool m_needsGPUBufferClear = false;  // Set by ResetAccumulation(), cleared by NeedsGPUBufferClear()

    // GI parameters (can be overridden by RTXSceneConfig)
    float m_giIntensity = 1.0f;

    // Ambient minimum intensity: floor for indirect bounce light.
    // Applied in the shader's ambient term to prevent fully black shadows.
    // Stored here for CPU-side probe computations that want consistent behavior.
    float m_ambientMinIntensity = 0.10f;

    // Denoise tuning
    float m_colorSigma = DEFAULT_COLOR_SIGMA;
    float m_normalSigma = DEFAULT_NORMAL_SIGMA;
    float m_depthSigma = DEFAULT_DEPTH_SIGMA;
    float m_blendAlpha = DEFAULT_BLEND_ALPHA;

    // === SH Helpers ===
    // Evaluate SH basis functions for a given direction
    static void EvaluateSHBasis(const float dir[3], float basis[4]);

    // Add a directional light contribution to a probe's SH coefficients
    static void AccumulateSHFromDirection(float shCoeffs[12], const float lightDir[3],
                                          const float lightColor[3], float intensity);
};

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // GI_SYSTEM_H
