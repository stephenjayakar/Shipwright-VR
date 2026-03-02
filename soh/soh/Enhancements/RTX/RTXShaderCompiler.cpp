#ifdef ENABLE_DX12_RTX

#include "RTXShaderCompiler.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>
#include <cstdio>
#ifdef _WIN32
#include <Windows.h>
#endif

// Force runtime shader recompilation from HLSL source every time.
// This bypasses all precompiled .cso files to ensure HLSL edits always take effect.
// Set to 0 to re-enable CSO loading (once shader deployment pipeline is verified).
#define RTX_FORCE_SHADER_RECOMPILE 1

// Link DXC library
#pragma comment(lib, "dxcompiler.lib")

namespace RTX {

RTXShaderCompiler::RTXShaderCompiler() = default;

RTXShaderCompiler::~RTXShaderCompiler() {
    Shutdown();
}

bool RTXShaderCompiler::Initialize() {
    if (m_initialized) {
        return true;
    }

    OutputDebugStringA("[RTX ShaderCompiler] Initializing DXC compiler...\n");

    HRESULT hr = DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&m_dxcLibrary));
    if (FAILED(hr)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[RTX ShaderCompiler] FAILED to create DXC library: 0x%08X\n", (uint32_t)hr);
        OutputDebugStringA(msg);
        SPDLOG_ERROR("[RTX ShaderCompiler] Failed to create DXC library: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_dxcCompiler));
    if (FAILED(hr)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "[RTX ShaderCompiler] FAILED to create DXC compiler: 0x%08X\n", (uint32_t)hr);
        OutputDebugStringA(msg);
        SPDLOG_ERROR("[RTX ShaderCompiler] Failed to create DXC compiler: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    m_initialized = true;
    OutputDebugStringA("[RTX ShaderCompiler] DXC shader compiler initialized SUCCESSFULLY\n");
    SPDLOG_INFO("[RTX ShaderCompiler] DXC shader compiler initialized");
    return true;
}

void RTXShaderCompiler::Shutdown() {
    ClearCache();
    m_dxcCompiler.Reset();
    m_dxcLibrary.Reset();
    m_initialized = false;
}

ComPtr<IDxcBlob> RTXShaderCompiler::CompileFromFile(
    const std::wstring& filePath,
    const wchar_t* entryPoint,
    const wchar_t* target,
    const std::vector<std::pair<std::wstring, std::wstring>>& defines,
    const std::vector<std::wstring>& includePaths)
{
    if (!m_initialized) {
        SPDLOG_ERROR("[RTX ShaderCompiler] Compiler not initialized");
        return nullptr;
    }

    // Log what we're compiling
    {
        std::string narrowPath = std::filesystem::path(filePath).string();
        char compileMsg[512];
        snprintf(compileMsg, sizeof(compileMsg),
                 "[RTX ShaderCompiler] CompileFromFile: %s (entry='%ls', target='%ls')\n",
                 narrowPath.c_str(),
                 entryPoint ? entryPoint : L"(null)",
                 target ? target : L"(null)");
        OutputDebugStringA(compileMsg);

        // Also log file size and first 200 chars to verify we're reading the right file
        try {
            auto fsize = std::filesystem::file_size(filePath);
            snprintf(compileMsg, sizeof(compileMsg),
                     "[RTX ShaderCompiler] CompileFromFile: file size = %llu bytes\n",
                     (unsigned long long)fsize);
            OutputDebugStringA(compileMsg);

            // DIAGNOSTIC: Write shader source info to dump file
            FILE* shaderDiag = fopen("rtx_cb_dump.txt", "a");
            if (shaderDiag) {
                fprintf(shaderDiag, "  CompileFromFile: %s (%llu bytes, entry='%ls', target='%ls')\n",
                        narrowPath.c_str(), (unsigned long long)fsize,
                        entryPoint ? entryPoint : L"(null)",
                        target ? target : L"(null)");
                // Read first 200 chars of the file to verify content
                std::ifstream peek(filePath, std::ios::in);
                if (peek.is_open()) {
                    char preview[201] = {};
                    peek.read(preview, 200);
                    preview[200] = 0;
                    // Replace newlines with spaces for single-line display
                    for (int pi = 0; pi < 200; pi++) {
                        if (preview[pi] == '\n' || preview[pi] == '\r') preview[pi] = ' ';
                    }
                    fprintf(shaderDiag, "    FIRST 200 CHARS: %s\n", preview);
                    peek.close();
                }
                fclose(shaderDiag);
            }
        } catch (...) {
            OutputDebugStringA("[RTX ShaderCompiler] CompileFromFile: could not get file size\n");
        }
    }

    // Read shader file
    ComPtr<IDxcBlobEncoding> sourceBlob;
    HRESULT hr = m_dxcLibrary->CreateBlobFromFile(filePath.c_str(), nullptr, &sourceBlob);
    if (FAILED(hr)) {
        char errMsg[512];
        snprintf(errMsg, sizeof(errMsg),
                 "[RTX ShaderCompiler] Failed to load shader file: %ls (hr=0x%08X)\n",
                 filePath.c_str(), (uint32_t)hr);
        OutputDebugStringA(errMsg);
        SPDLOG_ERROR("[RTX ShaderCompiler] Failed to load shader file: {} (hr=0x{:08X})",
                     std::filesystem::path(filePath).string(), (uint32_t)hr);
        return nullptr;
    }

    // Build DXC arguments
    std::vector<const wchar_t*> arguments;

    // Include paths
    std::vector<std::wstring> includeArgs;
    for (const auto& path : includePaths) {
        includeArgs.push_back(L"-I");
        includeArgs.push_back(path);
    }
    for (const auto& arg : includeArgs) {
        arguments.push_back(arg.c_str());
    }

    // Build define structures
    std::vector<DxcDefine> dxcDefines;
    dxcDefines.reserve(defines.size());
    for (const auto& [name, value] : defines) {
        DxcDefine def;
        def.Name = name.c_str();
        def.Value = value.empty() ? nullptr : value.c_str();
        dxcDefines.push_back(def);
    }

    // Create a default include handler so that #include directives in HLSL
    // resolve to files on disk (relative to the source file's directory).
    // Without this, IDxcCompiler::Compile silently fails to resolve #includes,
    // causing shader compilation to fail at runtime.
    ComPtr<IDxcIncludeHandler> includeHandler;
    hr = m_dxcLibrary->CreateIncludeHandler(&includeHandler);
    if (FAILED(hr)) {
        SPDLOG_WARN("[RTX ShaderCompiler] Failed to create include handler (0x{:08X}), proceeding without", (uint32_t)hr);
        // Fall through — some simple shaders may not need includes
    }

    // Compile
    ComPtr<IDxcOperationResult> result;
    hr = m_dxcCompiler->Compile(
        sourceBlob.Get(),
        filePath.c_str(),
        entryPoint,
        target,
        arguments.empty() ? nullptr : arguments.data(),
        static_cast<UINT32>(arguments.size()),
        dxcDefines.empty() ? nullptr : dxcDefines.data(),
        static_cast<UINT32>(dxcDefines.size()),
        includeHandler.Get(),  // include handler for #include resolution
        &result
    );

    if (SUCCEEDED(hr)) {
        result->GetStatus(&hr);
    }

    if (FAILED(hr)) {
        ComPtr<IDxcBlobEncoding> errors;
        if (result) result->GetErrorBuffer(&errors);
        if (errors && errors->GetBufferSize() > 0) {
            const char* errorText = (const char*)errors->GetBufferPointer();
            SPDLOG_ERROR("[RTX ShaderCompiler] Shader compilation error: {}", errorText);
            // Also output to debug console for immediate visibility
            OutputDebugStringA("[RTX ShaderCompiler] COMPILATION ERROR:\n");
            OutputDebugStringA(errorText);
            OutputDebugStringA("\n");
        } else {
            char hrMsg[128];
            snprintf(hrMsg, sizeof(hrMsg), "[RTX ShaderCompiler] Compilation failed with HRESULT=0x%08X (no error text)\n", (uint32_t)hr);
            OutputDebugStringA(hrMsg);
        }
        return nullptr;
    }

    ComPtr<IDxcBlob> compiled;
    result->GetResult(&compiled);

    {
        char successMsg[256];
        snprintf(successMsg, sizeof(successMsg),
                 "[RTX ShaderCompiler] Successfully compiled: %s (%zu bytes)\n",
                 std::filesystem::path(filePath).filename().string().c_str(),
                 compiled ? compiled->GetBufferSize() : 0);
        OutputDebugStringA(successMsg);

        // DIAGNOSTIC: Write compilation success to dump file with source content verification
        FILE* compileDiag = fopen("rtx_cb_dump.txt", "a");
        if (compileDiag) {
            fprintf(compileDiag, "  COMPILED OK: %s → %zu DXIL bytes\n",
                    std::filesystem::path(filePath).filename().string().c_str(),
                    compiled ? compiled->GetBufferSize() : 0);

            // Check if the source file contains our nuclear test marker
            std::ifstream checkFile(filePath, std::ios::in);
            if (checkFile.is_open()) {
                std::string content((std::istreambuf_iterator<char>(checkFile)),
                                     std::istreambuf_iterator<char>());
                checkFile.close();

                bool hasNuclearTest = (content.find("NUCLEAR TEST") != std::string::npos);
                bool hasMagenta = (content.find("float3(1.0, 0.0, 1.0)") != std::string::npos ||
                                   content.find("float4(1.0, 0.0, 1.0") != std::string::npos);
                bool hasDebugMode = (content.find("debugMode") != std::string::npos);
                bool hasAlbedoDebug = (content.find("debugMode == 1") != std::string::npos);
                fprintf(compileDiag, "    Source checks: NUCLEAR_TEST=%s, MAGENTA=%s, debugMode=%s, albedoDebug=%s\n",
                        hasNuclearTest ? "YES" : "no",
                        hasMagenta ? "YES" : "no",
                        hasDebugMode ? "YES" : "no",
                        hasAlbedoDebug ? "YES" : "no");
                fprintf(compileDiag, "    Source file length: %zu chars\n", content.size());
            }
            fclose(compileDiag);
        }
    }
    SPDLOG_INFO("[RTX ShaderCompiler] Successfully compiled shader: {} ({} bytes)",
                std::filesystem::path(filePath).filename().string(),
                compiled ? compiled->GetBufferSize() : 0);

    return compiled;
}

ComPtr<IDxcBlob> RTXShaderCompiler::CompileFromSource(
    const std::string& source,
    const std::wstring& sourceName,
    const wchar_t* entryPoint,
    const wchar_t* target)
{
    if (!m_initialized) {
        SPDLOG_ERROR("[RTX ShaderCompiler] Compiler not initialized");
        return nullptr;
    }

    // Create blob from source string
    ComPtr<IDxcBlobEncoding> sourceBlob;
    HRESULT hr = m_dxcLibrary->CreateBlobWithEncodingFromPinned(
        source.data(), static_cast<UINT32>(source.size()), CP_UTF8, &sourceBlob);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX ShaderCompiler] Failed to create blob from source");
        return nullptr;
    }

    // Create a default include handler
    ComPtr<IDxcIncludeHandler> includeHandler;
    m_dxcLibrary->CreateIncludeHandler(&includeHandler);

    // Compile
    ComPtr<IDxcOperationResult> result;
    hr = m_dxcCompiler->Compile(
        sourceBlob.Get(),
        sourceName.c_str(),
        entryPoint,
        target,
        nullptr, 0,    // arguments
        nullptr, 0,    // defines
        includeHandler.Get(),  // include handler for #include resolution
        &result
    );

    if (SUCCEEDED(hr)) {
        result->GetStatus(&hr);
    }

    if (FAILED(hr)) {
        ComPtr<IDxcBlobEncoding> errors;
        result->GetErrorBuffer(&errors);
        if (errors && errors->GetBufferSize() > 0) {
            SPDLOG_ERROR("[RTX ShaderCompiler] Source compilation error: {}",
                         (const char*)errors->GetBufferPointer());
        }
        return nullptr;
    }

    ComPtr<IDxcBlob> compiled;
    result->GetResult(&compiled);
    return compiled;
}

ComPtr<IDxcBlob> RTXShaderCompiler::LoadPrecompiledShader(const std::wstring& filePath) {
    // Check if file exists
    if (!std::filesystem::exists(filePath)) {
        SPDLOG_WARN("[RTX ShaderCompiler] Precompiled shader not found: {}",
                     std::filesystem::path(filePath).filename().string());
        return nullptr;
    }

    // Read binary file
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        SPDLOG_ERROR("[RTX ShaderCompiler] Failed to open precompiled shader: {}",
                     std::filesystem::path(filePath).filename().string());
        return nullptr;
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    if (fileSize == 0) {
        SPDLOG_ERROR("[RTX ShaderCompiler] Precompiled shader is empty (0 bytes): {}",
                     std::filesystem::path(filePath).filename().string());
        return nullptr;
    }
    // Sanity check: a valid DXIL/DXBC CSO file has at least a header (typically 32+ bytes).
    // Reject obviously corrupt or truncated files.
    if (fileSize < 32) {
        SPDLOG_ERROR("[RTX ShaderCompiler] Precompiled shader too small ({} bytes, expected >= 32) — likely corrupt: {}",
                     fileSize, std::filesystem::path(filePath).filename().string());
        return nullptr;
    }

    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(fileSize);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    if (!file.good() && !file.eof()) {
        SPDLOG_ERROR("[RTX ShaderCompiler] Failed to read precompiled shader data (read error): {}",
                     std::filesystem::path(filePath).filename().string());
        return nullptr;
    }
    file.close();

    // Create a DXC blob from the raw data
    if (!m_initialized) {
        // If DXC isn't initialized, we can still create a simple blob wrapper
        // For now, require initialization
        SPDLOG_ERROR("[RTX ShaderCompiler] Compiler not initialized for LoadPrecompiledShader");
        return nullptr;
    }

    ComPtr<IDxcBlobEncoding> blob;
    // Use OnHeapCopy (not FromPinned) because `data` is a local vector that
    // goes out of scope when this function returns.  FromPinned would create
    // a dangling-pointer blob (use-after-free).
    HRESULT hr = m_dxcLibrary->CreateBlobWithEncodingOnHeapCopy(
        data.data(), static_cast<UINT32>(data.size()), 0, &blob);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX ShaderCompiler] Failed to create blob from precompiled shader");
        return nullptr;
    }

    SPDLOG_INFO("[RTX ShaderCompiler] Loaded precompiled shader: {} ({} bytes)",
                std::filesystem::path(filePath).filename().string(), fileSize);

    // IDxcBlobEncoding derives from IDxcBlob, so we can return it directly
    ComPtr<IDxcBlob> result;
    blob.As(&result);
    return result;
}

ComPtr<IDxcBlob> RTXShaderCompiler::GetCachedShader(const std::wstring& name) const {
    auto it = m_shaderCache.find(name);
    if (it != m_shaderCache.end()) {
        return it->second;
    }
    return nullptr;
}

void RTXShaderCompiler::CacheShader(const std::wstring& name, ComPtr<IDxcBlob> blob) {
    if (blob) {
        m_shaderCache[name] = blob;
    }
}

void RTXShaderCompiler::ClearCache() {
    m_shaderCache.clear();
}

ComPtr<IDxcBlob> RTXShaderCompiler::LoadOrCompile(
    const std::wstring& precompiledPath,
    const std::wstring& hlslPath,
    const wchar_t* entryPoint,
    const wchar_t* target,
    const std::wstring& cacheName)
{
    // Check cache first
    auto cached = GetCachedShader(cacheName);
    if (cached) {
        SPDLOG_DEBUG("[RTX ShaderCompiler] Using cached shader: {}",
                     std::filesystem::path(cacheName).filename().string());
        return cached;
    }

    // Log all paths for diagnosis
    {
        std::string hlslNarrow = std::filesystem::path(hlslPath).string();
        std::string csoNarrow = std::filesystem::path(precompiledPath).string();
        bool hlslExists = false, csoExists = false;
        try { hlslExists = std::filesystem::exists(hlslPath); } catch (...) {}
        try { csoExists = std::filesystem::exists(precompiledPath); } catch (...) {}

        char msg[1024];
        snprintf(msg, sizeof(msg),
                 "[RTX ShaderCompiler] LoadOrCompile: cache='%ls'\n"
                 "  HLSL: %s (exists: %s)\n"
                 "  CSO:  %s (exists: %s)\n"
                 "  FORCE_RECOMPILE: %d\n",
                 cacheName.c_str(),
                 hlslNarrow.c_str(), hlslExists ? "YES" : "NO",
                 csoNarrow.c_str(), csoExists ? "YES" : "NO",
                 RTX_FORCE_SHADER_RECOMPILE);
        OutputDebugStringA(msg);
        SPDLOG_INFO("{}", msg);
    }

#if RTX_FORCE_SHADER_RECOMPILE
    // ALWAYS compile from HLSL source — never use precompiled CSOs.
    // This guarantees that any HLSL source edits take effect immediately.
    {
        bool hlslExists = false;
        try { hlslExists = std::filesystem::exists(hlslPath); } catch (...) {}

        if (hlslExists) {
            SPDLOG_INFO("[RTX ShaderCompiler] FORCE_RECOMPILE: Compiling from HLSL source: {}",
                        std::filesystem::path(hlslPath).filename().string());
            OutputDebugStringA("[RTX ShaderCompiler] FORCE_RECOMPILE: Compiling from HLSL source\n");

            std::wstring includeDir = std::filesystem::path(hlslPath).parent_path().wstring();
            std::vector<std::wstring> includePaths = { includeDir };

            auto compiledBlob = CompileFromFile(hlslPath, entryPoint, target, {}, includePaths);
            if (compiledBlob) {
                char successMsg[512];
                snprintf(successMsg, sizeof(successMsg),
                         "[RTX ShaderCompiler] FORCE_RECOMPILE SUCCESS: %ls (%zu bytes)\n",
                         cacheName.c_str(), compiledBlob->GetBufferSize());
                OutputDebugStringA(successMsg);
                SPDLOG_INFO("{}", successMsg);
                CacheShader(cacheName, compiledBlob);
                return compiledBlob;
            } else {
                char failMsg[512];
                snprintf(failMsg, sizeof(failMsg),
                         "[RTX ShaderCompiler] FORCE_RECOMPILE FAILED for %ls — falling through to CSO\n",
                         cacheName.c_str());
                OutputDebugStringA(failMsg);
                SPDLOG_ERROR("{}", failMsg);
                // Fall through to try CSO as last resort
            }
        } else {
            char noHlslMsg[512];
            snprintf(noHlslMsg, sizeof(noHlslMsg),
                     "[RTX ShaderCompiler] FORCE_RECOMPILE: HLSL not found at path, falling through to CSO: %ls\n",
                     hlslPath.c_str());
            OutputDebugStringA(noHlslMsg);
            SPDLOG_WARN("{}", noHlslMsg);
            // Fall through to try CSO
        }
    }
#else
    // Try precompiled first, but prefer HLSL source if it's newer than the CSO.
    // This prevents stale CSOs from shadowing updated HLSL shader source.
    bool preferHLSL = false;
    try {
        if (std::filesystem::exists(precompiledPath) && std::filesystem::exists(hlslPath)) {
            auto csoTime = std::filesystem::last_write_time(precompiledPath);
            auto hlslTime = std::filesystem::last_write_time(hlslPath);
            if (hlslTime > csoTime) {
                preferHLSL = true;
                SPDLOG_INFO("[RTX ShaderCompiler] HLSL source is newer than CSO — preferring runtime compilation: {}",
                            std::filesystem::path(hlslPath).filename().string());
            }
        }
    } catch (...) {
        // If timestamp check fails, fall through to normal logic
    }

    if (!preferHLSL) {
        auto precompiledBlob = LoadPrecompiledShader(precompiledPath);
        if (precompiledBlob) {
            CacheShader(cacheName, precompiledBlob);
            return precompiledBlob;
        }
    }
#endif

    // Fall back to (or prefer) runtime compilation from HLSL source
    SPDLOG_INFO("[RTX ShaderCompiler] Compiling from source (fallback): {}",
                std::filesystem::path(hlslPath).filename().string());

    // Extract include directory from hlsl path
    std::wstring includeDir = std::filesystem::path(hlslPath).parent_path().wstring();
    std::vector<std::wstring> includePaths = { includeDir };

    auto compiledBlob = CompileFromFile(hlslPath, entryPoint, target, {}, includePaths);
    if (compiledBlob) {
        CacheShader(cacheName, compiledBlob);
    } else {
        char finalFailMsg[512];
        snprintf(finalFailMsg, sizeof(finalFailMsg),
                 "[RTX ShaderCompiler] FINAL FAILURE: Could not compile or load shader '%ls'\n",
                 cacheName.c_str());
        OutputDebugStringA(finalFailMsg);
        SPDLOG_ERROR("{}", finalFailMsg);
    }
    return compiledBlob;
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
