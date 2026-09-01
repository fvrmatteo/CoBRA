#pragma once

#include "Orchestrator.h"
#include "cobra/core/Result.h"

#include <vector>

namespace cobra {

    // ---------------------------------------------------------------
    // PassId — identifies each pass in the registry
    // ---------------------------------------------------------------

    enum class PassId : uint8_t {
        kLowerNotOverArith,
        kClassifyAst,
        kBuildSignatureState,
        // Semilinear passes
        kSemilinearNormalize,
        kSemilinearCheck,
        kSemilinearRewrite,
        kSemilinearReconstruct,
        // Decomposition extractors
        kExtractProductCore,
        kExtractPolyCoreD2,
        kExtractTemplateCore,
        kExtractPolyCoreD3,
        kExtractPolyCoreD4,
        // Remainder prep
        kPrepareDirectRemainder,
        kPrepareRemainderFromCore,
        // Decomposition residual solvers
        kResidualSupported,
        kResidualPolyRecovery,
        kResidualGhost,
        kResidualFactoredGhost,
        kResidualFactoredGhostEscalated,
        kResidualTemplate,
        // Competition resolution
        kResolveCompetition,
        // Signature technique passes
        kSignaturePatternMatch,
        kSignatureAnf,
        kPrepareCoeffModel,
        kSignatureCobCandidate,
        kSignatureSingletonPolyRecovery,
        kSignatureMultivarPolyRecovery,
        kSignatureBitwiseDecompose,
        kSignatureHybridDecompose,
        // Structural rewrites
        kOperandSimplify,
        kProductIdentityCollapse,
        kXorLowering,
        kVerifyCandidate,
        // Lifting passes
        kLiftArithmeticAtoms,
        kLiftRepeatedSubexpressions,
        kPrepareLiftedOuterSolve,
        // Sentinel — must remain last.
        kCount_,
    };

    static_assert(
        static_cast< uint8_t >(PassId::kCount_) <= 64,
        "PassId count exceeds attempted_mask width (uint64_t)"
    );

    inline bool IsDecompositionFamilyPass(PassId id) {
        return id >= PassId::kExtractProductCore && id <= PassId::kResidualTemplate;
    }

    // Passes that belong to no family are always scheduled: they are the
    // classification, signature-building and verification steps every route
    // depends on, not techniques in their own right.
    inline uint64_t DisabledPassMask(TechniqueFamily enabled) {
        const auto kBit = [](PassId id) { return uint64_t{ 1 } << static_cast< uint8_t >(id); };

        uint64_t mask = 0;
        if (!HasFamily(enabled, TechniqueFamily::kSemilinear)) {
            mask |= kBit(PassId::kSemilinearNormalize) | kBit(PassId::kSemilinearCheck)
                | kBit(PassId::kSemilinearRewrite) | kBit(PassId::kSemilinearReconstruct);
        }
        if (!HasFamily(enabled, TechniqueFamily::kPolynomial)) {
            mask |= kBit(PassId::kExtractPolyCoreD2) | kBit(PassId::kExtractPolyCoreD3)
                | kBit(PassId::kExtractPolyCoreD4) | kBit(PassId::kResidualPolyRecovery)
                | kBit(PassId::kSignatureSingletonPolyRecovery)
                | kBit(PassId::kSignatureMultivarPolyRecovery);
        }
        if (!HasFamily(enabled, TechniqueFamily::kTemplate)) {
            mask |= kBit(PassId::kExtractTemplateCore) | kBit(PassId::kResidualTemplate);
        }
        if (!HasFamily(enabled, TechniqueFamily::kBitwise)) {
            mask |= kBit(PassId::kSignatureBitwiseDecompose)
                | kBit(PassId::kSignatureHybridDecompose);
        }
        if (!HasFamily(enabled, TechniqueFamily::kGhost)) {
            mask |= kBit(PassId::kResidualGhost) | kBit(PassId::kResidualFactoredGhost)
                | kBit(PassId::kResidualFactoredGhostEscalated);
        }
        if (!HasFamily(enabled, TechniqueFamily::kLifting)) {
            mask |= kBit(PassId::kLiftArithmeticAtoms)
                | kBit(PassId::kLiftRepeatedSubexpressions)
                | kBit(PassId::kPrepareLiftedOuterSolve);
        }
        return mask;
    }

    inline ExtractorKind ProjectExtractorKind(RemainderOrigin origin) {
        switch (origin) {
            case RemainderOrigin::kDirectBooleanNull:
                return ExtractorKind::kBooleanNullDirect;
            case RemainderOrigin::kProductCore:
                return ExtractorKind::kProductAST;
            case RemainderOrigin::kPolynomialCore:
                return ExtractorKind::kPolynomial;
            case RemainderOrigin::kTemplateCore:
                return ExtractorKind::kTemplate;
            case RemainderOrigin::kSignatureLowering:
                return ExtractorKind::kBooleanNullDirect;
            case RemainderOrigin::kLiftedOuter:
                return ExtractorKind::kBooleanNullDirect;
        }
        return ExtractorKind::kBooleanNullDirect;
    }

    // ---------------------------------------------------------------
    // PassTag — broad category for each pass
    // ---------------------------------------------------------------

    enum class PassTag {
        kAnalysis,
        kRewrite,
        kSolver,
        kVerifier,
    };

    // ---------------------------------------------------------------
    // Function pointer types for pass dispatch
    // ---------------------------------------------------------------

    using ApplicabilityFn = bool (*)(const WorkItem &, const OrchestratorContext &);
    using PassFn          = Result< PassResult > (*)(const WorkItem &, OrchestratorContext &);

    // ---------------------------------------------------------------
    // PassDescriptor — static metadata for one pass
    // ---------------------------------------------------------------

    struct PassDescriptor
    {
        PassId id;
        StateKind consumes;
        PassTag tag;
        ApplicabilityFn applicable;
        PassFn run;
    };

    // ---------------------------------------------------------------
    // Registry
    // ---------------------------------------------------------------

    const std::vector< PassDescriptor > &GetPassRegistry();

    // ---------------------------------------------------------------
    // Pass adapter declarations
    // ---------------------------------------------------------------

    Result< PassResult > RunLowerNotOverArith(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunClassifyAst(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunBuildSignatureState(const WorkItem &, OrchestratorContext &);

    Result< PassResult > RunOperandSimplify(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunProductIdentityCollapse(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunXorLowering(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunVerifyCandidate(const WorkItem &, OrchestratorContext &);
    Result< PassResult > RunPrepareLiftedOuterSolve(const WorkItem &, OrchestratorContext &);

    // ---------------------------------------------------------------
    // ActiveAst helpers — resolve vars/evaluator/sig from solve_ctx
    // ---------------------------------------------------------------

    const std::vector< std::string > &
    ActiveAstVars(const WorkItem &item, const OrchestratorContext &ctx);

    const std::optional< Evaluator > &
    ActiveAstEvaluator(const WorkItem &item, const OrchestratorContext &ctx);

    const std::vector< uint64_t > *
    ActiveAstInputSig(const WorkItem &item, const OrchestratorContext &ctx);

} // namespace cobra
