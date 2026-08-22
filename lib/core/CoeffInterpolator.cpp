#include "cobra/core/CoeffInterpolator.h"
#include "cobra/core/BitWidth.h"
#include "cobra/core/Trace.h"
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cobra {

    std::vector< uint64_t > InterpolateCoefficients(
        std::vector< uint64_t > sig, uint32_t num_vars, uint32_t bitwidth
    ) { // NOLINT(readability-identifier-naming)
        // The butterfly stride is `1U << var` (uint32) for var in [0, num_vars).
        // For num_vars >= 32 the shift would be UB. Callers gate on
        // ctx.opts.max_vars (default 16); the assert protects against
        // any caller that bypasses that gate (RunPrepareCoeffModel adds
        // an explicit policy check at its boundary, mirroring RunSignatureAnf).
        assert(num_vars < 32 && "InterpolateCoefficients: 1U << var UB for num_vars >= 32");
        const uint64_t mask = Bitmask(bitwidth);
        const size_t len    = sig.size();
        COBRA_TRACE(
            "CoeffInterp", "InterpolateCoefficients: vars={} bitwidth={} sig_len={}", num_vars,
            bitwidth, len
        );

        // In-place butterfly interpolation: for each variable, subtract the
        // "without this variable" entry from the "with" entry to isolate its
        // contribution. Equivalent to evaluating directly in the (1, x, y, x&y)
        // basis rather than computing a change-of-basis matrix.
        //
        // Canonical block butterfly: each var pairs index i (bit clear) with
        // i + half (bit set), so we touch exactly len/2 elements per var with
        // no per-element bit test and contiguous access within each half-block.
        for (uint32_t var = 0; var < num_vars; ++var) {
            const size_t half  = size_t{ 1 } << var;
            const size_t block = half << 1;
            for (size_t base = 0; base < len; base += block) {
                for (size_t i = base; i < base + half; ++i) {
                    sig[i + half] = (sig[i + half] - sig[i]) & mask;
                }
            }
        }
        COBRA_TRACE_SIG("CoeffInterp", "output coeffs", sig);
        return sig;
    }

} // namespace cobra
