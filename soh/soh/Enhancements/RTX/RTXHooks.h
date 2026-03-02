#pragma once
#ifndef RTX_HOOKS_H
#define RTX_HOOKS_H

// C-callable hooks for integration with the game's C codebase.
// These are called from z_scene_table.c, z_play.c, OTRGlobals.cpp, z_scene_otr.cpp.

#ifdef __cplusplus
extern "C" {
#endif

#ifdef ENABLE_DX12_RTX

// Check if RTX renderer is active and rendering
int RTX_IsActive(void);

// Check if the current scene is Kokiri Forest
int RTX_IsKokiriForest(void* play);

// Update scene rendering parameters from the scene draw config.
// Called from func_8009E0B8() (SDC_KOKIRI_FOREST) in z_scene_table.c.
// Extracts camera, lighting, fog, and scene-specific parameters from PlayState.
void RTX_UpdateSceneParams(void* play);

// Dispatch rays and present the frame.
// Called from Graph_ProcessGfxCommands() in OTRGlobals.cpp.
void RTX_DispatchAndPresent(void);

// Called when a room finishes loading.
// Triggers geometry extraction and BLAS building.
void RTX_OnRoomLoaded(void* play, int roomNum);

// Called when a new scene is loaded (before room loading begins).
// Initializes RTX state for the scene if it's an RTX-enabled scene.
void RTX_OnSceneLoaded(int sceneNum);

// Called when leaving a scene.
// Releases acceleration structures, textures, and GPU buffers.
void RTX_OnSceneUnload(void);

// Initialize the RTX renderer.
// Called once during startup if RTX hardware is detected.
// hwnd: game window handle
// width/height: initial render resolution
int RTX_Initialize(void* hwnd, unsigned int width, unsigned int height);

// Shut down the RTX renderer.
void RTX_Shutdown(void);

// Early initialization: creates DX12 device + command queue and registers
// bridge callbacks BEFORE gfx_dxgi creates its swap chain.
// Must be called during startup, before the graphics backend initializes.
void RTX_EarlyInit(void);

// Check if the DX12 bridge is active (swap chain owned by DX12)
int RTX_IsBridgeActive(void);

// Get the shared swap chain (from bridge)
void* RTX_GetSharedSwapChain(void);

// Acquire the game window HWND for DX12 use, tearing down the DX11 swap chain.
// Returns the HWND as void* on success, NULL on failure.
// Must be called before RTX_Initialize(). Defined in OTRGlobals.cpp.
void* RTX_AcquireWindowForDX12(void);

// Get the game window HWND WITHOUT releasing the DX11 swap chain.
// Used for the probe phase of two-phase initialization.
// Defined in OTRGlobals.cpp.
void* RTX_GetWindowHWND(void);

// Get the game window dimensions from the DXGI backend.
// Defined in OTRGlobals.cpp.
void RTX_GetWindowDimensions(unsigned int* width, unsigned int* height);

// Check if the DX11 swap chain has been released (for DX12 takeover).
// When true, the DX11 rendering path must not try to present.
// Defined in OTRGlobals.cpp.
int RTX_IsDX11SwapChainReleased(void);

// Probe DX12/DXR support without touching DX11.
// Returns 1 if DX12 + DXR raytracing is available, 0 otherwise.
// Called before RTX_AcquireWindowForDX12 to avoid tearing down DX11 unnecessarily.
int RTX_ProbeSupport(void);

// Get the game HWND for DX12 use.
// The HWND is obtained from the DXGI backend's stored window handle.
// Returns the HWND as void* on success, NULL on failure.
// Unlike RTX_AcquireWindowForDX12, this does NOT release the DX11 swap chain.
// Worker 2 (DX12Device) should use this to get the HWND for swap chain creation.
void* RTX_GetGameHWND(void);

// Block or unblock DX11 rendering and Present calls.
// When blocked, the DX11 backend skips all Draw/Clear/Present operations.
// This is the primary mechanism to prevent DX11 from fighting with DX12.
void RTX_BlockDX11(int block);

// Intercept a decoded N64 texture and upload it to the RTX texture manager.
// Called from the texture decode path when RTX is active.
// timgAddr: original N64 timg pointer/address (used for hash computation)
// rgbaData: decoded RGBA8 pixel data (width * height * 4 bytes)
// width/height: texture dimensions in texels
// format: packed N64 format ((G_IM_FMT << 4) | G_IM_SIZ)
void RTX_InterceptTexture(const void* timgAddr, const unsigned char* rgbaData,
                          unsigned int width, unsigned int height, unsigned int format);

// Upload UI overlay pixel data for compositing on top of the RTX scene.
// Called from Graph_ProcessGfxCommands after capturing the game's HUD output.
// pixelData: RGBA8 pixel data (width * height * 4 bytes), or NULL for no UI.
// width/height: UI frame dimensions (should match the render resolution).
void RTX_UploadUIOverlay(const unsigned char* pixelData, unsigned int width, unsigned int height);

// Diagnostic: Get the state of the DX12 rendering pipeline as a string.
// Writes up to bufSize chars into buf. Returns the number of chars written.
// This bypasses all shaders and reports raw pipeline state.
int RTX_GetDiagnosticState(char* buf, int bufSize);

// Diagnostic: Write a solid color rectangle to the DX12 back buffer.
// This is a C++ level test that bypasses ALL shaders.
// If the output changes, it proves the DX12 swap chain is presenting.
// If the output does NOT change, the DX12 swap chain is not displaying.
// r,g,b,a are 0.0-1.0 floating point color values.
// Returns 1 if the clear was executed, 0 if it was skipped.
int RTX_DiagnosticClearBackBuffer(float r, float g, float b, float a);

#else

// No-op stubs when RTX is not enabled
static inline int RTX_IsActive(void) { return 0; }
static inline int RTX_IsKokiriForest(void* play) { (void)play; return 0; }
static inline void RTX_UpdateSceneParams(void* play) { (void)play; }
static inline void RTX_DispatchAndPresent(void) {}
static inline void RTX_OnRoomLoaded(void* play, int roomNum) { (void)play; (void)roomNum; }
static inline void RTX_OnSceneLoaded(int sceneNum) { (void)sceneNum; }
static inline void RTX_OnSceneUnload(void) {}
static inline int RTX_Initialize(void* hwnd, unsigned int width, unsigned int height) { (void)hwnd; (void)width; (void)height; return 0; }
static inline void RTX_Shutdown(void) {}
static inline void RTX_EarlyInit(void) {}
static inline int RTX_IsBridgeActive(void) { return 0; }
static inline void* RTX_GetSharedSwapChain(void) { return (void*)0; }
static inline void* RTX_AcquireWindowForDX12(void) { return (void*)0; }
static inline void* RTX_GetWindowHWND(void) { return (void*)0; }
static inline void RTX_GetWindowDimensions(unsigned int* width, unsigned int* height) { (void)width; (void)height; }
static inline int RTX_IsDX11SwapChainReleased(void) { return 0; }
static inline int RTX_ProbeSupport(void) { return 0; }
static inline void* RTX_GetGameHWND(void) { return (void*)0; }
static inline void RTX_BlockDX11(int block) { (void)block; }
static inline void RTX_InterceptTexture(const void* timgAddr, const unsigned char* rgbaData,
                                        unsigned int width, unsigned int height, unsigned int format) {
    (void)timgAddr; (void)rgbaData; (void)width; (void)height; (void)format;
}
static inline void RTX_UploadUIOverlay(const unsigned char* pixelData, unsigned int width, unsigned int height) {
    (void)pixelData; (void)width; (void)height;
}
static inline int RTX_GetDiagnosticState(char* buf, int bufSize) { (void)buf; (void)bufSize; return 0; }
static inline int RTX_DiagnosticClearBackBuffer(float r, float g, float b, float a) { (void)r; (void)g; (void)b; (void)a; return 0; }

#endif // ENABLE_DX12_RTX

#ifdef __cplusplus
}
#endif

#endif // RTX_HOOKS_H
