#ifdef ENABLE_DX12_RTX

#include "RTXShaderCompiler.h"
#include <spdlog/spdlog.h>
#include <fstream>
#include <filesystem>

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

    HRESULT hr = DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&m_dxcLibrary));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX ShaderCompiler] Failed to create DXC library: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&m_dxcCompiler));
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX ShaderCompiler] Failed to create DXC compiler: 0x{:08X}", (uint32_t)hr);
        return false;
    }

    m_initialized = true;
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

    // Read shader file
    ComPtr<IDxcBlobEncoding> sourceBlob;
    HRESULT hr = m_dxcLibrary->CreateBlobFromFile(filePath.c_str(), nullptr, &sourceBlob);
    if (FAILED(hr)) {
        SPDLOG_ERROR("[RTX ShaderCompiler] Failed to load shader file");
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
        nullptr,  // include handler
        &result
    );

    if (SUCCEEDED(hr)) {
        result->GetStatus(&hr);
    }

    if (FAILED(hr)) {
        ComPtr<IDxcBlobEncoding> errors;
        result->GetErrorBuffer(&errors);
        if (errors && errors->GetBufferSize() > 0) {
            SPDLOG_ERROR("[RTX ShaderCompiler] Shader compilation error: {}",
                         (const char*)errors->GetBufferPointer());
        }
        return nullptr;
    }

    ComPtr<IDxcBlob> compiled;
    result->GetResult(&compiled);

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

    // Compile
    ComPtr<IDxcOperationResult> result;
    hr = m_dxcCompiler->Compile(
        sourceBlob.Get(),
        sourceName.c_str(),
        entryPoint,
        target,
        nullptr, 0,    // arguments
        nullptr, 0,    // defines
        nullptr,       // include handler
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
        SPDLOG_ERROR("[RTX ShaderCompiler] Precompiled shader is empty: {}",
                     std::filesystem::path(filePath).filename().string());
        return nullptr;
    }

    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(fileSize);
    file.read(reinterpret_cast<char*>(data.data()), fileSize);
    file.close();

    // Create a DXC blob from the raw data
    if (!m_initialized) {
        // If DXC isn't initialized, we can still create a simple blob wrapper
        // For now, require initialization
        SPDLOG_ERROR("[RTX ShaderCompiler] Compiler not initialized for LoadPrecompiledShader");
        return nullptr;
    }

    ComPtr<IDxcBlobEncoding> blob;
    HRESULT hr = m_dxcLibrary->CreateBlobWithEncodingFromPinned(
        data.data(), static_cast<UINT32>(data.size()), 0, &blob);
    if (FAILED(hr)) {
        // Fallback: create blob with encoding on copy
        hr = m_dxcLibrary->CreateBlobWithEncodingOnHeapCopy(
            data.data(), static_cast<UINT32>(data.size()), 0, &blob);
        if (FAILED(hr)) {
            SPDLOG_ERROR("[RTX ShaderCompiler] Failed to create blob from precompiled shader");
            return nullptr;
        }
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

    // Try precompiled first
    auto blob = LoadPrecompiledShader(precompiledPath);
    if (blob) {
        CacheShader(cacheName, blob);
        return blob;
    }

    // Fall back to runtime compilation
    SPDLOG_INFO("[RTX ShaderCompiler] Precompiled shader not found, compiling from source: {}",
                std::filesystem::path(hlslPath).filename().string());

    // Extract include directory from hlsl path
    std::wstring includeDir = std::filesystem::path(hlslPath).parent_path().wstring();
    std::vector<std::wstring> includePaths = { includeDir };

    blob = CompileFromFile(hlslPath, entryPoint, target, {}, includePaths);
    if (blob) {
        CacheShader(cacheName, blob);
    }
    return blob;
}

} // namespace RTX

#endif // ENABLE_DX12_RTX
