#include "abstract_layout.h"

#include <cstdlib>
#include <iostream>
#include <vector>

using palimpsest::detail::AbstractLayoutNode;
using palimpsest::detail::resolve_abstract_layout;

namespace {

void expect(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "abstract_layout_tests: " << message << '\n';
        std::exit(1);
    }
}

}  // namespace

int main() {
    {
        std::vector<AbstractLayoutNode> nodes = {
            {{80, 55, 440, 28}, 0, 18, 7, true, false, false, false, false, false},
            {{150, 100, 300, 12}, 0, 10, 8, false, true, false, false, false, false},
            {{55, 150, 70, 10}, 0, 10, 1, false, false, false, false, true, true},
            {{55, 164, 250, 10}, 0, 10, 13, false, false, false, false, false, false},
            {{55, 176, 250, 10}, 0, 10, 12, false, false, false, false, false, false},
            {{55, 188, 250, 10}, 0, 10, 12, false, false, false, false, false, false},
            {{55, 220, 100, 10}, 0, 11, 2, false, false, false, true, false, false},
        };
        const auto d = resolve_abstract_layout(nodes, 10.0, 600.0, 800.0);
        expect(d.abstract_indices.size() == 3, "label-only abstract heading should attach following prose");
        expect(!d.inferred_without_label, "explicit hint should not be marked inferred");
    }

    {
        std::vector<AbstractLayoutNode> nodes = {
            {{70, 50, 460, 30}, 0, 18, 8, true, false, false, false, false, false},
            {{140, 100, 320, 12}, 0, 10, 9, false, true, false, false, false, false},
            {{120, 120, 360, 10}, 0, 9, 8, false, false, true, false, false, false},
            {{55, 160, 250, 10}, 0, 10, 13, false, false, false, false, false, false},
            {{55, 172, 250, 10}, 0, 10, 12, false, false, false, false, false, false},
            {{55, 184, 250, 10}, 0, 10, 11, false, false, false, false, false, false},
            {{55, 196, 240, 10}, 0, 10, 10, false, false, false, false, false, false},
            {{130, 230, 120, 12}, 0, 12, 2, false, false, false, false, true, false},
        };
        const auto d = resolve_abstract_layout(nodes, 10.0, 600.0, 800.0);
        expect(d.inferred_without_label, "abstract should be inferred without an Abstract label");
        expect(d.abstract_indices.size() == 4, "structural front-matter prose run should be selected");
    }

    {
        std::vector<AbstractLayoutNode> nodes = {
            {{70, 50, 460, 30}, 0, 18, 8, true, false, false, false, false, false},
            {{140, 100, 320, 12}, 0, 10, 9, false, true, false, false, false, false},
            {{90, 120, 420, 10}, 0, 9, 12, false, false, true, false, false, false},
            {{80, 134, 440, 10}, 0, 9, 11, false, false, true, false, false, false},
            {{120, 170, 130, 12}, 0, 12, 2, false, false, false, false, true, false},
        };
        const auto d = resolve_abstract_layout(nodes, 10.0, 600.0, 800.0);
        expect(d.abstract_indices.empty(), "author and affiliation material must not be inferred as abstract");
    }

    std::cout << "abstract_layout_tests: ok\n";
    return 0;
}
