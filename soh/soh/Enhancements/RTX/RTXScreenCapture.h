#pragma once
#ifndef RTX_SCREEN_CAPTURE_H
#define RTX_SCREEN_CAPTURE_H

#ifdef ENABLE_DX12_RTX

#include <d3d12.h>
#include <dxgi1_6.h>
#include <string>
#include <cstdint>

// ============================================================================
// Free-function API for screenshot capture.
// These are the primary entry points for integrating screen capture into the
// rendering pipeline.
// ============================================================================

/// Initialize the screenshot capture system. Call once after DX12 device and
/// swap chain creation.
/// @param device   The DX12 device.
/// @param queue    The direct command queue used for rendering.
void RTX_InitScreenCapture(ID3D12Device* device, ID3D12CommandQueue* queue);

/// Capture a screenshot and save it to disk as a BMP file.
/// @param filename  Output filename (relative to screenshots/ directory).
///                  If nullptr or empty, a frame-stamped name is generated.
void RTX_CaptureScreenshot(const char* filename);

/// Called each frame after Present(). Handles:
///   - Auto-capture at early startup frames (3, 5, 10, 30, 60, 120, 300) for fresh output
///   - Periodic capture every N frames (if configured via RTX_SetAutoCaptureFrames)
///   - F12 key detection with debounce
/// Filenames use timestamps: rtx_YYYYMMDD_HHMMSS_frameNNN.bmp
/// On first capture, writes screenshots/rtx_capture_marker.txt as a canary.
/// Output directory is resolved to an ABSOLUTE path based on the exe location.
/// If D3D12 readback fails, a 100x100 solid blue fallback BMP is written as proof of execution.
/// Null backBuffer, null queue, or zero dimensions are handled gracefully (logged and skipped).
/// @param backBuffer  The current back buffer resource (after Present, in PRESENT state).
/// @param queue       The command queue to execute the copy on.
/// @param format      The DXGI format of the back buffer.
/// @param width       Back buffer width in pixels.
/// @param height      Back buffer height in pixels.
void RTX_CaptureAfterPresent(ID3D12Resource* backBuffer, ID3D12CommandQueue* queue,
                             DXGI_FORMAT format, UINT width, UINT height);

/// Set auto-capture interval: capture a screenshot every N frames.
/// Set to 0 or negative to disable periodic capture.
/// @param frameInterval  Number of frames between captures.
void RTX_SetAutoCaptureFrames(int frameInterval);

/// Called each frame to check if auto-capture should fire.
/// Checks startup auto-capture frames (3,5,10,30,60), periodic interval,
/// and F12 key press with debounce. If any trigger fires, performs
/// the capture using the stored device/queue from RTX_InitScreenCapture.
/// Requires the swap chain to be set via RTXScreenCapture::Initialize.
/// All public methods have null guards and will not crash on invalid input.
void RTX_CheckAutoCapture();

/// Returns true if a capture should happen this frame (due to auto-capture
/// countdown, frame interval, or F12 key press).
bool RTX_ShouldCapture();

/// Set the output directory for screenshots. Defaults to "screenshots/".
/// @param dir  Directory path (will be created if it doesn't exist).
void RTX_SetOutputDir(const char* dir);


// ============================================================================
// Class-based API — static utility class wrapping the free-function API.
// Provides Initialize/Update/CaptureScreenshot/OnPresent/CaptureOnTimer/
// CaptureOnKeyPress as static methods.
// ============================================================================

namespace RTX {

class RTXScreenCapture {
public:
    /// Initialize the capture system with the DX12 device and command queue.
    /// The swap chain can optionally be provided; if nullptr, OnPresent() must
    /// supply it each frame.
    static void Initialize(ID3D12Device* device, ID3D12CommandQueue* queue);

    /// Overload accepting an explicit swap chain reference (stored for Update/CaptureScreenshot).
    static void Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain3* swapChain);

    /// Capture a specific back buffer resource and save it as a BMP file.
    /// This is the primary capture method when you have a direct reference to the
    /// back buffer resource (e.g. after Present).
    /// @param backBuffer  The back buffer resource to capture (in PRESENT state).
    /// @param filename    Optional output filename. If nullptr, a timestamped name is generated.
    static void CaptureFrame(ID3D12Resource* backBuffer, const char* filename = nullptr);

    /// Capture the current back buffer and save it as a BMP file.
    /// @param filename  Name of the output file (relative to screenshots/ directory).
    ///                  If empty, a frame-stamped name is generated.
    static void CaptureScreenshot(const std::string& filename = "");

    /// Configure periodic auto-capture: capture a screenshot every @p frameInterval frames.
    /// Set to 0 or negative to disable periodic capture.
    static void SetAutoCapture(int frameInterval);

    /// Alias for SetAutoCapture for backwards compatibility.
    static void CaptureOnTimer(int frameInterval);

    /// Set the output directory for screenshots. Defaults to "screenshots/".
    /// @param dir  Directory path (will be created if it doesn't exist).
    static void SetOutputDir(const char* dir);

    /// Check if F12 is pressed and capture a screenshot if so (with debounce).
    /// This is called internally by OnPresent/Update, but can also be called
    /// manually from other locations.
    static void CaptureOnKeyPress();

    /// Check for F12 key press to trigger manual capture (alias for CaptureOnKeyPress).
    /// Intended to be called each frame from the render loop.
    static void CheckKeyPress();

    /// Called each frame after swapChain->Present(). Obtains the current back
    /// buffer from the swap chain and delegates to RTX_CaptureAfterPresent() for
    /// auto-capture at early frames (3,5,10,30,60), periodic capture, and F12 key.
    /// Null swapChain is handled gracefully (logged and skipped).
    /// @param swapChain  The swap chain that was just presented.
    static void OnPresent(IDXGISwapChain* swapChain);

    /// Capture from swap chain after Present(). Obtains the back buffer from the
    /// swap chain, checks auto-capture triggers and F12 key, and captures if needed.
    /// @param swapChain  The swap chain that was just presented.
    /// @param queue      The command queue for executing the readback copy.
    static void CaptureAfterPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue);

    /// Called each frame (after Present). Legacy update method — requires the swap
    /// chain to have been set via Initialize(device, queue, swapChain).
    static void Update();

    /// Returns true if the capture system has been initialized.
    static bool IsInitialized();

private:
    static ID3D12Device*       s_device;
    static ID3D12CommandQueue*  s_commandQueue;
    static IDXGISwapChain3*    s_swapChain;
    static bool                s_initialized;
};

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // RTX_SCREEN_CAPTURE_H
