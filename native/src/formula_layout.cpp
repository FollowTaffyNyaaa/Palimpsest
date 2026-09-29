#include "formula_layout.h"

#include <algorithm>
#include <cmath>

namespace palimpsest::detail {
namespace {

bool columns_compatible(const FormulaLayoutNode& a, const FormulaLayoutNode& b) {
    return a.column == b.column || a.column < 0 || b.column < 0;
}

double center_y(const Rect& rect) {
    return rect.y + rect.height * 0.5;
}

bool equation_number_same_row(
    const Rect& formula,
    const Rect& number,
    const double body_font,
    const double page_width) {
    const double overlap = formula_vertical_overlap_fraction(formula, number);
    const double center_delta = std::abs(center_y(formula) - center_y(number));
    const double gap = formula_horizontal_gap(formula, number);
    return (overlap >= 0.20 || center_delta <= std::max(3.0, body_font * 0.55)) &&
        gap <= std::max(body_font * 8.0, page_width * 0.45);
}

}  // namespace

double formula_vertical_overlap_fraction(const Rect& a, const Rect& b) {
    const double overlap = std::max(0.0, std::min(a.bottom(), b.bottom()) - std::max(a.y, b.y));
    return overlap / std::max(1.0, std::min(a.height, b.height));
}

double formula_horizontal_gap(const Rect& a, const Rect& b) {
    return std::max({0.0, b.x - a.right(), a.x - b.right()});
}

bool formula_same_text_row(const Rect& formula, const Rect& text, const double body_font) {
    const double overlap = formula_vertical_overlap_fraction(formula, text);
    const double center_delta = std::abs(center_y(formula) - center_y(text));
    const double gap = formula_horizontal_gap(formula, text);

    // Pix2Text-style primary rule: a formula that overlaps a text line by ~60%
    // is an inline candidate. Normalize by the smaller box so tall fractions,
    // sums and integrals can still be recognized as inline.
    if (overlap >= 0.60 && gap <= std::max(8.0, body_font * 5.0)) return true;

    // PDF glyph extraction can split subscripts/superscripts into a neighboring
    // y bucket. Treat a very close fragment in the same baseline band as inline
    // even when its box has little vertical overlap with the prose box.
    return center_delta <= std::max(3.0, body_font * 0.65) &&
        gap <= std::max(5.0, body_font * 2.25);
}

FormulaLayoutDecision resolve_formula_layout(
    const std::span<const FormulaLayoutNode> nodes,
    const std::size_t candidate_index,
    const double body_font,
    const double page_width) {
    FormulaLayoutDecision decision;
    if (candidate_index >= nodes.size()) return decision;

    const auto& candidate = nodes[candidate_index];
    bool has_same_row_prose = false;
    bool has_ambiguous_prose = false;
    double best_anchor_score = -1.0e9;

    const double candidate_center_x = candidate.bbox.x + candidate.bbox.width * 0.5;
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        if (i == candidate_index) continue;
        const auto& node = nodes[i];
        if (!columns_compatible(candidate, node)) continue;

        if (node.equation_number &&
            equation_number_same_row(candidate.bbox, node.bbox, body_font, page_width)) {
            decision.has_equation_number = true;
        }

        if (!node.prose) continue;

        const double overlap = formula_vertical_overlap_fraction(candidate.bbox, node.bbox);
        const double gap = formula_horizontal_gap(candidate.bbox, node.bbox);
        const double center_delta = std::abs(center_y(candidate.bbox) - center_y(node.bbox));
        const bool same_row = formula_same_text_row(candidate.bbox, node.bbox, body_font);

        if (same_row) {
            has_same_row_prose = true;
            decision.text_overlap = std::max(decision.text_overlap, overlap);
            decision.line_contained = decision.line_contained ||
                center_delta <= std::max(3.0, body_font * 0.65);

            const double node_center_x = node.bbox.x + node.bbox.width * 0.5;
            if (node_center_x < candidate_center_x) decision.has_left_text = true;
            if (node_center_x > candidate_center_x) decision.has_right_text = true;

            const double score = overlap * 10.0 - gap / std::max(1.0, body_font);
            if (score > best_anchor_score) {
                best_anchor_score = score;
                decision.anchor_index = i;
            }
            continue;
        }

        const bool weak_overlap = overlap >= 0.25 && gap <= std::max(6.0, body_font * 4.0);
        const bool weak_baseline = center_delta <= std::max(4.0, body_font * 0.85) &&
            gap <= std::max(5.0, body_font * 2.75);
        has_ambiguous_prose = has_ambiguous_prose || weak_overlap || weak_baseline;
    }

    if (has_same_row_prose) {
        decision.kind = FormulaLayoutKind::Inline;
    } else if (decision.has_equation_number) {
        decision.kind = FormulaLayoutKind::Display;
    } else if (has_ambiguous_prose) {
        decision.kind = FormulaLayoutKind::Uncertain;
    } else {
        // A math candidate with no prose occupying its row is an own-line formula.
        // This deliberately avoids width/centering as a hard requirement so
        // short equations such as "x = 0" remain valid display equations.
        decision.kind = FormulaLayoutKind::Display;
    }

    return decision;
}

}  // namespace palimpsest::detail
