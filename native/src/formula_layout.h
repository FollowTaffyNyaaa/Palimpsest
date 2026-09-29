#pragma once

#include <palimpsest/paper_ir.h>

#include <cstddef>
#include <limits>
#include <span>

namespace palimpsest::detail {

enum class FormulaLayoutKind {
    Inline,
    Display,
    Uncertain,
};

inline constexpr std::size_t kNoFormulaAnchor = std::numeric_limits<std::size_t>::max();

struct FormulaLayoutNode {
    Rect bbox;
    int column{-1};
    bool prose{false};
    bool equation_number{false};
};

struct FormulaLayoutDecision {
    FormulaLayoutKind kind{FormulaLayoutKind::Uncertain};
    std::size_t anchor_index{kNoFormulaAnchor};
    double text_overlap{};
    bool has_left_text{false};
    bool has_right_text{false};
    bool has_equation_number{false};
    bool line_contained{false};
};

[[nodiscard]] double formula_vertical_overlap_fraction(const Rect& a, const Rect& b);
[[nodiscard]] double formula_horizontal_gap(const Rect& a, const Rect& b);
[[nodiscard]] bool formula_same_text_row(const Rect& formula, const Rect& text, double body_font);
[[nodiscard]] FormulaLayoutDecision resolve_formula_layout(
    std::span<const FormulaLayoutNode> nodes,
    std::size_t candidate_index,
    double body_font,
    double page_width);

}  // namespace palimpsest::detail
