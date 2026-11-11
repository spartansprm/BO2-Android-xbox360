#pragma once

#include "shader.h"
#include "shader_code.h"


#include <iterator>

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
    std::unordered_map<uint32_t, std::string> interpolators;
    std::unordered_map<uint32_t, const ConstantInfo*> float4Constants;
    std::unordered_map<uint32_t, const char*> boolConstants;
    std::unordered_map<uint32_t, const char*> samplers;
    std::unordered_map<uint32_t, uint32_t> ifEndLabels;
    uint32_t specConstantsMask = 0;

#ifdef UNLEASHED_RECOMP
    bool hasMtxProjection = false;
    bool hasMtxPrevInvViewProjection = false;
#endif

    void indent()
    {
        for (uint32_t i = 0; i < indentation; i++)
            out += '\t';
    }


    int predOpen_ = -1;


    size_t closeMark_ = std::string::npos;
    size_t closeEnd_ = std::string::npos;
    int closeCondition_ = -1;


    bool reopenClose(int wanted)
    {
        if (predOpen_ >= 0 || closeCondition_ != wanted)
            return false;
        if (closeEnd_ == std::string::npos || closeEnd_ != out.size())
            return false;
        out.resize(closeMark_);
        ++indentation;
        closeMark_ = closeEnd_ = std::string::npos;
        return true;
    }


    void annotateCloseCf(size_t braceMark, int cond)
    {
        if (cond >= 0 && predGuaranteed_ == cond)
        {
            closeMark_ = braceMark;
            closeEnd_ = out.size();
            closeCondition_ = cond;
        }
        else
        {
            closeMark_ = closeEnd_ = std::string::npos;
        }
    }


    int predGuaranteed_ = -1;

    void openPredicate(bool cond)
    {
        const int wanted = cond ? 1 : 0;
        if (predOpen_ == wanted)
            return;
        if (predOpen_ < 0 && predGuaranteed_ == wanted)
            return;


        if (reopenClose(wanted))
        {
            predOpen_ = wanted;
            return;
        }
        closePredicate();
        indent();
        println("if ({}p0)", cond ? "" : "!");
        indent();
        out += "{\n";
        ++indentation;
        predOpen_ = wanted;
    }

    void closePredicate()
    {
        if (predOpen_ < 0)
            return;
        --indentation;
        const size_t mark = out.size();
        indent();
        out += "}\n";

        closeMark_ = mark;
        closeEnd_ = out.size();
        closeCondition_ = predOpen_;
        predOpen_ = -1;
    }


    void closePredicateIfWrites(size_t mark)
    {
        if (out.find("p0 = ", mark) == std::string::npos)
            return;
        closePredicate();

        predGuaranteed_ = -1;


        closeMark_ = closeEnd_ = std::string::npos;
    }

    void printDstSwizzle(uint32_t dstSwizzle, bool operand);
    void printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle);

    void recompile(const VertexFetchInstruction& instr, uint32_t address);
    void recompile(const TextureFetchInstruction& instr, bool bicubic);
    void recompile(const AluInstruction& instr);

    void recompile(const uint8_t* shaderData, const std::string_view& include);
};
