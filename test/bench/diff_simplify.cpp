// What Simplify() makes of every expression in the given dataset files, one
// line each, so that two builds - or one build under two settings - can be
// compared by diffing their output. All files run in one process, so caches
// that outlive a single call (the subtree memo in PatternMatcher.cpp) carry
// from one expression to the next as they do inside a real host.
//
// Usage: diff_simplify [--max-vars N] [--semilinear] file...
//        COBRA_PATTERN_MEMO=0 diff_simplify ... > off.txt; diff_simplify ... > on.txt

#include "ExprParser.h"
#include "cobra/core/Expr.h"
#include "cobra/core/Simplifier.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace cobra;

int main(int argc, char **argv) {
    uint32_t max_vars = 16;
    auto families     = TechniqueFamily::kAll;
    std::vector< std::string > files;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--max-vars") == 0 && i + 1 < argc) {
            max_vars = static_cast< uint32_t >(std::stoul(argv[++i]));
        } else if (std::strcmp(argv[i], "--semilinear") == 0) {
            families = TechniqueFamily::kSemilinear;
        } else {
            files.emplace_back(argv[i]);
        }
    }

    for (const auto &path : files) {
        std::ifstream in(path);
        std::string line;
        uint32_t lineno = 0;
        while (std::getline(in, line)) {
            ++lineno;
            if (line.empty() || line[0] == '#') { continue; }
            auto sep = line.find(',');
            if (sep == std::string::npos) { sep = line.find('\t'); }
            const std::string raw = sep != std::string::npos ? line.substr(0, sep) : line;

            std::cout << path << ':' << lineno << ' ';
            auto parsed = ParseAndEvaluate(raw, 64);
            if (!parsed.has_value()) {
                std::cout << "unparsed\n";
                continue;
            }
            auto ast               = ParseToAst(raw, 64);
            const Expr *input_expr = ast.has_value() ? ast->expr.get() : nullptr;
            Options opts{ .bitwidth         = 64,
                          .max_vars         = max_vars,
                          .spot_check       = true,
                          .enabled_families = families };
            auto result = Simplify(parsed->sig, parsed->vars, input_expr, opts);
            if (!result.has_value()) {
                std::cout << "error " << result.error().message << '\n';
                continue;
            }
            if (result->kind != SimplifyOutcome::Kind::kSimplified || !result->expr) {
                std::cout << "unsimplified\n";
                continue;
            }
            const auto &names = result->real_vars.empty() ? parsed->vars : result->real_vars;
            std::cout << "simplified " << Render(*result->expr, names, 64) << '\n';
        }
    }
    return 0;
}
