#include <deque>
#include <mutex>
#include <thread>

#include "shader.h"
#include "shader_recompiler.h"
#include "dxc_compiler.h"

#ifdef XENOS_RECOMP_AIR
#include "air_compiler.h"
#endif

static std::unique_ptr<uint8_t[]> readAllBytes(const char* filePath, size_t& fileSize)
{
    FILE* file = fopen(filePath, "rb");
    fseek(file, 0, SEEK_END);
    fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    auto data = std::make_unique<uint8_t[]>(fileSize);
    fread(data.get(), 1, fileSize, file);
    fclose(file);
    return data;
}

static void writeAllBytes(const char* filePath, const void* data, size_t dataSize)
{
    FILE* file = fopen(filePath, "wb");
    fwrite(data, 1, dataSize, file);
    fclose(file);
}

struct RecompiledShader
{
    uint8_t* data = nullptr;
    IDxcBlob* dxil = nullptr;
    std::vector<uint8_t> spirv;
    std::vector<uint8_t> air;
    uint32_t specConstantsMask = 0;
};

void recompileShader(RecompiledShader& shader, const std::string_view include, std::atomic<uint32_t>& progress, uint32_t numShaders)
{
    thread_local ShaderRecompiler recompiler;
    recompiler = {};
    recompiler.recompile(shader.data, include);

    if (const char* dumpDirectory = std::getenv("XENOS_RECOMP_DUMP_SOURCE_DIR"))
    {
        const auto* container = reinterpret_cast<const ShaderContainer*>(shader.data);
        const size_t containerSize = container->virtualSize + container->physicalSize;
        const uint64_t hash = XXH3_64bits(shader.data, containerSize);
        std::filesystem::create_directories(dumpDirectory);
        const auto path = std::filesystem::path(dumpDirectory) /
            fmt::format("{:016x}.hlsl", hash);
        writeAllBytes(path.string().c_str(), recompiler.out.data(), recompiler.out.size());
    }


    shader.specConstantsMask = recompiler.specConstantsMask;

    thread_local DxcCompiler dxcCompiler;

#ifdef XENOS_RECOMP_DXIL
    shader.dxil = dxcCompiler.compile(recompiler.out, recompiler.isPixelShader, recompiler.specConstantsMask != 0, false);
    assert(shader.dxil != nullptr);
    assert(*(reinterpret_cast<uint32_t *>(shader.dxil->GetBufferPointer()) + 1) != 0 && "DXIL was not signed properly!");
#endif

#ifdef XENOS_RECOMP_AIR
    shader.air = AirCompiler::compile(recompiler.out);
#endif

    IDxcBlob* spirv = dxcCompiler.compile(recompiler.out, recompiler.isPixelShader, false, true);
    assert(spirv != nullptr);

    bool result = smolv::Encode(spirv->GetBufferPointer(), spirv->GetBufferSize(), shader.spirv, smolv::kEncodeFlagStripDebugInfo);
    assert(result);

    spirv->Release();

    size_t currentProgress = ++progress;
    if ((currentProgress % 10) == 0 || (currentProgress == numShaders - 1))
        fmt::println("Recompiling shaders... {}%", currentProgress / float(numShaders) * 100.0f);
}

int main(int argc, char** argv)
{
#ifndef XENOS_RECOMP_INPUT
    if (argc < 4)
    {
        printf("Usage: XenosRecomp [input path] [output path] [shader common header file path]");
        return 0;
    }
#endif

    const char* input =
#ifdef XENOS_RECOMP_INPUT 
        XENOS_RECOMP_INPUT
#else
        argv[1]
#endif
    ;

    const char* output =
#ifdef XENOS_RECOMP_OUTPUT 
        XENOS_RECOMP_OUTPUT
#else
        argv[2]
#endif
        ;
    
    const char* includeInput =
#ifdef XENOS_RECOMP_INCLUDE_INPUT
        XENOS_RECOMP_INCLUDE_INPUT
#else
        argv[3]
#endif
        ;

    size_t includeSize = 0;
    auto includeData = readAllBytes(includeInput, includeSize);
    std::string_view include(reinterpret_cast<const char*>(includeData.get()), includeSize);

    if (std::filesystem::is_directory(input))
    {
        std::vector<std::unique_ptr<uint8_t[]>> files;
        std::map<XXH64_hash_t, RecompiledShader> shaders;
        std::map<XXH64_hash_t, std::string> shaderFilenames;
        uint32_t rawShaderContainers = 0;
        uint32_t rawPixelShaders = 0;
        uint32_t rawVertexShaders = 0;

        for (auto& file : std::filesystem::recursive_directory_iterator(input))
        {
            if (std::filesystem::is_directory(file))
            {
                continue;
            }
            
            size_t fileSize = 0;
            auto fileData = readAllBytes(file.path().string().c_str(), fileSize);
            bool foundAny = false;

            for (size_t i = 0; fileSize > sizeof(ShaderContainer) && i < fileSize - sizeof(ShaderContainer) - 1;)
            {
                auto shaderContainer = reinterpret_cast<const ShaderContainer*>(fileData.get() + i);
                size_t dataSize = shaderContainer->virtualSize + shaderContainer->physicalSize;

                // Burnout Revenge uses the earlier 0x0E shader container
                // version; newer games commonly use 0x11.
                uint32_t containerVersion = shaderContainer->flags & 0xFFFFFF00;
                bool isLegacyContainer = containerVersion == 0x102A0E00;
                bool hasExpectedTrailer = isLegacyContainer ||
                    (shaderContainer->field1C == 0 && shaderContainer->field20 == 0);
                if ((isLegacyContainer || containerVersion == 0x102A1100) &&
                    dataSize <= (fileSize - i) && hasExpectedTrailer)
                {
                    ++rawShaderContainers;
                    if ((shaderContainer->flags & 1) == 0)
                        ++rawPixelShaders;
                    else
                        ++rawVertexShaders;
                    XXH64_hash_t hash = XXH3_64bits(shaderContainer, dataSize);
                    auto shader = shaders.try_emplace(hash);
                    if (shader.second)
                    {
                        shader.first->second.data = fileData.get() + i;
                        foundAny = true;
                        shaderFilenames[hash] = file.path().string();
                    }

                    i += dataSize;
                }
                else
                {
                    i += sizeof(uint32_t);
                }
            }

            if (foundAny)
                files.emplace_back(std::move(fileData));
        }

        std::mutex shaderQueueMutex;
        std::deque<XXH64_hash_t> shaderQueue;
        for (const auto& [hash, _] : shaders)
        {
            shaderQueue.emplace_back(hash);
        }

        const uint32_t numThreads = std::max(std::thread::hardware_concurrency(), 1u);
        fmt::println("Found {} raw shader containers ({} pixel, {} vertex), {} unique shaders",
            rawShaderContainers, rawPixelShaders, rawVertexShaders, shaders.size());
        fmt::println("Recompiling shaders with {} threads", numThreads);

        std::atomic<uint32_t> progress = 0;
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (uint32_t i = 0; i < numThreads; i++)
        {
            threads.emplace_back([&]
            {
                while (true)
                {
                    XXH64_hash_t shaderHash;
                    {
                        std::lock_guard lock(shaderQueueMutex);
                        if (shaderQueue.empty()) {
                            return;
                        }
                        shaderHash = shaderQueue.front();
                        shaderQueue.pop_front();
                    }
                    recompileShader(shaders[shaderHash], include, progress, shaders.size());
                }
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }

        fmt::println("Creating shader cache...");

        StringBuffer f;
        f.println("#include \"shader_cache.h\"");
        f.println("ShaderCacheEntry g_shaderCacheEntries[] = {{");

        std::vector<uint8_t> dxil;
        std::vector<uint8_t> spirv;
        std::vector<uint8_t> air;
        struct MicrocodeEntry
        {
            XXH64_hash_t microcodeHash;
            XXH64_hash_t shaderHash;
            uint32_t byteSize;
            uint32_t stage;
        };
        std::vector<MicrocodeEntry> microcodeEntries;

        for (auto& [hash, shader] : shaders)
        {
            const std::string& fullFilename = shaderFilenames[hash];
            std::string filename = fullFilename;
            size_t shaderPos = filename.find("shader");
            if (shaderPos != std::string::npos) {
                filename = filename.substr(shaderPos);
                // Prevent bad escape sequences in Windows shader path.
                std::replace(filename.begin(), filename.end(), '\\', '/');
            }
            f.println("\t{{ 0x{:X}, {}, {}, {}, {}, {}, {}, {}, \"{}\" }},",
                hash, dxil.size(), (shader.dxil != nullptr) ? shader.dxil->GetBufferSize() : 0,
                spirv.size(), shader.spirv.size(), air.size(), shader.air.size(), shader.specConstantsMask, filename);

            const auto container = reinterpret_cast<const ShaderContainer*>(shader.data);
            const uint32_t containerVersion = container->flags & 0xFFFFFF00u;
            uint32_t codeOffset = container->virtualSize;
            uint32_t codeSize = container->physicalSize;
            if (containerVersion == 0x102A1100u)
            {
                const auto shaderInfo = reinterpret_cast<const Shader*>(
                    shader.data + container->shaderOffset);
                codeOffset += shaderInfo->physicalOffset;
                codeSize = shaderInfo->size;
            }
            if (codeSize != 0 && codeOffset <= container->virtualSize + container->physicalSize &&
                codeSize <= container->virtualSize + container->physicalSize - codeOffset)
            {
                microcodeEntries.push_back({
                    XXH3_64bits(shader.data + codeOffset, codeSize), hash, codeSize,
                    (container->flags & 1u) != 0 ? 0u : 1u});
            }

            if (shader.dxil != nullptr)
            {
                dxil.insert(dxil.end(), reinterpret_cast<uint8_t *>(shader.dxil->GetBufferPointer()),
                    reinterpret_cast<uint8_t *>(shader.dxil->GetBufferPointer()) + shader.dxil->GetBufferSize());
            }

#ifdef XENOS_RECOMP_AIR
            air.insert(air.end(), shader.air.begin(), shader.air.end());
#endif

            spirv.insert(spirv.end(), shader.spirv.begin(), shader.spirv.end());
        }

        f.println("}};");
        f.println("const size_t g_shaderCacheEntryCount = sizeof(g_shaderCacheEntries) / sizeof(g_shaderCacheEntries[0]);");

        std::sort(microcodeEntries.begin(), microcodeEntries.end(),
            [](const MicrocodeEntry& a, const MicrocodeEntry& b) {
                return a.microcodeHash < b.microcodeHash;
            });
        microcodeEntries.erase(std::unique(microcodeEntries.begin(), microcodeEntries.end(),
            [](const MicrocodeEntry& a, const MicrocodeEntry& b) {
                return a.microcodeHash == b.microcodeHash;
            }), microcodeEntries.end());
        f.println("ShaderMicrocodeEntry g_shaderMicrocodeEntries[] = {{");
        for (const auto& entry : microcodeEntries)
            f.println("\t{{ 0x{:X}, 0x{:X}, {}, {} }},", entry.microcodeHash,
                entry.shaderHash, entry.byteSize, entry.stage);
        f.println("}};");
        f.println("const size_t g_shaderMicrocodeEntryCount = sizeof(g_shaderMicrocodeEntries) / sizeof(g_shaderMicrocodeEntries[0]);");

        fmt::println("Compressing DXIL cache...");

        int level = ZSTD_maxCLevel();

#ifdef XENOS_RECOMP_DXIL
        std::vector<uint8_t> dxilCompressed(ZSTD_compressBound(dxil.size()));
        dxilCompressed.resize(ZSTD_compress(dxilCompressed.data(), dxilCompressed.size(), dxil.data(), dxil.size(), level));

        f.print("const uint8_t g_compressedDxilCache[] = {{");

        for (auto data : dxilCompressed)
            f.print("{},", data);

        f.println("}};");
        f.println("const size_t g_dxilCacheCompressedSize = {};", dxilCompressed.size());
        f.println("const size_t g_dxilCacheDecompressedSize = {};", dxil.size());
#endif

#ifdef XENOS_RECOMP_AIR
        fmt::println("Compressing AIR cache...");

        std::vector<uint8_t> airCompressed(ZSTD_compressBound(air.size()));
        airCompressed.resize(ZSTD_compress(airCompressed.data(), airCompressed.size(), air.data(), air.size(), level));

        f.print("const uint8_t g_compressedAirCache[] = {{");

        for (auto data : airCompressed)
            f.print("{},", data);

        f.println("}};");
        f.println("const size_t g_airCacheCompressedSize = {};", airCompressed.size());
        f.println("const size_t g_airCacheDecompressedSize = {};", air.size());
#endif

        fmt::println("Compressing SPIRV cache...");

        std::vector<uint8_t> spirvCompressed(ZSTD_compressBound(spirv.size()));
        spirvCompressed.resize(ZSTD_compress(spirvCompressed.data(), spirvCompressed.size(), spirv.data(), spirv.size(), level));

        f.print("const uint8_t g_compressedSpirvCache[] = {{");

        for (auto data : spirvCompressed)
            f.print("{},", data);

        f.println("}};");

        f.println("const size_t g_spirvCacheCompressedSize = {};", spirvCompressed.size());
        f.println("const size_t g_spirvCacheDecompressedSize = {};", spirv.size());
        writeAllBytes(output, f.out.data(), f.out.size());
    }
    else
    {
        ShaderRecompiler recompiler;
        size_t fileSize;
        recompiler.recompile(readAllBytes(input, fileSize).get(), include);
        writeAllBytes(output, recompiler.out.data(), recompiler.out.size());
    }

    return 0;
}
