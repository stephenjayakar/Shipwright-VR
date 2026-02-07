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

#else

// No-op stubs when RTX is not enabled
static inline int RTX_IsActive(void) { return 0; }
static inline int RTX_IsKokiriForest(void* play) { (void)play; return 0; }
static inline void RTX_UpdateSceneParams(void* play) { (void)play; }
static inline void RTX_DispatchAndPresent(void) {}
static inline void RTX_OnRoomLoaded(void* play, int roomNum) { (void)play; (void)roomNum; }
static inline void RTX_OnSceneUnload(void) {}
static inline int RTX_Initialize(void* hwnd, unsigned int width, unsigned int height) { (void)hwnd; (void)width; (void)height; return 0; }
static inline void RTX_Shutdown(void) {}

#endif // ENABLE_DX12_RTX

#ifdef __cplusplus
}
#endif

#endif // RTX_HOOKS_H
