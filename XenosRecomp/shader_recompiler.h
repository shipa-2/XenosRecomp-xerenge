#pragma once

#include "shader.h"
#include "shader_code.h"

struct StringBuffer
{
    std::string out;

    template<class... Args>
    void print(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
    }

    template<class... Args>
    void println(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
        out += '\n';
    }
};

struct ShaderRecompiler : StringBuffer
{
    uint32_t indentation = 0;
    bool isPixelShader = false;
    const uint8_t* constantTableData = nullptr;
    std::unordered_map<uint32_t, VertexElement> vertexElements;
    std::unordered_map<uint32_t, std::string> vertexElementNames;
    std::unordered_map<uint32_t, std::string> vertexElementSemantics;
    std::unordered_map<uint32_t, std::string> interpolators;
    std::unordered_map<uint32_t, const ConstantInfo*> float4Constants;
    std::unordered_map<uint32_t, const char*> boolConstants;
    std::unordered_map<uint32_t, const char*> samplers;
    std::unordered_map<uint32_t, uint32_t> ifEndLabels;
    uint32_t specConstantsMask = 0;
    // A vertex shader that builds its own instancing out of the index Xenos
    // puts in r0.x (Burnout's gNumVertices shaders). The host hands that index
    // in as an input, since what it draws is already expanded.
    bool guestIndexInput = false;

#ifdef UNLEASHED_RECOMP
    bool hasMtxProjection = false;
    bool hasMtxPrevInvViewProjection = false;
#endif

    void indent()
    {
        for (uint32_t i = 0; i < indentation; i++)
            out += '\t';
    }

    // Pixel shaders in half precision where it is safe (min16float, which
    // SPIR-V carries as RelaxedPrecision: mobile GPUs run it at twice the rate).
    // A temporary stays full precision when it reaches a texture coordinate,
    // the depth output or the address register, directly or through the
    // registers computed from it; the rest - colour, lighting, fog - goes to
    // half. XENOSRECOMP_FULL_PRECISION=1 keeps every shader as it was.
    bool halfPrecision = false;
    uint64_t preciseRegisters = 0;
    // Per ALU instruction, in the order they are written: whether its vector
    // result is computed in half (analyzePrecision), and which one is next.
    std::string halfReason;
    std::vector<uint8_t> aluHalf;
    uint32_t aluIndex = 0;
    // The ALU instruction being written puts its vector result in half.
    bool vectorDestHalf = false;
    // Its operands that are not half already go into half locals first,
    // declared ahead of the statement: cast where they stand, DXC leaves the
    // operation undecorated (a dot of two casts stays full precision).
    bool collectHalfTemps = false;
    std::vector<std::string> halfTemps;
    uint32_t halfTempCount = 0;
    bool IsHalfRegister(uint32_t reg) const
    {
        return halfPrecision && reg < 64 && ((preciseRegisters >> reg) & 1) == 0;
    }
    void analyzePrecision(const be<uint32_t>* code, uint32_t instrSize);

    uint32_t printDstSwizzle(uint32_t dstSwizzle, bool operand);
    void printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle);

    void recompile(const VertexFetchInstruction& instr, uint32_t address);
    void recompile(const TextureFetchInstruction& instr, bool bicubic);
    void recompile(const AluInstruction& instr);

    void recompile(const uint8_t* shaderData, const std::string_view& include);
};
