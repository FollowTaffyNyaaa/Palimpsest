#include "abstract_layout.h"

#include <algorithm>
#include <cmath>

namespace palimpsest::detail {
namespace {

bool columns_compatible(const AbstractLayoutNode& a, const AbstractLayoutNode& b) {
    return a.column == b.column || a.column < 0 || b.column < 0;
}

double horizontal_overlap_ratio(const Rect& a, const Rect& b) {
    const double overlap = std::max(0.0, std::min(a.right(), b.right()) - std::max(a.x, b.x));
    return overlap / std::max(1.0, std::min(a.width, b.width));
}

bool prose_candidate(
    const AbstractLayoutNode& node,
    const double body_font,
    const double page_width) {
    if (node.title || node.author || node.affiliation || node.keywords || node.heading) return false;
    if (node.prose_words < 4) return false;
    if (node.font_size < body_font * 0.62 || node.font_size > body_font * 1.28) return false;
    return node.bbox.width >= std::max(36.0, page_width * 0.12);
}

bool front_matter_boundary(const AbstractLayoutNode& node, const double body_font) {
    if (node.keywords) return true;
    return node.heading && !node.abstract_hint && node.font_size >= body_font * 1.02;
}

bool substantial_run(const std::vector<std::size_t>& run, const std::span<const AbstractLayoutNode> nodes) {
    int words = 0;
    for (const auto index : run) words += nodes[index].prose_words;
    return (run.size() >= 3 && words >= 30) ||
        (run.size() >= 2 && words >= 44) ||
        words >= 70;
}

}  // namespace

AbstractLayoutDecision resolve_abstract_layout(
    const std::span<const AbstractLayoutNode> nodes,
    const double body_font,
    const double page_width,
    const double page_height) {
    AbstractLayoutDecision decision;
    if (nodes.empty()) return decision;

    double identity_bottom = 0.0;
    for (const auto& node : nodes) {
        if (node.title || node.author || node.affiliation) {
            identity_bottom = std::max(identity_bottom, node.bbox.bottom());
        }
    }

    const double search_floor = std::max(identity_bottom, page_height * 0.05);
    double stop_y = page_height * 0.72;
    for (const auto& node : nodes) {
        if (node.bbox.y <= search_floor + body_font * 0.5) continue;
        if (front_matter_boundary(node, body_font)) {
            stop_y = std::min(stop_y, node.bbox.y);
        }
    }

    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const auto& node = nodes[i];
        if (!node.abstract_hint) continue;
        if (node.bbox.y + body_font < search_floor || node.bbox.y >= stop_y) continue;
        decision.hint_index = i;
        break;
    }

    if (decision.hint_index != kNoAbstractAnchor) {
        const auto& hint = nodes[decision.hint_index];
        std::size_t previous = decision.hint_index;
        bool have_prose = false;

        if (hint.prose_words >= 4) {
            decision.abstract_indices.push_back(decision.hint_index);
            have_prose = true;
        }

        for (std::size_t i = decision.hint_index + 1; i < nodes.size(); ++i) {
            const auto& node = nodes[i];
            if (node.bbox.y >= stop_y) break;
            if (!columns_compatible(hint, node)) continue;
            if (!prose_candidate(node, body_font, page_width)) continue;

            const auto& prev = nodes[previous];
            const double gap = node.bbox.y - prev.bbox.bottom();
            if (have_prose && gap > std::max(8.0, body_font * 2.8)) break;
            if (!have_prose && node.bbox.y - hint.bbox.bottom() > std::max(12.0, body_font * 4.0)) break;

            if (have_prose &&
                horizontal_overlap_ratio(prev.bbox, node.bbox) < 0.38 &&
                std::abs(prev.bbox.x - node.bbox.x) > body_font * 4.0) {
                break;
            }

            decision.abstract_indices.push_back(i);
            previous = i;
            have_prose = true;
        }
        return decision;
    }

    // No "Abstract" label is required. Infer the first substantial, paragraph-like
    // prose run in the front matter between identity material and the first real
    // section boundary. This is intentionally based on structure rather than venue templates.
    double best_score = -1.0e9;
    std::vector<std::size_t> best_run;

    for (std::size_t seed = 0; seed < nodes.size(); ++seed) {
        const auto& first = nodes[seed];
        if (first.bbox.y <= search_floor || first.bbox.y >= stop_y) continue;
        if (first.bbox.y > page_height * 0.62) continue;
        if (!prose_candidate(first, body_font, page_width)) continue;

        std::vector<std::size_t> run{seed};
        std::size_t previous = seed;
        int words = first.prose_words;

        for (std::size_t i = seed + 1; i < nodes.size(); ++i) {
            const auto& node = nodes[i];
            if (node.bbox.y >= stop_y) break;
            if (!columns_compatible(first, node)) continue;
            if (front_matter_boundary(node, body_font)) break;
            if (!prose_candidate(node, body_font, page_width)) continue;

            const auto& prev = nodes[previous];
            const double gap = node.bbox.y - prev.bbox.bottom();
            if (gap > std::max(8.0, body_font * 2.6)) break;
            if (horizontal_overlap_ratio(prev.bbox, node.bbox) < 0.38 &&
                std::abs(prev.bbox.x - node.bbox.x) > body_font * 4.0) {
                break;
            }

            run.push_back(i);
            previous = i;
            words += node.prose_words;
        }

        if (!substantial_run(run, nodes)) continue;
        const double early_penalty = (first.bbox.y - search_floor) / std::max(1.0, body_font);
        const double score = static_cast<double>(words) + static_cast<double>(run.size()) * 6.0 - early_penalty * 0.35;
        if (score > best_score) {
            best_score = score;
            best_run = std::move(run);
        }
    }

    if (!best_run.empty()) {
        decision.abstract_indices = std::move(best_run);
        decision.inferred_without_label = true;
    }
    return decision;
}

}  // namespace palimpsest::detail
