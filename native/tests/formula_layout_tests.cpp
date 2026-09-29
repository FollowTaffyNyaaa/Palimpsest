#include "formula_layout.h"

#include <cstdlib>
#include <iostream>
#include <vector>

using palimpsest::detail::FormulaLayoutKind;
using palimpsest::detail::FormulaLayoutNode;
using palimpsest::detail::resolve_formula_layout;

namespace {

void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "formula_layout_tests: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    {
        std::vector<FormulaLayoutNode> nodes = {
            {{148.0, 100.0, 8.0, 8.0}, 0, false, false},
            {{50.0, 99.0, 94.0, 10.0}, 0, true, false},
            {{160.0, 99.0, 110.0, 10.0}, 0, true, false},
        };
        const auto decision = resolve_formula_layout(nodes, 0, 10.0, 600.0);
        expect(decision.kind == FormulaLayoutKind::Inline, "inline symbol should stay inline");
        expect(decision.has_left_text && decision.has_right_text, "inline symbol should see prose on both sides");
    }

    {
        std::vector<FormulaLayoutNode> nodes = {
            {{145.0, 92.0, 28.0, 26.0}, 0, false, false},
            {{50.0, 100.0, 90.0, 10.0}, 0, true, false},
            {{178.0, 100.0, 110.0, 10.0}, 0, true, false},
        };
        const auto decision = resolve_formula_layout(nodes, 0, 10.0, 600.0);
        expect(decision.kind == FormulaLayoutKind::Inline, "tall fraction/sum in a prose row should stay inline");
    }

    {
        std::vector<FormulaLayoutNode> nodes = {
            {{250.0, 150.0, 42.0, 10.0}, 0, false, false},
            {{50.0, 126.0, 240.0, 10.0}, 0, true, false},
            {{50.0, 178.0, 240.0, 10.0}, 0, true, false},
        };
        const auto decision = resolve_formula_layout(nodes, 0, 10.0, 600.0);
        expect(decision.kind == FormulaLayoutKind::Display, "short own-line equation should be display");
    }

    {
        std::vector<FormulaLayoutNode> nodes = {
            {{170.0, 220.0, 190.0, 13.0}, 0, false, false},
            {{510.0, 221.0, 18.0, 10.0}, 0, false, true},
            {{50.0, 198.0, 250.0, 10.0}, 0, true, false},
            {{50.0, 246.0, 250.0, 10.0}, 0, true, false},
        };
        const auto decision = resolve_formula_layout(nodes, 0, 10.0, 600.0);
        expect(decision.kind == FormulaLayoutKind::Display, "numbered equation should be display");
        expect(decision.has_equation_number, "equation number cue should be detected");
    }

    {
        std::vector<FormulaLayoutNode> nodes = {
            {{150.0, 107.5, 30.0, 10.0}, 0, false, false},
            {{50.0, 100.0, 95.0, 10.0}, 0, true, false},
        };
        const auto decision = resolve_formula_layout(nodes, 0, 10.0, 600.0);
        expect(decision.kind == FormulaLayoutKind::Uncertain, "weak partial overlap should stay conservative");
    }

    std::cout << "formula_layout_tests: ok\n";
    return 0;
}
