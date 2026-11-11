#pragma once

enum class ParameterClass : uint16_t
{
    Scalar,
    Vector,
    MatrixRows,
    MatrixColumns,
    Object,
    Struct
};

enum class ParameterType : uint16_t
{
    Void,
    Bool,
    Int,
    Float,
    String,
    Texture,
    Texture1D,
    Texture2D,
    Texture3D,
    TextureCube,
    Sampler,
    Sampler1D,
    Sampler2D,
    Sampler3D,
    SamplerCube,
    PixelShader,
    VertexShader,
    PixelFragment,
    VertexFragment,
    Unsupported
};

struct StructMemberInfo
{
    be<uint32_t> name;
    be<uint32_t> typeInfo;
};

struct TypeInfo
{
    be<ParameterClass> parameterClass;
    be<ParameterType> parameterType;
    be<uint16_t> rows;
    be<uint16_t> columns;
    be<uint16_t> elements;
    be<uint16_t> structMembers;
    be<uint32_t> structMemberInfo;
};

enum class RegisterSet : uint16_t
{
    Bool,
    Int4,
    Float4,
    Sampler
};

struct ConstantInfo
{
    be<uint32_t> name;
    be<RegisterSet> registerSet;
    be<uint16_t> registerIndex;
    be<uint16_t> registerCount;
    be<uint16_t> reserved;
    be<uint32_t> typeInfo;
    be<uint32_t> defaultValue;
};

struct ConstantTable
{
    be<uint32_t> size;
    be<uint32_t> creator;
    be<uint32_t> version;
    be<uint32_t> constants;
    be<uint32_t> constantInfo;
    be<uint32_t> flags;
    be<uint32_t> target;
};

struct ConstantTableContainer
{
    be<uint32_t> size;
    ConstantTable constantTable;
};
