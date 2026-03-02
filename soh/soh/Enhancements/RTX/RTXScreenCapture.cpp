#ifdef ENABLE_DX12_RTX

#include "RTXScreenCapture.h"
#include "RTXDiagLog.h"

#include <wrl/client.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <direct.h>    // _mkdir
#include <sys/stat.h>  // _stat
#endif

using Microsoft::WRL::ComPtr;

// ============================================================================
// Helper: OutputDebugStringA-based logging for every capture attempt
// ============================================================================
static void CaptureDebugLog(const char* fmt, ...) {
#ifdef _WIN32
    char buf[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
#endif
}

// ============================================================================
// Internal state
// ============================================================================
static ID3D12Device*       g_captureDevice       = nullptr;
static ID3D12CommandQueue*  g_captureQueue        = nullptr;
static bool                g_captureInitialized   = false;
static uint32_t            g_frameCounter         = 0;
static int                 g_autoCaptureInterval  = 0;   // 0 = disabled; >0 = capture every N frames
static bool                g_f12WasDown           = false;
static bool                g_pendingCapture       = false; // set by RTX_ShouldCapture checks
static std::string         g_outputDir;                    // resolved to ABSOLUTE path on init
static bool                g_outputDirResolved    = false; // true once we've resolved the absolute path
static std::string         g_exeDir;                       // directory containing the running exe

// Auto-capture at specific startup frames — VERY early captures to guarantee fresh output.
// Frame 3 and 5 prove execution early (even if geometry isn't fully loaded).
// Frame 10 captures first real rendered content.
// Frame 30 catches Kokiri Forest loaded.
// Frame 60 gives a converged frame.
static const uint32_t AUTO_CAPTURE_FRAMES[] = { 3, 5, 10, 30, 60, 120, 300 };
static const int      AUTO_CAPTURE_FRAME_COUNT = sizeof(AUTO_CAPTURE_FRAMES) / sizeof(AUTO_CAPTURE_FRAMES[0]);

// Marker file written on first successful capture — canary to verify captures happened
static bool g_markerFileWritten = false;

// Proof file at known absolute path
static const char* FALLBACK_PROOF_PATH = "C:\\Users\\aj12a\\programming\\Shipwright-3\\rtx_capture_proof.txt";
static bool g_proofFileWritten = false;

// Counter for manual (F12) captures, used for filename generation
static uint32_t        g_manualCaptureCount   = 0;

// ============================================================================
// Forward declarations for internal helpers
// ============================================================================
static void ResolveOutputDir();
static void EnsureScreenshotDir();
static std::string GenerateTimestampedFrameFilename();
static std::string GenerateTimestampedManualFilename();
static std::string GenerateTimestampFilename();
static bool WriteBMP(const std::string& filepath, uint32_t width, uint32_t height,
                     const uint8_t* rgbaData, uint32_t rowPitch, DXGI_FORMAT format);
static void CaptureBackBuffer(ID3D12Resource* backBuffer, ID3D12Device* device,
                              ID3D12CommandQueue* queue, DXGI_FORMAT format,
                              UINT width, UINT height, const char* filename);
static void WriteProofFile();
static void WriteMarkerFile();
static void WriteFallbackBlueBMP(const char* filename);
static void WriteFallbackBlueBMPForFrame();

// ============================================================================
// Free-function API implementation
// ============================================================================

void RTX_InitScreenCapture(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (!device) {
        CaptureDebugLog("[RTX_ScreenCapture] RTX_InitScreenCapture() SKIPPED — null device");
        RTX_DIAG("RTX_InitScreenCapture() SKIPPED — null device");
        return;
    }
    if (!queue) {
        CaptureDebugLog("[RTX_ScreenCapture] RTX_InitScreenCapture() SKIPPED — null queue");
        RTX_DIAG("RTX_InitScreenCapture() SKIPPED — null queue");
        return;
    }

    g_captureDevice = device;
    g_captureQueue = queue;
    g_captureInitialized = true;
    g_frameCounter = 0;
    g_autoCaptureInterval = 0;
    g_f12WasDown = false;
    g_pendingCapture = false;
    g_markerFileWritten = false;
    g_proofFileWritten = false;

    // Resolve the output directory to an absolute path based on the exe location.
    // This MUST happen during init so all subsequent captures write to a known location.
    ResolveOutputDir();

    CaptureDebugLog("[RTX_ScreenCapture] RTX_InitScreenCapture() SUCCESS — device=%p, queue=%p, outputDir='%s'",
                    (void*)device, (void*)queue, g_outputDir.c_str());
    RTX_DIAG("RTX_InitScreenCapture() SUCCESS — device=%p, queue=%p, outputDir='%s'",
             (void*)device, (void*)queue, g_outputDir.c_str());
}

void RTX_CaptureScreenshot(const char* filename) {
    if (!g_captureInitialized || !g_captureDevice || !g_captureQueue) {
        CaptureDebugLog("[RTX_ScreenCapture] RTX_CaptureScreenshot() SKIPPED — not initialized (device=%p, queue=%p, init=%d)",
                        (void*)g_captureDevice, (void*)g_captureQueue, (int)g_captureInitialized);
        RTX_DIAG("RTX_CaptureScreenshot() SKIPPED — not initialized");
        return;
    }
    // This is for programmatic capture without explicit back buffer params.
    // It requires the class-based API (with swap chain) to obtain the back buffer.
    // For the free-function path, callers should use RTX_CaptureAfterPresent().
    CaptureDebugLog("[RTX_ScreenCapture] RTX_CaptureScreenshot('%s') — use RTX_CaptureAfterPresent() for explicit capture",
                    filename ? filename : "(null)");
    RTX_DIAG("RTX_CaptureScreenshot('%s') — use RTX_CaptureAfterPresent() for explicit capture",
             filename ? filename : "(null)");
}

void RTX_CaptureAfterPresent(ID3D12Resource* backBuffer, ID3D12CommandQueue* queue,
                             DXGI_FORMAT format, UINT width, UINT height) {
    if (!g_captureInitialized || !g_captureDevice) {
        CaptureDebugLog("[RTX_ScreenCapture] RTX_CaptureAfterPresent() SKIPPED — not initialized");
        return;
    }

    g_frameCounter++;

    // Write proof file on first frame — proves this code path is executing
    if (!g_proofFileWritten) {
        WriteProofFile();
    }

    // Log frame counter for first 20 frames to aid debugging
    if (g_frameCounter <= 20 || (g_frameCounter % 100) == 0) {
        CaptureDebugLog("[RTX_ScreenCapture] RTX_CaptureAfterPresent() frame %u — bb=%p, q=%p, %ux%u, fmt=%u",
                        g_frameCounter, (void*)backBuffer, (void*)queue, width, height, (unsigned)format);
        RTX_DIAG("RTX_CaptureAfterPresent() frame %u — bb=%p, q=%p, %ux%u, fmt=%u",
                 g_frameCounter, (void*)backBuffer, (void*)queue, width, height, (unsigned)format);
    }

    // Determine if we should capture this frame
    bool shouldCapture = false;
    const char* reason = nullptr;

    // Check startup auto-capture frames (3, 5, 10, 30, 60)
    for (int i = 0; i < AUTO_CAPTURE_FRAME_COUNT; i++) {
        if (g_frameCounter == AUTO_CAPTURE_FRAMES[i]) {
            shouldCapture = true;
            reason = "startup-auto";
            break;
        }
    }

    // Check periodic auto-capture interval
    if (!shouldCapture && g_autoCaptureInterval > 0 && (g_frameCounter % g_autoCaptureInterval) == 0) {
        shouldCapture = true;
        reason = "interval-auto";
    }

    // Check F12 key press with debounce
    bool isManualCapture = false;
#ifdef _WIN32
    bool f12Down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (f12Down && !g_f12WasDown) {
        shouldCapture = true;
        isManualCapture = true;
        reason = "F12-key";
    }
    g_f12WasDown = f12Down;
#endif

    if (!shouldCapture) return;

    // Validate parameters — null guards to prevent crashes
    if (!backBuffer) {
        CaptureDebugLog("[RTX_ScreenCapture] CAPTURE SKIPPED frame %u (%s) — null backBuffer resource",
                        g_frameCounter, reason ? reason : "unknown");
        RTX_DIAG("RTX_CaptureAfterPresent() — skipping capture frame %u: null backBuffer", g_frameCounter);
        return;
    }
    if (!queue) {
        CaptureDebugLog("[RTX_ScreenCapture] CAPTURE SKIPPED frame %u (%s) — null command queue",
                        g_frameCounter, reason ? reason : "unknown");
        RTX_DIAG("RTX_CaptureAfterPresent() — skipping capture frame %u: null queue", g_frameCounter);
        return;
    }
    if (width == 0 || height == 0) {
        CaptureDebugLog("[RTX_ScreenCapture] CAPTURE SKIPPED frame %u (%s) — zero dimensions %ux%u",
                        g_frameCounter, reason ? reason : "unknown", width, height);
        RTX_DIAG("RTX_CaptureAfterPresent() — skipping capture frame %u: zero dimensions %ux%u",
                 g_frameCounter, width, height);
        return;
    }

    // Generate timestamped filename: rtx_YYYYMMDD_HHMMSS_frameNNN.bmp
    std::string autoFilename;
    if (isManualCapture) {
        g_manualCaptureCount++;
        autoFilename = GenerateTimestampedManualFilename();
    } else {
        autoFilename = GenerateTimestampedFrameFilename();
    }

    CaptureDebugLog("[RTX_ScreenCapture] CAPTURING frame %u (%s) -> %s (%ux%u)",
                    g_frameCounter, reason ? reason : "unknown", autoFilename.c_str(), width, height);
    RTX_DIAG("RTX_CaptureAfterPresent() — capturing frame %u (%s) -> %s",
             g_frameCounter, reason ? reason : "unknown", autoFilename.c_str());

    CaptureBackBuffer(backBuffer, g_captureDevice, queue, format, width, height, autoFilename.c_str());
}

void RTX_SetAutoCaptureFrames(int frameInterval) {
    g_autoCaptureInterval = frameInterval > 0 ? frameInterval : 0;
    CaptureDebugLog("[RTX_ScreenCapture] RTX_SetAutoCaptureFrames(%d) — interval set to %d",
                    frameInterval, g_autoCaptureInterval);
    RTX_DIAG("RTX_SetAutoCaptureFrames(%d) — interval set to %d", frameInterval, g_autoCaptureInterval);
}

bool RTX_ShouldCapture() {
    if (!g_captureInitialized) return false;

    // Check startup auto-capture frames
    // Note: g_frameCounter is updated inside RTX_CaptureAfterPresent, so this
    // peeking check uses frameCounter+1 (the next frame that will be processed).
    uint32_t nextFrame = g_frameCounter + 1;
    for (int i = 0; i < AUTO_CAPTURE_FRAME_COUNT; i++) {
        if (nextFrame == AUTO_CAPTURE_FRAMES[i]) return true;
    }

    // Check periodic interval
    if (g_autoCaptureInterval > 0 && (nextFrame % g_autoCaptureInterval) == 0) return true;

    // Check F12 key
#ifdef _WIN32
    bool f12Down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    if (f12Down && !g_f12WasDown) return true;
#endif

    return false;
}

void RTX_CheckAutoCapture() {
    if (!g_captureInitialized || !g_captureDevice || !g_captureQueue) {
        return;
    }

    // Delegate to the class-based API which uses the stored swap chain to obtain
    // the back buffer and then calls RTX_CaptureAfterPresent() with auto-capture
    // frame checks, periodic interval, and F12 key detection.
    if (RTX::RTXScreenCapture::IsInitialized()) {
        RTX::RTXScreenCapture::Update();
    }
}

void RTX_SetOutputDir(const char* dir) {
    // Reset the resolved flag so ResolveOutputDir runs again with the new value
    g_outputDirResolved = false;

    if (dir && dir[0] != '\0') {
        g_outputDir = dir;
        // Ensure trailing slash
        if (g_outputDir.back() != '/' && g_outputDir.back() != '\\') {
            g_outputDir += '/';
        }
    } else {
        g_outputDir.clear(); // will be resolved to exe-dir/screenshots/ by ResolveOutputDir
    }

    // Re-resolve to absolute path
    ResolveOutputDir();

    CaptureDebugLog("[RTX_ScreenCapture] RTX_SetOutputDir() — output directory set to '%s'", g_outputDir.c_str());
    RTX_DIAG("RTX_SetOutputDir() — output directory set to '%s'", g_outputDir.c_str());
}

// ============================================================================
// Class-based API implementation
// ============================================================================

namespace RTX {

// Static member definitions
ID3D12Device*       RTXScreenCapture::s_device       = nullptr;
ID3D12CommandQueue*  RTXScreenCapture::s_commandQueue = nullptr;
IDXGISwapChain3*    RTXScreenCapture::s_swapChain    = nullptr;
bool                RTXScreenCapture::s_initialized   = false;

void RTXScreenCapture::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue) {
    if (!device || !queue) {
        CaptureDebugLog("[RTX_ScreenCapture] RTXScreenCapture::Initialize(2-param) SKIPPED — null pointer(s) (device=%p, queue=%p)",
                        (void*)device, (void*)queue);
        RTX_DIAG("RTXScreenCapture::Initialize(2-param) SKIPPED — null pointer(s) (device=%p, queue=%p)",
                 (void*)device, (void*)queue);
        return;
    }

    s_device = device;
    s_commandQueue = queue;
    s_swapChain = nullptr;
    s_initialized = true;

    // Also initialize the free-function API
    RTX_InitScreenCapture(device, queue);

    CaptureDebugLog("[RTX_ScreenCapture] RTXScreenCapture::Initialize(2-param) SUCCESS — device=%p, queue=%p",
                    (void*)device, (void*)queue);
    RTX_DIAG("RTXScreenCapture::Initialize(2-param) SUCCESS — device=%p, queue=%p",
             (void*)device, (void*)queue);
}

void RTXScreenCapture::Initialize(ID3D12Device* device, ID3D12CommandQueue* queue, IDXGISwapChain3* swapChain) {
    if (!device || !queue || !swapChain) {
        CaptureDebugLog("[RTX_ScreenCapture] RTXScreenCapture::Initialize(3-param) SKIPPED — null pointer(s) (device=%p, queue=%p, swapChain=%p)",
                        (void*)device, (void*)queue, (void*)swapChain);
        RTX_DIAG("RTXScreenCapture::Initialize(3-param) SKIPPED — null pointer(s) (device=%p, queue=%p, swapChain=%p)",
                 (void*)device, (void*)queue, (void*)swapChain);
        return;
    }

    s_device = device;
    s_commandQueue = queue;
    s_swapChain = swapChain;
    s_initialized = true;

    // Also initialize the free-function API
    RTX_InitScreenCapture(device, queue);

    CaptureDebugLog("[RTX_ScreenCapture] RTXScreenCapture::Initialize(3-param) SUCCESS — device=%p, queue=%p, swapChain=%p",
                    (void*)device, (void*)queue, (void*)swapChain);
    RTX_DIAG("RTXScreenCapture::Initialize(3-param) SUCCESS — device=%p, queue=%p, swapChain=%p",
             (void*)device, (void*)queue, (void*)swapChain);
}

bool RTXScreenCapture::IsInitialized() {
    return s_initialized;
}

void RTXScreenCapture::CaptureFrame(ID3D12Resource* backBuffer, const char* filename) {
    if (!s_initialized || !s_device || !s_commandQueue) {
        CaptureDebugLog("[RTX_ScreenCapture] RTXScreenCapture::CaptureFrame() SKIPPED — not initialized (device=%p, queue=%p, init=%d)",
                        (void*)s_device, (void*)s_commandQueue, (int)s_initialized);
        RTX_DIAG("RTXScreenCapture::CaptureFrame() SKIPPED — not initialized");
        return;
    }

    if (!backBuffer) {
        CaptureDebugLog("[RTX_ScreenCapture] RTXScreenCapture::CaptureFrame() SKIPPED — null backBuffer");
        RTX_DIAG("RTXScreenCapture::CaptureFrame() SKIPPED — null backBuffer");
        return;
    }

    D3D12_RESOURCE_DESC bbDesc = backBuffer->GetDesc();
    uint32_t width = static_cast<uint32_t>(bbDesc.Width);
    uint32_t height = static_cast<uint32_t>(bbDesc.Height);
    DXGI_FORMAT format = bbDesc.Format;

    if (width == 0 || height == 0) {
        CaptureDebugLog("[RTX_ScreenCapture] RTXScreenCapture::CaptureFrame() SKIPPED — zero dimensions %ux%u", width, height);
        RTX_DIAG("RTXScreenCapture::CaptureFrame() SKIPPED — zero dimensions %ux%u", width, height);
        return;
    }

    std::string fname;
    if (filename && filename[0] != '\0') {
        fname = filename;
    } else {
        fname = GenerateTimestampFilename();
    }

    CaptureDebugLog("[RTX_ScreenCapture] RTXScreenCapture::CaptureFrame() -> %s (%ux%u)", fname.c_str(), width, height);
    CaptureBackBuffer(backBuffer, s_device, s_commandQueue, format, width, height, fname.c_str());
}

void RTXScreenCapture::CaptureScreenshot(const std::string& filename) {
    if (!s_initialized || !s_device || !s_commandQueue || !s_swapChain) {
        CaptureDebugLog("[RTX_ScreenCapture] RTXScreenCapture::CaptureScreenshot() SKIPPED — not initialized (device=%p, queue=%p, swap=%p, init=%d)",
                        (void*)s_device, (void*)s_commandQueue, (void*)s_swapChain, (int)s_initialized);
        RTX_DIAG("RTXScreenCapture::CaptureScreenshot() SKIPPED — not initialized");
        return;
    }

    // Get the presented back buffer from the swap chain.
    // After Present() + MoveToNextFrame(), GetCurrentBackBufferIndex() points to the
    // NEXT frame's buffer. The just-presented buffer is the previous index.
    DXGI_SWAP_CHAIN_DESC scDescCap = {};
    s_swapChain->GetDesc(&scDescCap);
    UINT currentIdxCap = s_swapChain->GetCurrentBackBufferIndex();
    UINT presentedIdxCap = (currentIdxCap > 0) ? (currentIdxCap - 1) : (scDescCap.BufferCount - 1);

    ComPtr<ID3D12Resource> backBuffer;
    HRESULT hr = s_swapChain->GetBuffer(presentedIdxCap, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        // Fallback to current index
        hr = s_swapChain->GetBuffer(currentIdxCap, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr) || !backBuffer) {
            CaptureDebugLog("[RTX_ScreenCapture] RTXScreenCapture::CaptureScreenshot() FAILED — GetBuffer returned 0x%08lX", hr);
            RTX_DIAG("RTXScreenCapture::CaptureScreenshot() FAILED — GetBuffer returned 0x%08lX", hr);
            return;
        }
    }

    D3D12_RESOURCE_DESC bbDesc = backBuffer->GetDesc();
    uint32_t width = static_cast<uint32_t>(bbDesc.Width);
    uint32_t height = static_cast<uint32_t>(bbDesc.Height);
    DXGI_FORMAT format = bbDesc.Format;

    if (width == 0 || height == 0) {
        CaptureDebugLog("[RTX_ScreenCapture] RTXScreenCapture::CaptureScreenshot() SKIPPED — zero dimensions %ux%u", width, height);
        RTX_DIAG("RTXScreenCapture::CaptureScreenshot() SKIPPED — zero dimensions %ux%u", width, height);
        return;
    }

    std::string fname = filename;
    if (fname.empty()) {
        fname = GenerateTimestampFilename();
    }

    CaptureDebugLog("[RTX_ScreenCapture] RTXScreenCapture::CaptureScreenshot() -> %s (%ux%u)", fname.c_str(), width, height);
    CaptureBackBuffer(backBuffer.Get(), s_device, s_commandQueue, format, width, height, fname.c_str());
}

void RTXScreenCapture::SetAutoCapture(int frameInterval) {
    RTX_SetAutoCaptureFrames(frameInterval);
}

void RTXScreenCapture::CaptureOnTimer(int frameInterval) {
    RTX_SetAutoCaptureFrames(frameInterval);
}

void RTXScreenCapture::SetOutputDir(const char* dir) {
    RTX_SetOutputDir(dir);
}

void RTXScreenCapture::CaptureOnKeyPress() {
    if (!s_initialized || !s_device || !s_commandQueue || !s_swapChain) return;

#ifdef _WIN32
    bool f12Down = (GetAsyncKeyState(VK_F12) & 0x8000) != 0;
    static bool s_prevF12 = false;
    if (f12Down && !s_prevF12) {
        CaptureDebugLog("[RTX_ScreenCapture] RTXScreenCapture::CaptureOnKeyPress() — F12 detected, capturing...");
        RTX_DIAG("RTXScreenCapture::CaptureOnKeyPress() — F12 detected, capturing...");
        CaptureScreenshot();
    }
    s_prevF12 = f12Down;
#endif
}

void RTXScreenCapture::CheckKeyPress() {
    CaptureOnKeyPress();
}

void RTXScreenCapture::OnPresent(IDXGISwapChain* swapChain) {
    if (!swapChain) {
        CaptureDebugLog("[RTX_ScreenCapture] RTXScreenCapture::OnPresent() SKIPPED — null swapChain");
        return;
    }
    CaptureAfterPresent(swapChain, s_commandQueue);
}

void RTXScreenCapture::CaptureAfterPresent(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue) {
    if (!s_initialized || !s_device) return;

    if (!swapChain) {
        CaptureDebugLog("[RTX_ScreenCapture] RTXScreenCapture::CaptureAfterPresent() SKIPPED — null swapChain");
        return;
    }

    // Use the provided queue or fall back to stored queue
    ID3D12CommandQueue* activeQueue = queue ? queue : s_commandQueue;
    if (!activeQueue) {
        CaptureDebugLog("[RTX_ScreenCapture] RTXScreenCapture::CaptureAfterPresent() SKIPPED — null queue");
        return;
    }

    // Query IDXGISwapChain3 interface to get the current back buffer index
    ComPtr<IDXGISwapChain3> swapChain3;
    HRESULT hr = swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3));
    if (FAILED(hr) || !swapChain3) {
        // Fallback: try getting buffer 0
        ComPtr<ID3D12Resource> backBuffer;
        hr = swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr) || !backBuffer) return;

        D3D12_RESOURCE_DESC bbDesc = backBuffer->GetDesc();
        RTX_CaptureAfterPresent(backBuffer.Get(), activeQueue,
                                bbDesc.Format, static_cast<UINT>(bbDesc.Width), bbDesc.Height);
        return;
    }

    // Store the swap chain if not already set (for CaptureScreenshot() calls)
    if (!s_swapChain) {
        s_swapChain = swapChain3.Get();
        // Note: we don't AddRef here since the renderer owns the swap chain lifetime
    }

    // After Present() + MoveToNextFrame(), GetCurrentBackBufferIndex() returns
    // the NEXT frame's buffer, not the one we just presented. The presented buffer
    // is the previous index. For a 2-buffer swap chain: presentedIndex = (current + 1) % 2
    DXGI_SWAP_CHAIN_DESC scDesc = {};
    swapChain3->GetDesc(&scDesc);
    UINT currentIndex = swapChain3->GetCurrentBackBufferIndex();
    UINT presentedIndex = (currentIndex > 0) ? (currentIndex - 1) : (scDesc.BufferCount - 1);

    ComPtr<ID3D12Resource> backBuffer;
    hr = swapChain3->GetBuffer(presentedIndex, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        // Fallback to current index if previous index fails
        hr = swapChain3->GetBuffer(currentIndex, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr) || !backBuffer) return;
    }

    D3D12_RESOURCE_DESC bbDesc = backBuffer->GetDesc();
    RTX_CaptureAfterPresent(backBuffer.Get(), activeQueue,
                            bbDesc.Format, static_cast<UINT>(bbDesc.Width), bbDesc.Height);
}

void RTXScreenCapture::Update() {
    if (!s_initialized) return;
    if (!s_swapChain) {
        CaptureDebugLog("[RTX_ScreenCapture] RTXScreenCapture::Update() SKIPPED — null swapChain");
        return;
    }

    // After Present() + MoveToNextFrame(), GetCurrentBackBufferIndex() returns the NEXT
    // frame's buffer. The presented buffer is the previous index.
    DXGI_SWAP_CHAIN_DESC scDesc = {};
    s_swapChain->GetDesc(&scDesc);
    UINT currentIndex = s_swapChain->GetCurrentBackBufferIndex();
    UINT presentedIndex = (currentIndex > 0) ? (currentIndex - 1) : (scDesc.BufferCount - 1);

    ComPtr<ID3D12Resource> backBuffer;
    HRESULT hr = s_swapChain->GetBuffer(presentedIndex, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        // Fallback to current index
        hr = s_swapChain->GetBuffer(currentIndex, IID_PPV_ARGS(&backBuffer));
        if (FAILED(hr) || !backBuffer) return;
    }

    D3D12_RESOURCE_DESC bbDesc = backBuffer->GetDesc();
    UINT width = static_cast<UINT>(bbDesc.Width);
    UINT height = static_cast<UINT>(bbDesc.Height);
    DXGI_FORMAT format = bbDesc.Format;

    RTX_CaptureAfterPresent(backBuffer.Get(), s_commandQueue, format, width, height);
}

} // namespace RTX

// ============================================================================
// Internal helpers
// ============================================================================

/// Resolve g_outputDir to an ABSOLUTE path based on the running exe's directory.
/// This ensures screenshots always go to a known, discoverable location regardless
/// of the process's current working directory. Called once during initialization.
static void ResolveOutputDir() {
    if (g_outputDirResolved) return;
    g_outputDirResolved = true;

#ifdef _WIN32
    // Get the directory containing the running exe
    char exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    if (len > 0) {
        char* lastSlash = strrchr(exePath, '\\');
        if (!lastSlash) lastSlash = strrchr(exePath, '/');
        if (lastSlash) {
            *lastSlash = '\0';
            g_exeDir = exePath;
        }
    }
    if (g_exeDir.empty()) {
        // Fallback to known build output location
        g_exeDir = "C:\\Users\\aj12a\\programming\\Shipwright-3\\x64\\Release";
    }

    // If g_outputDir is empty or relative (no drive letter / UNC prefix), make it absolute
    bool isAbsolute = false;
    if (g_outputDir.size() >= 2 && g_outputDir[1] == ':') isAbsolute = true;      // e.g. C:\...
    if (g_outputDir.size() >= 2 && g_outputDir[0] == '\\' && g_outputDir[1] == '\\') isAbsolute = true; // UNC

    if (g_outputDir.empty()) {
        g_outputDir = g_exeDir + "\\screenshots\\";
    } else if (!isAbsolute) {
        // Prepend exe directory to make it absolute
        // Strip trailing slashes from the relative part temporarily
        std::string relPart = g_outputDir;
        while (!relPart.empty() && (relPart.back() == '/' || relPart.back() == '\\')) {
            relPart.pop_back();
        }
        g_outputDir = g_exeDir + "\\" + relPart + "\\";
    }

    // Ensure trailing backslash
    if (!g_outputDir.empty() && g_outputDir.back() != '\\' && g_outputDir.back() != '/') {
        g_outputDir += '\\';
    }

    CaptureDebugLog("[RTX_ScreenCapture] ResolveOutputDir() — resolved to: '%s' (exeDir='%s')",
                    g_outputDir.c_str(), g_exeDir.c_str());
    RTX_DIAG("ResolveOutputDir() — resolved to: '%s' (exeDir='%s')", g_outputDir.c_str(), g_exeDir.c_str());
#else
    if (g_outputDir.empty()) {
        g_outputDir = "screenshots/";
    }
#endif
}

static void EnsureScreenshotDir() {
    // Resolve absolute path if not done yet (safety net in case Init wasn't called)
    if (!g_outputDirResolved) {
        ResolveOutputDir();
    }

#ifdef _WIN32
    // Create the primary output directory (now an ABSOLUTE path)
    // Strip trailing slash for CreateDirectoryA
    std::string dirPath = g_outputDir;
    while (!dirPath.empty() && (dirPath.back() == '/' || dirPath.back() == '\\')) {
        dirPath.pop_back();
    }
    if (!dirPath.empty()) {
        BOOL ok = CreateDirectoryA(dirPath.c_str(), nullptr);
        DWORD err = GetLastError();
        if (!ok && err != ERROR_ALREADY_EXISTS) {
            // Try to create parent directories if needed
            // Find the parent by looking for last separator
            size_t sep = dirPath.find_last_of("\\/");
            if (sep != std::string::npos) {
                std::string parent = dirPath.substr(0, sep);
                CreateDirectoryA(parent.c_str(), nullptr);
            }
            // Retry creating the directory
            ok = CreateDirectoryA(dirPath.c_str(), nullptr);
            if (!ok && GetLastError() != ERROR_ALREADY_EXISTS) {
                _mkdir(dirPath.c_str());
            }
        }
        CaptureDebugLog("[RTX_ScreenCapture] EnsureScreenshotDir() — primary dir: '%s' (ok=%d, err=%lu)",
                        dirPath.c_str(), (int)ok, (unsigned long)err);
    }

    // Also create the project-root screenshots directory as a backup
    CreateDirectoryA("C:\\Users\\aj12a\\programming\\Shipwright-3\\screenshots", nullptr);

    // If the exe dir is known, ensure screenshots/ exists there too (same as g_outputDir if not overridden)
    if (!g_exeDir.empty()) {
        std::string exeScreenDir = g_exeDir + "\\screenshots";
        CreateDirectoryA(exeScreenDir.c_str(), nullptr);
    }
#endif
}

/// Helper to get current timestamp components
static void GetTimestamp(int& year, int& month, int& day, int& hour, int& minute, int& second) {
    time_t now = time(nullptr);
    struct tm tmBuf = {};
#ifdef _WIN32
    localtime_s(&tmBuf, &now);
#else
    localtime_r(&now, &tmBuf);
#endif
    year = tmBuf.tm_year + 1900;
    month = tmBuf.tm_mon + 1;
    day = tmBuf.tm_mday;
    hour = tmBuf.tm_hour;
    minute = tmBuf.tm_min;
    second = tmBuf.tm_sec;
}

/// Generate a timestamped frame filename: rtx_YYYYMMDD_HHMMSS_frameNNN.bmp
/// Used for auto-capture (startup frames and periodic captures).
static std::string GenerateTimestampedFrameFilename() {
    int y, mo, d, h, mi, s;
    GetTimestamp(y, mo, d, h, mi, s);
    char buf[256];
    snprintf(buf, sizeof(buf), "rtx_%04d%02d%02d_%02d%02d%02d_frame%03u.bmp",
             y, mo, d, h, mi, s, g_frameCounter);
    return std::string(buf);
}

/// Generate a timestamped manual capture filename: rtx_YYYYMMDD_HHMMSS_manual_NNN.bmp
/// Used for F12 key captures.
static std::string GenerateTimestampedManualFilename() {
    int y, mo, d, h, mi, s;
    GetTimestamp(y, mo, d, h, mi, s);
    char buf[256];
    snprintf(buf, sizeof(buf), "rtx_%04d%02d%02d_%02d%02d%02d_manual%03u.bmp",
             y, mo, d, h, mi, s, g_manualCaptureCount);
    return std::string(buf);
}

/// Generate a timestamp-based filename: rtx_YYYYMMDD_HHMMSS_frameNNNNN.bmp
/// Fallback for programmatic captures when no explicit name is given.
static std::string GenerateTimestampFilename() {
    int y, mo, d, h, mi, s;
    GetTimestamp(y, mo, d, h, mi, s);
    char buf[256];
    snprintf(buf, sizeof(buf), "rtx_%04d%02d%02d_%02d%02d%02d_frame%05u.bmp",
             y, mo, d, h, mi, s, g_frameCounter);
    return std::string(buf);
}

/// Write the marker file screenshots/rtx_capture_marker.txt on first successful capture.
/// This is a canary to verify captures happened.
static void WriteMarkerFile() {
    if (g_markerFileWritten) return;
    g_markerFileWritten = true;

    EnsureScreenshotDir();
    std::string markerPath = g_outputDir + "rtx_capture_marker.txt";

    FILE* fp = fopen(markerPath.c_str(), "w");
    if (!fp) {
        // Try absolute path as fallback
        markerPath = "C:\\Users\\aj12a\\programming\\Shipwright-3\\screenshots\\rtx_capture_marker.txt";
        fp = fopen(markerPath.c_str(), "w");
    }
    if (!fp) {
        CaptureDebugLog("[RTX_ScreenCapture] WriteMarkerFile() FAILED — cannot open marker file");
        RTX_DIAG("WriteMarkerFile() FAILED — cannot open marker file");
        return;
    }

    int y, mo, d, h, mi, s;
    GetTimestamp(y, mo, d, h, mi, s);

    fprintf(fp, "RTX Capture Marker\n");
    fprintf(fp, "==================\n");
    fprintf(fp, "Timestamp: %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, mi, s);
    fprintf(fp, "Frame: %u\n", g_frameCounter);
    fprintf(fp, "Output directory: %s\n", g_outputDir.c_str());
    fprintf(fp, "Capture initialized: %s\n", g_captureInitialized ? "YES" : "NO");
    fprintf(fp, "This file proves a screenshot capture was attempted.\n");
    fclose(fp);

    CaptureDebugLog("[RTX_ScreenCapture] WriteMarkerFile() SUCCESS — wrote '%s' at frame %u", markerPath.c_str(), g_frameCounter);
    RTX_DIAG("WriteMarkerFile() SUCCESS — wrote '%s' at frame %u", markerPath.c_str(), g_frameCounter);

    // Also write marker to absolute path for discovery
    std::string absMarker = "C:\\Users\\aj12a\\programming\\Shipwright-3\\screenshots\\rtx_capture_marker.txt";
    if (markerPath != absMarker) {
        FILE* fp2 = fopen(absMarker.c_str(), "w");
        if (fp2) {
            fprintf(fp2, "RTX Capture Marker\n");
            fprintf(fp2, "==================\n");
            fprintf(fp2, "Timestamp: %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, mi, s);
            fprintf(fp2, "Frame: %u\n", g_frameCounter);
            fprintf(fp2, "Output directory: %s\n", g_outputDir.c_str());
            fprintf(fp2, "This file proves a screenshot capture was attempted.\n");
            fclose(fp2);
        }
    }
}

/// Capture the given back buffer resource to a BMP file.
/// The back buffer is expected to be in D3D12_RESOURCE_STATE_PRESENT after swap chain Present().
static void CaptureBackBuffer(ID3D12Resource* backBuffer, ID3D12Device* device,
                              ID3D12CommandQueue* queue, DXGI_FORMAT format,
                              UINT width, UINT height, const char* filename) {
    if (!backBuffer) {
        CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() SKIPPED — null backBuffer");
        RTX_DIAG("CaptureBackBuffer() — null backBuffer");
        return;
    }
    if (!device) {
        CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() SKIPPED — null device");
        RTX_DIAG("CaptureBackBuffer() — null device");
        return;
    }
    if (!queue) {
        CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() SKIPPED — null command queue");
        RTX_DIAG("CaptureBackBuffer() — null command queue");
        return;
    }
    if (!filename) {
        CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() SKIPPED — null filename");
        RTX_DIAG("CaptureBackBuffer() — null filename");
        return;
    }
    if (width == 0 || height == 0) {
        CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() SKIPPED — zero dimensions %ux%u", width, height);
        RTX_DIAG("CaptureBackBuffer() — zero dimensions %ux%u", width, height);
        return;
    }

    // Build full path using configured output directory (ABSOLUTE path resolved at init)
    EnsureScreenshotDir();
    std::string fullPath = g_outputDir + filename;

    // Also build the absolute project-root backup path
    std::string projectRootPath = std::string("C:\\Users\\aj12a\\programming\\Shipwright-3\\screenshots\\") + filename;
    // Ensure that project-root screenshots dir exists
    CreateDirectoryA("C:\\Users\\aj12a\\programming\\Shipwright-3\\screenshots", nullptr);

    // ======== REQUIRED LOG: "RTX CAPTURE: Attempting frame N to path X" ========
    OutputDebugStringA(("RTX CAPTURE: Attempting frame " + std::to_string(g_frameCounter) + " to path " + fullPath + "\n").c_str());
    CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() starting: frame=%u, %ux%u, format=%u, file='%s'",
                    g_frameCounter, width, height, (unsigned)format, fullPath.c_str());
    RTX_DIAG("CaptureBackBuffer() starting: %ux%u, format=%u, file='%s'", width, height, (unsigned)format, fullPath.c_str());

    HRESULT hr;

    // Calculate row pitch (aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT = 256)
    uint32_t bytesPerPixel = 4; // RGBA8 or BGRA8
    uint32_t rowPitch = (width * bytesPerPixel + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1)
                        & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    uint64_t totalSize = static_cast<uint64_t>(rowPitch) * height;

    // Create readback buffer (D3D12_HEAP_TYPE_READBACK)
    ComPtr<ID3D12Resource> readbackBuffer;
    {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;
        heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = totalSize;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hr = device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackBuffer));
        if (FAILED(hr)) {
            OutputDebugStringA(("RTX CAPTURE: FAILED error code " + std::to_string((unsigned long)hr) + " (CreateCommittedResource)\n").c_str());
            CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() FAILED — CreateCommittedResource readback: 0x%08lX (frame %u, file '%s')",
                            hr, g_frameCounter, filename);
            RTX_DIAG("CaptureBackBuffer() FAILED — CreateCommittedResource readback: 0x%08lX", hr);
            WriteFallbackBlueBMP(filename);
            return;
        }
    }

    // Create dedicated command allocator + command list for the copy operation
    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    if (FAILED(hr)) {
        OutputDebugStringA(("RTX CAPTURE: FAILED error code " + std::to_string((unsigned long)hr) + " (CreateCommandAllocator)\n").c_str());
        CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() FAILED — CreateCommandAllocator: 0x%08lX (frame %u, file '%s')",
                        hr, g_frameCounter, filename);
        RTX_DIAG("CaptureBackBuffer() FAILED — CreateCommandAllocator: 0x%08lX", hr);
        WriteFallbackBlueBMP(filename);
        return;
    }

    ComPtr<ID3D12GraphicsCommandList> cmdList;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList));
    if (FAILED(hr)) {
        OutputDebugStringA(("RTX CAPTURE: FAILED error code " + std::to_string((unsigned long)hr) + " (CreateCommandList)\n").c_str());
        CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() FAILED — CreateCommandList: 0x%08lX (frame %u, file '%s')",
                        hr, g_frameCounter, filename);
        RTX_DIAG("CaptureBackBuffer() FAILED — CreateCommandList: 0x%08lX", hr);
        WriteFallbackBlueBMP(filename);
        return;
    }

    // Transition back buffer: PRESENT -> COPY_SOURCE
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmdList->ResourceBarrier(1, &barrier);

    // Set up copy: texture -> buffer via CopyTextureRegion
    D3D12_TEXTURE_COPY_LOCATION srcLoc = {};
    srcLoc.pResource = backBuffer;
    srcLoc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    srcLoc.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION dstLoc = {};
    dstLoc.pResource = readbackBuffer.Get();
    dstLoc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dstLoc.PlacedFootprint.Offset = 0;
    dstLoc.PlacedFootprint.Footprint.Format = format;
    dstLoc.PlacedFootprint.Footprint.Width = width;
    dstLoc.PlacedFootprint.Footprint.Height = height;
    dstLoc.PlacedFootprint.Footprint.Depth = 1;
    dstLoc.PlacedFootprint.Footprint.RowPitch = rowPitch;

    cmdList->CopyTextureRegion(&dstLoc, 0, 0, 0, &srcLoc, nullptr);

    // Transition back buffer: COPY_SOURCE -> PRESENT
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cmdList->ResourceBarrier(1, &barrier);

    // Close and execute command list
    hr = cmdList->Close();
    if (FAILED(hr)) {
        OutputDebugStringA(("RTX CAPTURE: FAILED error code " + std::to_string((unsigned long)hr) + " (Close)\n").c_str());
        CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() FAILED — Close: 0x%08lX (frame %u, file '%s')",
                        hr, g_frameCounter, filename);
        RTX_DIAG("CaptureBackBuffer() FAILED — Close: 0x%08lX", hr);
        WriteFallbackBlueBMP(filename);
        return;
    }

    ID3D12CommandList* ppCmdLists[] = { cmdList.Get() };
    queue->ExecuteCommandLists(1, ppCmdLists);

    // Create fence and wait for GPU completion
    ComPtr<ID3D12Fence> fence;
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(hr)) {
        OutputDebugStringA(("RTX CAPTURE: FAILED error code " + std::to_string((unsigned long)hr) + " (CreateFence)\n").c_str());
        CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() FAILED — CreateFence: 0x%08lX (frame %u, file '%s')",
                        hr, g_frameCounter, filename);
        RTX_DIAG("CaptureBackBuffer() FAILED — CreateFence: 0x%08lX", hr);
        WriteFallbackBlueBMP(filename);
        return;
    }

    HANDLE fenceEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent) {
        OutputDebugStringA("RTX CAPTURE: FAILED error code 0 (CreateEvent returned NULL)\n");
        CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() FAILED — CreateEvent returned NULL (frame %u, file '%s')",
                        g_frameCounter, filename);
        RTX_DIAG("CaptureBackBuffer() FAILED — CreateEvent returned NULL");
        WriteFallbackBlueBMP(filename);
        return;
    }

    hr = queue->Signal(fence.Get(), 1);
    if (FAILED(hr)) {
        CloseHandle(fenceEvent);
        OutputDebugStringA(("RTX CAPTURE: FAILED error code " + std::to_string((unsigned long)hr) + " (Signal)\n").c_str());
        CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() FAILED — Signal: 0x%08lX (frame %u, file '%s')",
                        hr, g_frameCounter, filename);
        RTX_DIAG("CaptureBackBuffer() FAILED — Signal: 0x%08lX", hr);
        WriteFallbackBlueBMP(filename);
        return;
    }

    if (fence->GetCompletedValue() < 1) {
        hr = fence->SetEventOnCompletion(1, fenceEvent);
        if (SUCCEEDED(hr)) {
            DWORD waitResult = WaitForSingleObject(fenceEvent, 15000); // 15 second timeout
            if (waitResult == WAIT_OBJECT_0) {
                CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() — GPU fence signaled successfully (frame %u)",
                                g_frameCounter);
            } else if (waitResult == WAIT_TIMEOUT) {
                CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() WARNING — GPU fence TIMED OUT after 15s (frame %u). Readback data may be garbage.",
                                g_frameCounter);
                RTX_DIAG("CaptureBackBuffer() WARNING — GPU fence TIMED OUT after 15s (frame %u)", g_frameCounter);
                OutputDebugStringA("RTX CAPTURE: WARNING — GPU fence timed out, readback may contain garbage\n");
            } else {
                CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() WARNING — WaitForSingleObject returned %lu (frame %u)",
                                waitResult, g_frameCounter);
                RTX_DIAG("CaptureBackBuffer() WARNING — WaitForSingleObject returned %lu", waitResult);
            }
        } else {
            CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() WARNING — SetEventOnCompletion FAILED 0x%08lX (frame %u)",
                            hr, g_frameCounter);
            RTX_DIAG("CaptureBackBuffer() WARNING — SetEventOnCompletion FAILED 0x%08lX", hr);
        }
    } else {
        CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() — GPU already completed (no wait needed, frame %u)",
                        g_frameCounter);
    }
    CloseHandle(fenceEvent);

    // Map the readback buffer and read pixel data
    uint8_t* mappedData = nullptr;
    D3D12_RANGE readRange = { 0, static_cast<SIZE_T>(totalSize) };
    hr = readbackBuffer->Map(0, &readRange, reinterpret_cast<void**>(&mappedData));
    if (FAILED(hr) || !mappedData) {
        OutputDebugStringA(("RTX CAPTURE: FAILED error code " + std::to_string((unsigned long)hr) + " (Map)\n").c_str());
        CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() FAILED — Map: 0x%08lX (frame %u, file '%s')",
                        hr, g_frameCounter, filename);
        RTX_DIAG("CaptureBackBuffer() FAILED — Map: 0x%08lX", hr);
        WriteFallbackBlueBMP(filename);
        return;
    }

    // Write marker file on first capture attempt
    WriteMarkerFile();

    // Write BMP file to the configured output directory
    bool success = WriteBMP(fullPath, width, height, mappedData, rowPitch, format);

    // Also write a copy to the project root absolute path for easy discovery.
    // This ensures screenshots are findable even if the primary path fails.
    if (projectRootPath != fullPath) {
        bool copySuccess = WriteBMP(projectRootPath, width, height, mappedData, rowPitch, format);
        if (copySuccess) {
            CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() — backup copy written to '%s'", projectRootPath.c_str());
        } else {
            CaptureDebugLog("[RTX_ScreenCapture] CaptureBackBuffer() — backup copy FAILED for '%s'", projectRootPath.c_str());
        }
    }

    // Unmap
    D3D12_RANGE writeRange = { 0, 0 };
    readbackBuffer->Unmap(0, &writeRange);

    if (success) {
        // Get the actual file size for reporting
        uint32_t bmpRowSize = (width * 3 + 3) & ~3u;
        uint32_t bmpFileSize = 54 + bmpRowSize * height;
        // ======== REQUIRED LOG: "RTX CAPTURE: SUCCESS wrote N bytes" ========
        OutputDebugStringA(("RTX CAPTURE: SUCCESS wrote " + std::to_string(bmpFileSize) + " bytes\n").c_str());
        CaptureDebugLog("[RTX_ScreenCapture] CAPTURE SUCCESS — frame=%u file='%s' size=%ux%u bytes=%u",
                        g_frameCounter, fullPath.c_str(), width, height, bmpFileSize);
        RTX_DIAG("CaptureBackBuffer() SUCCESS — saved '%s' (%ux%u, %u bytes)", fullPath.c_str(), width, height, bmpFileSize);
        // Structured log line for automated test harness parsing
        printf("SCREENSHOT_CAPTURED: frame=%u path=%s size=%ux%u bytes=%u\n", g_frameCounter, fullPath.c_str(), width, height, bmpFileSize);
        fflush(stdout);
        RTX_DIAG("SCREENSHOT_CAPTURED: frame=%u path=%s size=%ux%u bytes=%u", g_frameCounter, fullPath.c_str(), width, height, bmpFileSize);
    } else {
        // ======== REQUIRED LOG: "RTX CAPTURE: FAILED error code N" ========
        OutputDebugStringA("RTX CAPTURE: FAILED error code 0 (WriteBMP returned false)\n");
        CaptureDebugLog("[RTX_ScreenCapture] CAPTURE FAILED — frame=%u file='%s' size=%ux%u (WriteBMP returned false)",
                        g_frameCounter, fullPath.c_str(), width, height);
        RTX_DIAG("CaptureBackBuffer() FAILED — WriteBMP returned false for '%s'", fullPath.c_str());
        printf("SCREENSHOT_FAILED: frame=%u path=%s size=%ux%u\n", g_frameCounter, fullPath.c_str(), width, height);
        fflush(stdout);
        WriteFallbackBlueBMP(filename);
    }
}

/// Write pixel data as a BMP file.
/// Handles DXGI_FORMAT_R8G8B8A8_UNORM and DXGI_FORMAT_B8G8R8A8_UNORM.
/// BMP format: BITMAPFILEHEADER (14) + BITMAPINFOHEADER (40) + pixel data (BGR, bottom-up).
static bool WriteBMP(const std::string& filepath, uint32_t width, uint32_t height,
                     const uint8_t* rgbaData, uint32_t srcRowPitch, DXGI_FORMAT format) {
    if (!rgbaData || width == 0 || height == 0) return false;

    FILE* fp = fopen(filepath.c_str(), "wb");
    if (!fp) {
        CaptureDebugLog("[RTX_ScreenCapture] WriteBMP() FAILED — cannot open '%s' for writing", filepath.c_str());
        RTX_DIAG("WriteBMP() FAILED — cannot open '%s' for writing", filepath.c_str());
        return false;
    }

    // Determine source pixel layout based on DXGI format
    // DXGI_FORMAT_R8G8B8A8_UNORM: byte order [R][G][B][A] -> need swap R<->B for BGR
    // DXGI_FORMAT_B8G8R8A8_UNORM: byte order [B][G][R][A] -> already BGR-like, just strip A
    // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: same byte layout as RGBA8
    // DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: same byte layout as BGRA8
    bool sourceIsBGRA = (format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                         format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                         format == DXGI_FORMAT_B8G8R8A8_TYPELESS);

    // BMP row size: 3 bytes per pixel (BGR), padded to 4-byte alignment
    uint32_t bmpRowSize = (width * 3 + 3) & ~3u;
    uint32_t imageSize = bmpRowSize * height;
    uint32_t fileSize = 54 + imageSize;

    // ---- BMP File Header (14 bytes) ----
    uint8_t fileHeader[14] = {};
    fileHeader[0] = 'B';
    fileHeader[1] = 'M';
    fileHeader[2]  = (uint8_t)(fileSize & 0xFF);
    fileHeader[3]  = (uint8_t)((fileSize >> 8) & 0xFF);
    fileHeader[4]  = (uint8_t)((fileSize >> 16) & 0xFF);
    fileHeader[5]  = (uint8_t)((fileSize >> 24) & 0xFF);
    // Reserved bytes [6..9] = 0
    fileHeader[10] = 54;  // Offset to pixel data
    fileHeader[11] = 0;
    fileHeader[12] = 0;
    fileHeader[13] = 0;

    // ---- BMP Info Header (40 bytes) ----
    uint8_t infoHeader[40] = {};
    infoHeader[0] = 40; // Header size
    // Width (little-endian)
    infoHeader[4]  = (uint8_t)(width & 0xFF);
    infoHeader[5]  = (uint8_t)((width >> 8) & 0xFF);
    infoHeader[6]  = (uint8_t)((width >> 16) & 0xFF);
    infoHeader[7]  = (uint8_t)((width >> 24) & 0xFF);
    // Height (little-endian, positive = bottom-up)
    infoHeader[8]  = (uint8_t)(height & 0xFF);
    infoHeader[9]  = (uint8_t)((height >> 8) & 0xFF);
    infoHeader[10] = (uint8_t)((height >> 16) & 0xFF);
    infoHeader[11] = (uint8_t)((height >> 24) & 0xFF);
    // Planes
    infoHeader[12] = 1;
    infoHeader[13] = 0;
    // Bits per pixel (24 = BGR)
    infoHeader[14] = 24;
    infoHeader[15] = 0;
    // Compression (0 = BI_RGB, no compression)
    infoHeader[16] = 0;
    // Image size
    infoHeader[20] = (uint8_t)(imageSize & 0xFF);
    infoHeader[21] = (uint8_t)((imageSize >> 8) & 0xFF);
    infoHeader[22] = (uint8_t)((imageSize >> 16) & 0xFF);
    infoHeader[23] = (uint8_t)((imageSize >> 24) & 0xFF);
    // Pixels per meter (72 DPI ~= 2835 ppm)
    infoHeader[24] = 0x13;
    infoHeader[25] = 0x0B;
    infoHeader[28] = 0x13;
    infoHeader[29] = 0x0B;

    fwrite(fileHeader, 1, 14, fp);
    fwrite(infoHeader, 1, 40, fp);

    // ---- Pixel Data (bottom-up, BGR) ----
    // GPU readback is top-down, BMP is bottom-up, so iterate rows in reverse.
    uint8_t* rowBuf = new uint8_t[bmpRowSize];
    memset(rowBuf, 0, bmpRowSize);

    for (int y = (int)height - 1; y >= 0; y--) {
        const uint8_t* srcRow = rgbaData + (uint64_t)y * srcRowPitch;

        if (sourceIsBGRA) {
            // Source is [B][G][R][A] — BMP wants [B][G][R], just strip alpha
            for (uint32_t x = 0; x < width; x++) {
                rowBuf[x * 3 + 0] = srcRow[x * 4 + 0]; // B
                rowBuf[x * 3 + 1] = srcRow[x * 4 + 1]; // G
                rowBuf[x * 3 + 2] = srcRow[x * 4 + 2]; // R
            }
        } else {
            // Source is [R][G][B][A] — BMP wants [B][G][R], swap R and B
            for (uint32_t x = 0; x < width; x++) {
                uint8_t r = srcRow[x * 4 + 0];
                uint8_t g = srcRow[x * 4 + 1];
                uint8_t b = srcRow[x * 4 + 2];
                rowBuf[x * 3 + 0] = b; // B
                rowBuf[x * 3 + 1] = g; // G
                rowBuf[x * 3 + 2] = r; // R
            }
        }

        fwrite(rowBuf, 1, bmpRowSize, fp);
    }

    delete[] rowBuf;
    fclose(fp);
    return true;
}

// ============================================================================
// WriteProofFile — writes a proof-of-execution text file at a known absolute path.
// This proves that the RTX screen capture code path is actually executing,
// regardless of whether any BMP screenshots are successfully written.
// ============================================================================
static void WriteProofFile() {
    g_proofFileWritten = true;

    FILE* fp = fopen(FALLBACK_PROOF_PATH, "w");
    if (!fp) {
        CaptureDebugLog("[RTX_ScreenCapture] WriteProofFile() FAILED — cannot open '%s'", FALLBACK_PROOF_PATH);
        RTX_DIAG("WriteProofFile() FAILED — cannot open '%s'", FALLBACK_PROOF_PATH);
        return;
    }

    int y, mo, d, h, mi, s;
    GetTimestamp(y, mo, d, h, mi, s);

    fprintf(fp, "RTX Screen Capture Proof of Execution\n");
    fprintf(fp, "======================================\n");
    fprintf(fp, "Timestamp: %04d-%02d-%02d %02d:%02d:%02d\n", y, mo, d, h, mi, s);
    fprintf(fp, "Frame counter at proof write: %u\n", g_frameCounter);
    fprintf(fp, "Capture initialized: %s\n", g_captureInitialized ? "YES" : "NO");
    fprintf(fp, "Device: %p\n", (void*)g_captureDevice);
    fprintf(fp, "Queue: %p\n", (void*)g_captureQueue);
    fprintf(fp, "Output directory: %s\n", g_outputDir.c_str());
    fprintf(fp, "Auto-capture frames: 3, 5, 10, 30, 60\n");
    fprintf(fp, "Auto-capture interval: %d\n", g_autoCaptureInterval);
    fprintf(fp, "\nThis file proves RTX_CaptureAfterPresent() is being called.\n");
    fprintf(fp, "If this file exists but no BMPs exist, the DX12 readback pipeline is failing.\n");
    fclose(fp);

    CaptureDebugLog("[RTX_ScreenCapture] WriteProofFile() SUCCESS — wrote '%s' at frame %u", FALLBACK_PROOF_PATH, g_frameCounter);
    RTX_DIAG("WriteProofFile() SUCCESS — wrote '%s' at frame %u", FALLBACK_PROOF_PATH, g_frameCounter);
}

// ============================================================================
// WriteFallbackBlueBMP — Writes a 100x100 solid blue BMP as proof that the
// capture code path executed, even when the D3D12 readback pipeline fails.
// This is a SIMPLE fallback that doesn't require any GPU resources.
// ============================================================================
static void WriteFallbackBlueBMP(const char* filename) {
    EnsureScreenshotDir();

    // Build the fallback filename: prepend "fallback_" to the given filename
    std::string fallbackName;
    if (filename && filename[0] != '\0') {
        fallbackName = std::string("fallback_") + filename;
    } else {
        int y, mo, d, h, mi, s;
        GetTimestamp(y, mo, d, h, mi, s);
        char buf[256];
        snprintf(buf, sizeof(buf), "fallback_rtx_%04d%02d%02d_%02d%02d%02d_frame%03u.bmp",
                 y, mo, d, h, mi, s, g_frameCounter);
        fallbackName = buf;
    }

    std::string fullPath = g_outputDir + fallbackName;

    OutputDebugStringA(("RTX CAPTURE: Writing fallback blue BMP to " + fullPath + "\n").c_str());
    CaptureDebugLog("[RTX_ScreenCapture] WriteFallbackBlueBMP() — writing 100x100 blue BMP to '%s'", fullPath.c_str());
    RTX_DIAG("WriteFallbackBlueBMP() — writing 100x100 blue BMP to '%s'", fullPath.c_str());

    const uint32_t W = 100;
    const uint32_t H = 100;
    // BMP row: 3 bytes per pixel, padded to 4-byte boundary
    // 100 * 3 = 300, padded to 300 (already aligned to 4)
    const uint32_t bmpRowSize = (W * 3 + 3) & ~3u;
    const uint32_t imageSize = bmpRowSize * H;
    const uint32_t fileSize = 54 + imageSize;

    // BMP File Header (14 bytes)
    uint8_t fileHeader[14] = {};
    fileHeader[0] = 'B';
    fileHeader[1] = 'M';
    fileHeader[2]  = (uint8_t)(fileSize & 0xFF);
    fileHeader[3]  = (uint8_t)((fileSize >> 8) & 0xFF);
    fileHeader[4]  = (uint8_t)((fileSize >> 16) & 0xFF);
    fileHeader[5]  = (uint8_t)((fileSize >> 24) & 0xFF);
    fileHeader[10] = 54; // Offset to pixel data

    // BMP Info Header (40 bytes)
    uint8_t infoHeader[40] = {};
    infoHeader[0] = 40; // Header size
    infoHeader[4]  = (uint8_t)(W & 0xFF);
    infoHeader[5]  = (uint8_t)((W >> 8) & 0xFF);
    infoHeader[8]  = (uint8_t)(H & 0xFF);
    infoHeader[9]  = (uint8_t)((H >> 8) & 0xFF);
    infoHeader[12] = 1;  // Planes
    infoHeader[14] = 24; // Bits per pixel
    infoHeader[20] = (uint8_t)(imageSize & 0xFF);
    infoHeader[21] = (uint8_t)((imageSize >> 8) & 0xFF);
    infoHeader[22] = (uint8_t)((imageSize >> 16) & 0xFF);
    infoHeader[23] = (uint8_t)((imageSize >> 24) & 0xFF);

    // Prepare the blue pixel row once
    uint8_t rowBuf[304] = {}; // bmpRowSize is 300, use 304 for safety
    for (uint32_t x = 0; x < W; x++) {
        rowBuf[x * 3 + 0] = 255; // B
        rowBuf[x * 3 + 1] = 0;   // G
        rowBuf[x * 3 + 2] = 0;   // R
    }

    // Helper lambda to write the BMP to a given path
    auto writeBlueBmpTo = [&](const std::string& path) -> bool {
        FILE* fp = fopen(path.c_str(), "wb");
        if (!fp) return false;
        fwrite(fileHeader, 1, 14, fp);
        fwrite(infoHeader, 1, 40, fp);
        for (uint32_t y = 0; y < H; y++) {
            fwrite(rowBuf, 1, bmpRowSize, fp);
        }
        fclose(fp);
        return true;
    };

    // Try the primary output directory path
    bool wrote = writeBlueBmpTo(fullPath);

    // Also write to project root screenshots/ as backup
    std::string absPath = "C:\\Users\\aj12a\\programming\\Shipwright-3\\screenshots\\" + fallbackName;
    CreateDirectoryA("C:\\Users\\aj12a\\programming\\Shipwright-3\\screenshots", nullptr);
    bool wroteBackup = writeBlueBmpTo(absPath);

    if (!wrote && !wroteBackup) {
        CaptureDebugLog("[RTX_ScreenCapture] WriteFallbackBlueBMP() FAILED — cannot open file for writing at '%s' or '%s'",
                        fullPath.c_str(), absPath.c_str());
        RTX_DIAG("WriteFallbackBlueBMP() FAILED — cannot open file for writing");
        OutputDebugStringA("RTX CAPTURE: FAILED fallback blue BMP write — cannot open file\n");
        return;
    }

    std::string reportPath = wrote ? fullPath : absPath;
    OutputDebugStringA(("RTX CAPTURE: Fallback blue BMP written to " + reportPath + " (" + std::to_string(fileSize) + " bytes)\n").c_str());
    CaptureDebugLog("[RTX_ScreenCapture] WriteFallbackBlueBMP() SUCCESS — wrote '%s' (%u bytes, backup=%s)",
                    reportPath.c_str(), fileSize, wroteBackup ? "yes" : "no");
    RTX_DIAG("WriteFallbackBlueBMP() SUCCESS — wrote '%s' (%u bytes)", reportPath.c_str(), fileSize);
    printf("SCREENSHOT_FALLBACK: frame=%u path=%s size=100x100 bytes=%u\n", g_frameCounter, reportPath.c_str(), fileSize);
    fflush(stdout);
}

/// Called when a capture attempt on an auto-capture frame fails.
/// Writes the fallback blue BMP with the current frame's timestamp-based name.
static void WriteFallbackBlueBMPForFrame() {
    std::string name = GenerateTimestampedFrameFilename();
    WriteFallbackBlueBMP(name.c_str());
}

#endif // ENABLE_DX12_RTX
