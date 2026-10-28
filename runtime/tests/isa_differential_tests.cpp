// Differential oracle for the floating-point / paired-single ISA helpers.
//
// Runs every helper over a deterministic stream of edge-biased inputs (signed zeros, single and
// double denormals, values around FLT_MIN, infinities, quiet and signalling NaNs with payloads)
// with FPSCR[NI] both clear and set, and prints one FNV-1a digest per helper. Native x86-64,
// AArch64 and wasm builds must print identical digests; runtime/web/isa-differential.sh diffs a
// native build against a wasm32 build run under Node.
//
// Lanes of commutative operations (add, mul, the fused forms) with two or more NaN inputs are
// excluded: even native x86 propagates whichever NaN ends up as the first SSE source after clang
// freely commutes the operands (or picks an FMA form) at that call site - see the resolver comment
// in ppc_isa_float.h - so those lanes have no single right answer to compare.
//
// With FPSCR[NI] set, x86 and AArch64 model NI through the host's global flush-to-zero / denormals-
// are-zero mode, which also changes every scalar helper built from plain C++ arithmetic,
// conversions and compares. Wasm has no such mode; its pair primitives emulate it exactly, but the
// scalar-built helpers below only match on non-denormal operands. Their NI-set digests are tagged
// [host-daz] and excluded from the cross-host comparison (they are still compared with NI clear).
//
// Out-of-range and NaN float->int casts are undefined in C++, but runtime code and translated
// code built from it inherit whatever the host does, and x86 yields INT_MIN. Wasm matches that
// only without the saturating conversions (-mno-nontrapping-fptoint), which newer LLVM enables
// by default; the cast_to_int digest catches a build that loses the flag. AArch64 saturates like
// those conversions do, so that digest is tagged [host-cast] and only compared against x86.
//
// Usage: isa_differential_tests [iterations [trace-op]]

#include "isa/ppc_isa_float.h"
#include "isa/ppc_isa_fpenv.h"
#include "ppc_isa_quantized.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct Rng {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    uint64_t Next()
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};

uint32_t EdgeFloatBits(Rng& rng)
{
    static constexpr uint32_t kSpecial[] = {
        0x00000000u, 0x80000000u, 0x00000001u, 0x807FFFFFu, 0x00400000u, 0x00800000u,
        0x80800000u, 0x00800001u, 0x007FFFFFu, 0x3F800000u, 0xBF800000u, 0x7F7FFFFFu,
        0xFF7FFFFFu, 0x7F800000u, 0xFF800000u, 0x7FC00000u, 0xFFC00000u, 0x7F800001u,
        0xFFA5A5A5u, 0x7FC12345u, 0x4B800000u, 0x33800000u, 0x0C000000u, 0x34000000u,
    };
    const uint64_t pick = rng.Next();
    switch (pick & 7u) {
    case 0:
    case 1:
        return kSpecial[(pick >> 8) % (sizeof(kSpecial) / sizeof(kSpecial[0]))];
    case 2: {
        // Exponent near the single denormal boundary, random mantissa.
        const uint32_t exponent = static_cast<uint32_t>((pick >> 8) % 4u);
        return (static_cast<uint32_t>(pick >> 32) & 0x807FFFFFu) | (exponent << 23);
    }
    case 3: {
        // Small exponents, so products and sums land around FLT_MIN.
        const uint32_t exponent = 32u + static_cast<uint32_t>((pick >> 8) % 64u);
        return (static_cast<uint32_t>(pick >> 32) & 0x807FFFFFu) | (exponent << 23);
    }
    default: {
        // Ordinary magnitudes.
        const uint32_t exponent = 100u + static_cast<uint32_t>((pick >> 8) % 56u);
        return (static_cast<uint32_t>(pick >> 32) & 0x807FFFFFu) | (exponent << 23);
    }
    }
}

uint64_t EdgeDoubleBits(Rng& rng)
{
    const uint64_t pick = rng.Next();
    switch (pick & 7u) {
    case 0: {
        // Exact float32 value widened, the common case for paired-origin data.
        float f;
        const uint32_t bits = EdgeFloatBits(rng);
        std::memcpy(&f, &bits, sizeof(f));
        if (std::isnan(f)) return 0x7FF8000000000000ull | (pick & 0x8000000000000000ull);
        return PpcBitCastToU64Inline(static_cast<double>(f));
    }
    case 1: return (rng.Next() & 0x800FFFFFFFFFFFFFull);  // double denormal
    case 2: return 0x7FF0000000000001ull | (rng.Next() & 0x800FFFFFFFFFFFFFull);  // NaN family
    case 3: {
        // Around the single denormal range: exponent 874..905 (see ConvertPpcDoubleToSingleBits).
        const uint64_t exponent = 870u + (pick >> 8) % 40u;
        return (rng.Next() & 0x800FFFFFFFFFFFFFull) | (exponent << 52);
    }
    default: {
        const uint64_t exponent = 1000u + (pick >> 8) % 60u;
        return (rng.Next() & 0x800FFFFFFFFFFFFFull) | (exponent << 52);
    }
    }
}

double EdgePair(Rng& rng)
{
    return PpcPackPairedBitsInline(EdgeFloatBits(rng), EdgeFloatBits(rng));
}

// When set (second argument), every value fed to that digest is printed with its inputs' index,
// so a native and a wasm trace can be diffed to find the first divergent input.
const char* g_traceOp = nullptr;
long g_iteration = 0;

struct Digest {
    const char* name;
    uint64_t hash = 0xCBF29CE484222325ull;
    uint64_t count = 0;
    void Add(uint64_t value)
    {
        if (g_traceOp && std::strcmp(g_traceOp, name) == 0)
            std::printf("%ld %016llx\n", g_iteration, static_cast<unsigned long long>(value));
        for (int i = 0; i < 8; ++i) {
            hash ^= (value >> (i * 8)) & 0xFFu;
            hash *= 0x100000001B3ull;
        }
        ++count;
    }
    void AddDouble(double value) { Add(PpcBitCastToU64Inline(value)); }
};

bool IsNanBits(uint32_t bits) { return (bits & 0x7FFFFFFFu) > 0x7F800000u; }

int NanCount(double a, double c, double b, int lane)
{
    const int shift = lane == 0 ? 32 : 0;
    const auto bits = [&](double v) { return static_cast<uint32_t>(PpcBitCastToU64Inline(v) >> shift); };
    return int(IsNanBits(bits(a))) + int(IsNanBits(bits(c))) + int(IsNanBits(bits(b)));
}

// Hash only the lanes whose answer is architecturally unique (see header comment). Pass b = 0.0
// for two-operand operations.
void AddCommutativePair(Digest& digest, double result, double a, double c, double b)
{
    const uint64_t bits = PpcBitCastToU64Inline(result);
    const uint64_t keepPs0 = NanCount(a, c, b, 0) < 2 ? 0xFFFFFFFF00000000ull : 0;
    const uint64_t keepPs1 = NanCount(a, c, b, 1) < 2 ? 0x00000000FFFFFFFFull : 0;
    digest.Add(bits & (keepPs0 | keepPs1));
}

enum Op : int {
    kPsAdd, kPsSub, kPsMul, kPsDiv, kPsMadd, kPsMsub, kPsNmadd, kPsNmsub, kPsMadds0,
    kPsMadds1, kPsMuls0, kPsMuls1, kPsSum0, kPsSum1, kPsMerge, kPsSel, kPsNegAbs, kPsRes,
    kPsFromScalar, kForceSingle, kFmadds, kFmuls, kFres, kFrsqrte, kFctiwz, kDoubleToSingle,
    kPsqFloatBits, kPsqQuantize, kPsqDequantize, kPsqU8Scale61, kPsqHostRoundTrip, kCastToInt, kOpCount
};

// Helpers whose NI-set behavior comes from the host's global DAZ/FTZ (see header comment).
bool UsesHostDaz(int op)
{
    switch (op) {
    case kPsSum0: case kPsSum1: case kPsSel: case kPsRes: case kPsFromScalar: case kFmadds:
    case kFmuls: case kFres: case kFrsqrte:
        return true;
    default:
        return false;
    }
}

const char* const kOpNames[kOpCount] = {
    "ps_add", "ps_sub", "ps_mul", "ps_div", "ps_madd", "ps_msub", "ps_nmadd", "ps_nmsub",
    "ps_madds0", "ps_madds1", "ps_muls0", "ps_muls1", "ps_sum0", "ps_sum1", "ps_merge",
    "ps_sel", "ps_neg_abs", "ps_res", "ps_from_scalar", "force_single", "fmadds_family",
    "fmuls", "fres", "frsqrte", "fctiwz", "double_to_single", "psq_float_bits",
    "psq_quantize", "psq_dequantize", "psq_u8_scale61", "psq_host_round_trip", "cast_to_int",
};

void RunOnce(Digest* digests, Rng& rng)
{
    const double a = EdgePair(rng);
    const double b = EdgePair(rng);
    const double c = EdgePair(rng);

    AddCommutativePair(digests[kPsAdd], PPC_PsAddInline(a, b), a, b, 0.0);
    digests[kPsSub].AddDouble(PPC_PsSubInline(a, b));
    AddCommutativePair(digests[kPsMul], PPC_PsMulInline(a, c), a, c, 0.0);
    digests[kPsDiv].AddDouble(PPC_PsDivInline(a, b));
    AddCommutativePair(digests[kPsMadd], PPC_PsMaddInline(a, c, b), a, c, b);
    AddCommutativePair(digests[kPsMsub], PPC_PsMsubInline(a, c, b), a, c, b);
    AddCommutativePair(digests[kPsNmadd], PPC_PsNmaddInline(a, c, b), a, c, b);
    AddCommutativePair(digests[kPsNmsub], PPC_PsNmsubInline(a, c, b), a, c, b);
    const double c0 = PpcPackPairedInline(PpcGetPs0Inline(c), PpcGetPs0Inline(c));
    const double c1 = PpcPackPairedInline(PpcGetPs1Inline(c), PpcGetPs1Inline(c));
    AddCommutativePair(digests[kPsMadds0], PPC_PsMadds0Inline(a, c, b), a, c0, b);
    AddCommutativePair(digests[kPsMadds1], PPC_PsMadds1Inline(a, c, b), a, c1, b);
    AddCommutativePair(digests[kPsMuls0], PPC_PsMuls0Inline(a, c), a, c0, 0.0);
    AddCommutativePair(digests[kPsMuls1], PPC_PsMuls1Inline(a, c), a, c1, 0.0);
    // ps_sum0's ps0 (and ps_sum1's ps1) is the commutative sum a.ps0 + b.ps1.
    const bool sumAmbiguous = std::isnan(PpcGetPs0Inline(a)) && std::isnan(PpcGetPs1Inline(b));
    const uint64_t sum0 = PpcBitCastToU64Inline(PPC_PsSum0Inline(a, b, c));
    const uint64_t sum1 = PpcBitCastToU64Inline(PPC_PsSum1Inline(a, b, c));
    digests[kPsSum0].Add(sumAmbiguous ? sum0 & 0x00000000FFFFFFFFull : sum0);
    digests[kPsSum1].Add(sumAmbiguous ? sum1 & 0xFFFFFFFF00000000ull : sum1);
    digests[kPsMerge].AddDouble(PPC_PsMerge00Inline(a, b));
    digests[kPsMerge].AddDouble(PPC_PsMerge01Inline(a, b));
    digests[kPsMerge].AddDouble(PPC_PsMerge10Inline(a, b));
    digests[kPsMerge].AddDouble(PPC_PsMerge11Inline(a, b));
    digests[kPsSel].AddDouble(PPC_PsSelInline(a, c, b));
    digests[kPsNegAbs].AddDouble(PPC_PsNegInline(a));
    digests[kPsNegAbs].AddDouble(PPC_PsAbsInline(a));
    digests[kPsRes].AddDouble(PPC_PsRes(a));

    const double d = PpcBitCastToDoubleInline(EdgeDoubleBits(rng));
    const double e = PpcBitCastToDoubleInline(EdgeDoubleBits(rng));
    const double f = PpcBitCastToDoubleInline(EdgeDoubleBits(rng));
    digests[kPsFromScalar].AddDouble(PPC_PsFromScalarInline(d));
    digests[kForceSingle].Add(PpcBitCastToU32Inline(PpcForceSingleValueInline(d)));
    // Scalar fused singles go through std::fma on doubles, whose NaN choice is libm's, so
    // restrict them to non-NaN operands; NaN propagation is covered by the pair forms above.
    if (!std::isnan(d) && !std::isnan(e) && !std::isnan(f)) {
        digests[kFmadds].AddDouble(PPC_Fmadds(d, e, f));
        digests[kFmadds].AddDouble(PPC_Fmsubs(d, e, f));
        digests[kFmadds].AddDouble(PPC_Fnmadds(d, e, f));
        digests[kFmadds].AddDouble(PPC_Fnmsubs(d, e, f));
        digests[kFmuls].AddDouble(PpcFmulsInline(d, e));
    }
    digests[kFres].AddDouble(PPC_Fres(a));
    digests[kFrsqrte].AddDouble(PPC_Frsqrte(d));
    digests[kFctiwz].AddDouble(PPC_Fctiwz(d));
    digests[kDoubleToSingle].Add(PpcConvertToSingleFTZInline(PpcBitCastToU64Inline(d)));
    digests[kDoubleToSingle].Add(PpcConvertToDoubleBitsInline(PpcBitCastToU32Inline(PpcGetPs1Inline(a))));

    const uint64_t pairBits = PpcBitCastToU64Inline(a);
    digests[kPsqFloatBits].Add(PpcLoadPairPsqFloatBitsPackedInline(pairBits));
    digests[kPsqFloatBits].Add(PpcStorePairPsqFloatBitsPackedInline(pairBits));
    const uint32_t scale = static_cast<uint32_t>(rng.Next() % 64u);
    const float lane = PpcGetPs0Inline(a);
    // Out-of-range and NaN float->int conversion is host-defined in C++; the guest-visible
    // contract only covers finite values the clamp keeps in range.
    if (!std::isnan(lane)) {
        digests[kPsqQuantize].Add(static_cast<uint8_t>(PpcScaleAndClampPsqInline<int8_t>(lane, scale)));
        digests[kPsqQuantize].Add(static_cast<uint16_t>(PpcScaleAndClampPsqInline<int16_t>(lane, scale)));
    }
    const uint64_t raw = rng.Next();
    digests[kPsqDequantize].Add(PpcBitCastToU32Inline(
        PpcDequantizePsqInline<int16_t>(static_cast<int16_t>(raw), scale)));
    digests[kPsqDequantize].Add(PpcBitCastToU32Inline(
        PpcDequantizePsqInline<uint8_t>(static_cast<uint8_t>(raw >> 16), scale)));
    digests[kPsqU8Scale61].Add(PpcQuantizePairPsqU8Scale61PackedInline(a));
    uint8_t host[8];
    PpcStorePairPsqFloatToHostInline(host, a);
    uint64_t hostBits = 0;
    std::memcpy(&hostBits, host, sizeof(hostBits));
    digests[kPsqHostRoundTrip].Add(hostBits);
    digests[kPsqHostRoundTrip].AddDouble(PpcLoadPairPsqFloatFromHostInline(host));

    // Volatile so the casts happen at run time with the host's conversion instruction.
    volatile float castSingle = lane;
    volatile double castDouble = d;
    digests[kCastToInt].Add(static_cast<uint32_t>(static_cast<int32_t>(castSingle)));
    digests[kCastToInt].Add(static_cast<uint32_t>(static_cast<int32_t>(castDouble)));
}

}  // namespace

int main(int argc, char** argv)
{
    const long iterations = argc > 1 ? std::strtol(argv[1], nullptr, 10) : 200000;
    g_traceOp = argc > 2 ? argv[2] : nullptr;
    for (uint32_t fpscr : {0u, 4u}) {
        MkwApplyHostNiMode(fpscr);
        Digest digests[kOpCount];
        for (int i = 0; i < kOpCount; ++i) digests[i].name = kOpNames[i];
        Rng rng;
        for (g_iteration = 0; g_iteration < iterations; ++g_iteration) RunOnce(digests, rng);
        for (int i = 0; i < kOpCount; ++i) {
            const Digest& digest = digests[i];
            std::printf("ni=%u %-20s %016llx %llu%s\n", fpscr == 4u ? 1u : 0u, digest.name,
                        static_cast<unsigned long long>(digest.hash),
                        static_cast<unsigned long long>(digest.count),
                        fpscr == 4u && UsesHostDaz(i) ? " [host-daz]"
                            : i == kCastToInt ? " [host-cast]" : "");
        }
    }
    MkwApplyHostNiMode(0);
    return 0;
}
