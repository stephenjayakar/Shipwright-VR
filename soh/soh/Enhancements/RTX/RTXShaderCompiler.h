#pragma once
#ifndef RTX_SHADER_COMPILER_H
#define RTX_SHADER_COMPILER_H

#ifdef ENABLE_DX12_RTX

#include <d3d12.h>
#include <dxcapi.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace RTX {

/// RTXShaderCompiler provides a standalone interface for compiling HLSL shaders
/// using the DirectX Shader Compiler (DXC). It supports:
///   - Runtime HLSL compilation to DXIL bytecode
///   - Loading precompiled .dxil blobs from disk (built by CMake via dxc)
///   - Caching compiled shaders to avoid redundant recompilation
///
/// This class is used by DXRPipeline and can be used independently for
/// compute shaders (e.g., Denoise, Accumulate, GlobalIllumination).
class RTXShaderCompiler {
public:
    RTXShaderCompiler();
    ~RTXShaderCompiler();

    /// Initialize the DXC compiler instances.
    /// Must be called before CompileFromFile or CompileFromSource.
    /// Returns true on success.
    bool Initialize();

    /// Shut down and release DXC compiler resources and cached blobs.
    void Shutdown();

    /// Check if the compiler has been initialized.
    bool IsInitialized() const { return m_initialized; }

    /// Compile an HLSL file from disk at runtime using DXC.
    /// @param filePath     Full path to the .hlsl file.
    /// @param entryPoint   Shader entry point name (e.g., L"RayGen", L"main").
    ///                     For library shaders (lib_6_3), pass L"" or nullptr.
    /// @param target       Shader profile (e.g., L"lib_6_3", L"cs_6_0").
    /// @param defines      Optional preprocessor defines (name=value pairs).
    /// @param includePaths Optional additional include directories.
    /// @return Compiled DXIL blob, or nullptr on failure.
    ComPtr<IDxcBlob> CompileFromFile(
        const std::wstring& filePath,
        const wchar_t* entryPoint,
        const wchar_t* target,
        const std::vector<std::pair<std::wstring, std::wstring>>& defines = {},
        const std::vector<std::wstring>& includePaths = {}
    );

    /// Compile HLSL source code from memory.
    /// @param source       HLSL source code string.
    /// @param sourceName   Name for error reporting (e.g., L"RayGen.hlsl").
    /// @param entryPoint   Shader entry point name.
    /// @param target       Shader profile.
    /// @return Compiled DXIL blob, or nullptr on failure.
    ComPtr<IDxcBlob> CompileFromSource(
        const std::string& source,
        const std::wstring& sourceName,
        const wchar_t* entryPoint,
        const wchar_t* target
    );

    /// Load a precompiled DXIL blob from disk.
    /// These are produced by CMake's add_custom_command calling dxc at build time.
    /// @param filePath Full path to the .dxil file.
    /// @return The loaded blob, or nullptr on failure.
    ComPtr<IDxcBlob> LoadPrecompiledShader(const std::wstring& filePath);

    /// Get a cached shader by name. Returns nullptr if not in cache.
    ComPtr<IDxcBlob> GetCachedShader(const std::wstring& name) const;

    /// Store a shader blob in the cache under the given name.
    void CacheShader(const std::wstring& name, ComPtr<IDxcBlob> blob);

    /// Clear all cached shader blobs.
    void ClearCache();

    /// Try to load a precompiled shader first; if not found, compile from source.
    /// @param precompiledPath Path to .dxil file (may not exist).
    /// @param hlslPath        Path to .hlsl source file (fallback).
    /// @param entryPoint      Shader entry point.
    /// @param target          Shader profile.
    /// @param cacheName       Name to cache the result under.
    /// @return Compiled/loaded blob, or nullptr on failure.
    ComPtr<IDxcBlob> LoadOrCompile(
        const std::wstring& precompiledPath,
        const std::wstring& hlslPath,
        const wchar_t* entryPoint,
        const wchar_t* target,
        const std::wstring& cacheName
    );

private:
    bool m_initialized = false;

    // DXC compiler instances
    ComPtr<IDxcLibrary> m_dxcLibrary;
    ComPtr<IDxcCompiler> m_dxcCompiler;

    // Shader cache: name -> compiled blob
    std::unordered_map<std::wstring, ComPtr<IDxcBlob>> m_shaderCache;
};

} // namespace RTX

#endif // ENABLE_DX12_RTX
#endif // RTX_SHADER_COMPILER_H
