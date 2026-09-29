#pragma once

#include <palimpsest/paper_ir.h>

#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace palimpsest::detail {

inline constexpr std::size_t kNoAbstractAnchor = std::numeric_limits<std::size_t>::max();

struct AbstractLayoutNode {
    Rect bbox;
    int column{-1};
    double font_size{};
    int prose_words{};
    bool title{false};
    bool author{false};
    bool affiliation{false};
    bool keywords{false};
    bool heading{false};
    bool abstract_hint{false};
};

struct AbstractLayoutDecision {
    std::vector<std::size_t> abstract_indices;
    std::size_t hint_index{kNoAbstractAnchor};
    bool inferred_without_label{false};
};

[[nodiscard]] AbstractLayoutDecision resolve_abstract_layout(
    std::span<const AbstractLayoutNode> nodes,
    double body_font,
    double page_width,
    double page_height);

}  // namespace palimpsest::detail
