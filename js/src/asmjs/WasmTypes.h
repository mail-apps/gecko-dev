/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2015 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_types_h
#define wasm_types_h

#include "mozilla/DebugOnly.h"
#include "mozilla/HashFunctions.h"
#include "mozilla/Move.h"

#include "NamespaceImports.h"

#include "ds/LifoAlloc.h"
#include "jit/IonTypes.h"
#include "js/UniquePtr.h"
#include "js/Utility.h"
#include "js/Vector.h"

namespace js {

class PropertyName;

namespace wasm {

using mozilla::Move;
using mozilla::DebugOnly;
using mozilla::MallocSizeOf;

// The ValType enum represents the WebAssembly "value type", which are used to
// specify the type of locals and parameters.

// FIXME: uint8_t would make more sense for the underlying storage class, but
// causes miscompilations in GCC (fixed in 4.8.5 and 4.9.3).
enum class ValType
{
    I32,
    I64,
    F32,
    F64,
    I32x4,
    F32x4,
    B32x4
};

typedef Vector<ValType, 8, SystemAllocPolicy> ValTypeVector;

static inline bool
IsSimdType(ValType vt)
{
    return vt == ValType::I32x4 || vt == ValType::F32x4 || vt == ValType::B32x4;
}

static inline jit::MIRType
ToMIRType(ValType vt)
{
    switch (vt) {
      case ValType::I32: return jit::MIRType_Int32;
      case ValType::I64: MOZ_CRASH("NYI");
      case ValType::F32: return jit::MIRType_Float32;
      case ValType::F64: return jit::MIRType_Double;
      case ValType::I32x4: return jit::MIRType_Int32x4;
      case ValType::F32x4: return jit::MIRType_Float32x4;
      case ValType::B32x4: return jit::MIRType_Bool32x4;
    }
    MOZ_MAKE_COMPILER_ASSUME_IS_UNREACHABLE("bad type");
}

// The Val class represents a single WebAssembly value of a given value type,
// mostly for the purpose of numeric literals and initializers. A Val does not
// directly map to a JS value since there is not (currently) a precise
// representation of i64 values. A Val may contain non-canonical NaNs since,
// within WebAssembly, floats are not canonicalized. Canonicalization must
// happen at the JS boundary.

class Val
{
  public:
    typedef int32_t I32x4[4];
    typedef float F32x4[4];

  private:
    ValType type_;
    union {
        uint32_t i32_;
        uint64_t i64_;
        float f32_;
        double f64_;
        I32x4 i32x4_;
        F32x4 f32x4_;
    } u;

  public:
    Val() = default;

    explicit Val(uint32_t i32) : type_(ValType::I32) { u.i32_ = i32; }
    explicit Val(uint64_t i64) : type_(ValType::I64) { u.i64_ = i64; }
    explicit Val(float f32) : type_(ValType::F32) { u.f32_ = f32; }
    explicit Val(double f64) : type_(ValType::F64) { u.f64_ = f64; }

    explicit Val(const I32x4& i32x4, ValType type = ValType::I32x4) : type_(type) {
        MOZ_ASSERT(type_ == ValType::I32x4 || type_ == ValType::B32x4);
        memcpy(u.i32x4_, i32x4, sizeof(u.i32x4_));
    }
    explicit Val(const F32x4& f32x4) : type_(ValType::F32x4) { memcpy(u.f32x4_, f32x4, sizeof(u.f32x4_)); }

    ValType type() const { return type_; }
    bool isSimd() const { return IsSimdType(type()); }

    uint32_t i32() const { MOZ_ASSERT(type_ == ValType::I32); return u.i32_; }
    uint64_t i64() const { MOZ_ASSERT(type_ == ValType::I64); return u.i64_; }
    float f32() const { MOZ_ASSERT(type_ == ValType::F32); return u.f32_; }
    double f64() const { MOZ_ASSERT(type_ == ValType::F64); return u.f64_; }
    const I32x4& i32x4() const {
        MOZ_ASSERT(type_ == ValType::I32x4 || type_ == ValType::B32x4);
        return u.i32x4_;
    }
    const F32x4& f32x4() const { MOZ_ASSERT(type_ == ValType::F32x4); return u.f32x4_; }
};

// The ExprType enum represents the type of a WebAssembly expression or return
// value and may either be a value type or void. A future WebAssembly extension
// may generalize expression types to instead be a list of value types (with
// void represented by the empty list). For now it's easier to have a flat enum
// and be explicit about conversions to/from value types.

enum class ExprType : uint8_t
{
    I32 = uint8_t(ValType::I32),
    I64 = uint8_t(ValType::I64),
    F32 = uint8_t(ValType::F32),
    F64 = uint8_t(ValType::F64),
    I32x4 = uint8_t(ValType::I32x4),
    F32x4 = uint8_t(ValType::F32x4),
    B32x4 = uint8_t(ValType::B32x4),
    Void
};

static inline bool
IsVoid(ExprType et)
{
    return et == ExprType::Void;
}

static inline ValType
NonVoidToValType(ExprType et)
{
    MOZ_ASSERT(!IsVoid(et));
    return ValType(et);
}

static inline ExprType
ToExprType(ValType vt)
{
    return ExprType(vt);
}

static inline bool
IsSimdType(ExprType et)
{
    return IsVoid(et) ? false : IsSimdType(ValType(et));
}

static inline bool
IsSimdBoolType(ExprType vt)
{
    return vt == ExprType::B32x4;
}

static inline jit::MIRType
ToMIRType(ExprType et)
{
    return IsVoid(et) ? jit::MIRType_None : ToMIRType(ValType(et));
}

// The Sig class represents a WebAssembly function signature which takes a list
// of value types and returns an expression type. The engine uses two in-memory
// representations of the argument Vector's memory (when elements do not fit
// inline): normal malloc allocation (via SystemAllocPolicy) and allocation in
// a LifoAlloc (via LifoAllocPolicy). The former Sig objects can have any
// lifetime since they own the memory. The latter Sig objects must not outlive
// the associated LifoAlloc mark/release interval (which is currently the
// duration of module validation+compilation). Thus, long-lived objects like
// WasmModule must use malloced allocation.

class Sig
{
    ValTypeVector args_;
    ExprType ret_;

    Sig(const Sig&) = delete;
    Sig& operator=(const Sig&) = delete;

  public:
    Sig() : args_(), ret_(ExprType::Void) {}
    Sig(Sig&& rhs) : args_(Move(rhs.args_)), ret_(rhs.ret_) {}
    Sig(ValTypeVector&& args, ExprType ret) : args_(Move(args)), ret_(ret) {}

    bool clone(const Sig& rhs) {
        ret_ = rhs.ret_;
        MOZ_ASSERT(args_.empty());
        return args_.appendAll(rhs.args_);
    }
    Sig& operator=(Sig&& rhs) {
        ret_ = rhs.ret_;
        args_ = Move(rhs.args_);
        return *this;
    }

    ValType arg(unsigned i) const { return args_[i]; }
    const ValTypeVector& args() const { return args_; }
    const ExprType& ret() const { return ret_; }

    HashNumber hash() const {
        HashNumber hn = HashNumber(ret_);
        for (unsigned i = 0; i < args_.length(); i++)
            hn = mozilla::AddToHash(hn, HashNumber(args_[i]));
        return hn;
    }
    bool operator==(const Sig& rhs) const {
        if (ret() != rhs.ret())
            return false;
        if (args().length() != rhs.args().length())
            return false;
        for (unsigned i = 0; i < args().length(); i++) {
            if (arg(i) != rhs.arg(i))
                return false;
        }
        return true;
    }
    bool operator!=(const Sig& rhs) const {
        return !(*this == rhs);
    }
};

// A "declared" signature is a Sig object that is created and owned by the
// ModuleGenerator. These signature objects are read-only and have the same
// lifetime as the ModuleGenerator. This type is useful since some uses of Sig
// need this extended lifetime and want to statically distinguish from the
// common stack-allocated Sig objects that get passed around.

struct DeclaredSig : Sig
{
    DeclaredSig() = default;
    DeclaredSig(DeclaredSig&& rhs) : Sig(Move(rhs)) {}
    explicit DeclaredSig(Sig&& sig) : Sig(Move(sig)) {}
    void operator=(Sig&& rhs) { Sig& base = *this; base = Move(rhs); }
};

typedef Vector<DeclaredSig, 0, SystemAllocPolicy> DeclaredSigVector;
typedef Vector<const DeclaredSig*, 0, SystemAllocPolicy> DeclaredSigPtrVector;

// The (,Profiling,Func)Offsets classes are used to record the offsets of
// different key points in a CodeRange during compilation.

struct Offsets
{
    MOZ_IMPLICIT Offsets(uint32_t begin = 0, uint32_t end = 0)
      : begin(begin), end(end)
    {}

    // These define a [begin, end) contiguous range of instructions compiled
    // into a CodeRange.
    uint32_t begin;
    uint32_t end;

    void offsetBy(uint32_t offset) {
        begin += offset;
        end += offset;
    }
};

struct ProfilingOffsets : Offsets
{
    MOZ_IMPLICIT ProfilingOffsets(uint32_t profilingReturn = 0)
      : Offsets(), profilingReturn(profilingReturn)
    {}

    // For CodeRanges with ProfilingOffsets, 'begin' is the offset of the
    // profiling entry.
    uint32_t profilingEntry() const { return begin; }

    // The profiling return is the offset of the return instruction, which
    // precedes the 'end' by a variable number of instructions due to
    // out-of-line codegen.
    uint32_t profilingReturn;

    void offsetBy(uint32_t offset) {
        Offsets::offsetBy(offset);
        profilingReturn += offset;
    }
};

struct FuncOffsets : ProfilingOffsets
{
    MOZ_IMPLICIT FuncOffsets(uint32_t nonProfilingEntry = 0,
                             uint32_t profilingJump = 0,
                             uint32_t profilingEpilogue = 0)
      : ProfilingOffsets(),
        nonProfilingEntry(nonProfilingEntry),
        profilingJump(profilingJump),
        profilingEpilogue(profilingEpilogue)
    {}

    // Function CodeRanges have an additional non-profiling entry that comes
    // after the profiling entry and a non-profiling epilogue that comes before
    // the profiling epilogue.
    uint32_t nonProfilingEntry;

    // When profiling is enabled, the 'nop' at offset 'profilingJump' is
    // overwritten to be a jump to 'profilingEpilogue'.
    uint32_t profilingJump;
    uint32_t profilingEpilogue;

    void offsetBy(uint32_t offset) {
        ProfilingOffsets::offsetBy(offset);
        nonProfilingEntry += offset;
        profilingJump += offset;
        profilingEpilogue += offset;
    }
};

// While the frame-pointer chain allows the stack to be unwound without
// metadata, Error.stack still needs to know the line/column of every call in
// the chain. A CallSiteDesc describes a single callsite to which CallSite adds
// the metadata necessary to walk up to the next frame. Lastly CallSiteAndTarget
// adds the function index of the callee.

class CallSiteDesc
{
    uint32_t line_;
    uint32_t column_ : 31;
    uint32_t kind_ : 1;
  public:
    enum Kind {
        Relative,  // pc-relative call
        Register   // call *register
    };
    CallSiteDesc() {}
    explicit CallSiteDesc(Kind kind)
      : line_(0), column_(0), kind_(kind)
    {}
    CallSiteDesc(uint32_t line, uint32_t column, Kind kind)
      : line_(line), column_(column), kind_(kind)
    {
        MOZ_ASSERT(column_ == column, "column must fit in 31 bits");
    }
    uint32_t line() const { return line_; }
    uint32_t column() const { return column_; }
    Kind kind() const { return Kind(kind_); }
};

class CallSite : public CallSiteDesc
{
    uint32_t returnAddressOffset_;
    uint32_t stackDepth_;

  public:
    CallSite() {}

    CallSite(CallSiteDesc desc, uint32_t returnAddressOffset, uint32_t stackDepth)
      : CallSiteDesc(desc),
        returnAddressOffset_(returnAddressOffset),
        stackDepth_(stackDepth)
    { }

    void setReturnAddressOffset(uint32_t r) { returnAddressOffset_ = r; }
    void offsetReturnAddressBy(int32_t o) { returnAddressOffset_ += o; }
    uint32_t returnAddressOffset() const { return returnAddressOffset_; }

    // The stackDepth measures the amount of stack space pushed since the
    // function was called. In particular, this includes the pushed return
    // address on all archs (whether or not the call instruction pushes the
    // return address (x86/x64) or the prologue does (ARM/MIPS)).
    uint32_t stackDepth() const { return stackDepth_; }
};

class CallSiteAndTarget : public CallSite
{
    uint32_t targetIndex_;

  public:
    CallSiteAndTarget(CallSite cs, uint32_t targetIndex)
      : CallSite(cs), targetIndex_(targetIndex)
    { }

    static const uint32_t NOT_INTERNAL = UINT32_MAX;

    bool isInternal() const { return targetIndex_ != NOT_INTERNAL; }
    uint32_t targetIndex() const { MOZ_ASSERT(isInternal()); return targetIndex_; }
};

typedef Vector<CallSite, 0, SystemAllocPolicy> CallSiteVector;
typedef Vector<CallSiteAndTarget, 0, SystemAllocPolicy> CallSiteAndTargetVector;

// Summarizes a heap access made by wasm code that needs to be patched later
// and/or looked up by the wasm signal handlers. Different architectures need
// to know different things (x64: offset and length, ARM: where to patch in
// heap length, x86: where to patch in heap length and base).

#if defined(JS_CODEGEN_X86)
class HeapAccess
{
    uint32_t insnOffset_;
    uint8_t opLength_;  // the length of the load/store instruction
    uint8_t cmpDelta_;  // the number of bytes from the cmp to the load/store instruction

  public:
    HeapAccess() = default;
    static const uint32_t NoLengthCheck = UINT32_MAX;

    // If 'cmp' equals 'insnOffset' or if it is not supplied then the
    // cmpDelta_ is zero indicating that there is no length to patch.
    HeapAccess(uint32_t insnOffset, uint32_t after, uint32_t cmp = NoLengthCheck) {
        mozilla::PodZero(this);  // zero padding for Valgrind
        insnOffset_ = insnOffset;
        opLength_ = after - insnOffset;
        cmpDelta_ = cmp == NoLengthCheck ? 0 : insnOffset - cmp;
    }

    uint32_t insnOffset() const { return insnOffset_; }
    void setInsnOffset(uint32_t insnOffset) { insnOffset_ = insnOffset; }
    void offsetInsnOffsetBy(uint32_t offset) { insnOffset_ += offset; }
    void* patchHeapPtrImmAt(uint8_t* code) const { return code + (insnOffset_ + opLength_); }
    bool hasLengthCheck() const { return cmpDelta_ > 0; }
    void* patchLengthAt(uint8_t* code) const {
        MOZ_ASSERT(hasLengthCheck());
        return code + (insnOffset_ - cmpDelta_);
    }
};
#elif defined(JS_CODEGEN_X64)
class HeapAccess
{
  public:
    enum WhatToDoOnOOB {
        CarryOn, // loads return undefined, stores do nothing.
        Throw    // throw a RangeError
    };

  private:
    uint32_t insnOffset_;
    uint8_t offsetWithinWholeSimdVector_; // if is this e.g. the Z of an XYZ
    bool throwOnOOB_;                     // should we throw on OOB?
    uint8_t cmpDelta_;                    // the number of bytes from the cmp to the load/store instruction

  public:
    HeapAccess() = default;
    static const uint32_t NoLengthCheck = UINT32_MAX;

    // If 'cmp' equals 'insnOffset' or if it is not supplied then the
    // cmpDelta_ is zero indicating that there is no length to patch.
    HeapAccess(uint32_t insnOffset, WhatToDoOnOOB oob,
               uint32_t cmp = NoLengthCheck,
               uint32_t offsetWithinWholeSimdVector = 0)
    {
        mozilla::PodZero(this);  // zero padding for Valgrind
        insnOffset_ = insnOffset;
        offsetWithinWholeSimdVector_ = offsetWithinWholeSimdVector;
        throwOnOOB_ = oob == Throw;
        cmpDelta_ = cmp == NoLengthCheck ? 0 : insnOffset - cmp;
        MOZ_ASSERT(offsetWithinWholeSimdVector_ == offsetWithinWholeSimdVector);
    }

    uint32_t insnOffset() const { return insnOffset_; }
    void setInsnOffset(uint32_t insnOffset) { insnOffset_ = insnOffset; }
    void offsetInsnOffsetBy(uint32_t offset) { insnOffset_ += offset; }
    bool throwOnOOB() const { return throwOnOOB_; }
    uint32_t offsetWithinWholeSimdVector() const { return offsetWithinWholeSimdVector_; }
    bool hasLengthCheck() const { return cmpDelta_ > 0; }
    void* patchLengthAt(uint8_t* code) const {
        MOZ_ASSERT(hasLengthCheck());
        return code + (insnOffset_ - cmpDelta_);
    }
};
#elif defined(JS_CODEGEN_ARM) || defined(JS_CODEGEN_ARM64) || \
      defined(JS_CODEGEN_MIPS32) || defined(JS_CODEGEN_MIPS64)
class HeapAccess
{
    uint32_t insnOffset_;
  public:
    HeapAccess() = default;
    explicit HeapAccess(uint32_t insnOffset) : insnOffset_(insnOffset) {}
    uint32_t insnOffset() const { return insnOffset_; }
    void setInsnOffset(uint32_t insnOffset) { insnOffset_ = insnOffset; }
    void offsetInsnOffsetBy(uint32_t offset) { insnOffset_ += offset; }
};
#elif defined(JS_CODEGEN_NONE)
class HeapAccess {
  public:
    void offsetInsnOffsetBy(uint32_t) { MOZ_CRASH(); }
    uint32_t insnOffset() const { MOZ_CRASH(); }
};
#endif

typedef Vector<HeapAccess, 0, SystemAllocPolicy> HeapAccessVector;

// A wasm::SymbolicAddress represents a pointer to a well-known function or
// object that is embedded in wasm code. Since wasm code is serialized and
// later deserialized into a different address space, symbolic addresses must be
// used for *all* pointers into the address space. The MacroAssembler records a
// list of all SymbolicAddresses and the offsets of their use in the code for
// later patching during static linking.

enum class SymbolicAddress
{
    ToInt32,
#if defined(JS_CODEGEN_ARM)
    aeabi_idivmod,
    aeabi_uidivmod,
    AtomicCmpXchg,
    AtomicXchg,
    AtomicFetchAdd,
    AtomicFetchSub,
    AtomicFetchAnd,
    AtomicFetchOr,
    AtomicFetchXor,
#endif
    ModD,
    SinD,
    CosD,
    TanD,
    ASinD,
    ACosD,
    ATanD,
    CeilD,
    CeilF,
    FloorD,
    FloorF,
    ExpD,
    LogD,
    PowD,
    ATan2D,
    Runtime,
    RuntimeInterruptUint32,
    StackLimit,
    ReportOverRecursed,
    OnOutOfBounds,
    OnImpreciseConversion,
    HandleExecutionInterrupt,
    InvokeImport_Void,
    InvokeImport_I32,
    InvokeImport_F64,
    CoerceInPlace_ToInt32,
    CoerceInPlace_ToNumber,
    Limit
};

void*
AddressOf(SymbolicAddress imm, ExclusiveContext* cx);

// The CompileArgs struct captures global parameters that affect all wasm code
// generation. It also currently is the single source of truth for whether or
// not to use signal handlers for different purposes.

struct CompileArgs
{
    bool useSignalHandlersForOOB;
    bool useSignalHandlersForInterrupt;

    CompileArgs() = default;
    explicit CompileArgs(ExclusiveContext* cx);
    bool operator==(CompileArgs rhs) const;
    bool operator!=(CompileArgs rhs) const { return !(*this == rhs); }
};

// Constants:

static const unsigned ActivationGlobalDataOffset = 0;
static const unsigned HeapGlobalDataOffset = ActivationGlobalDataOffset + sizeof(void*);
static const unsigned NaN64GlobalDataOffset = HeapGlobalDataOffset + sizeof(void*);
static const unsigned NaN32GlobalDataOffset = NaN64GlobalDataOffset + sizeof(double);
static const unsigned InitialGlobalDataBytes = NaN32GlobalDataOffset + sizeof(float);

} // namespace wasm
} // namespace js

#endif // wasm_types_h
