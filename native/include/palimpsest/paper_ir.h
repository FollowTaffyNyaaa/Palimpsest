#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace palimpsest {

enum class RegionType {
    Title,
    Author,
    Affiliation,
    Abstract,
    Keywords,
    Heading,
    Body,
    AppendixHeading,
    AppendixBody,
    Footnote,
    Figure,
    FigureCaption,
    Table,
    TableCaption,
    FormulaDisplay,
    Algorithm,
    Code,
    Header,
    Footer,
    Reference,
    Other,
};

struct Rect {
    double x{};
    double y{};
    double width{};
    double height{};

    [[nodiscard]] double right() const { return x + width; }
    [[nodiscard]] double bottom() const { return y + height; }
};

struct Region {
    std::string region_id;
    RegionType type{RegionType::Other};
    std::string text;
    Rect bbox;
    std::string font_name;
    double font_size{};
    int column{-1};
    int reading_order{-1};
    bool locked{false};
    double confidence{0.5};
    std::string source_kind{"pdf_text"};
    std::string locked_reason;
    std::string parent_region_id;
    std::string span_mode{"column"};
};

struct Page {
    int page_index{};
    double width{};
    double height{};
    int rotation{};
    int column_count{1};
    double gutter_x{-1.0};
    std::string background_asset;
    std::vector<Region> locked_regions;
    std::vector<Region> semantic_regions;
};

struct LayoutFragment {
    int page_index{};
    int column{-1};
    Rect bbox;
    std::string source_text;
    std::string region_id;
    double font_size{};
    std::string span_mode{"column"};
};

struct Paragraph {
    std::string paragraph_id;
    RegionType semantic_type{RegionType::Body};
    std::string section_id;
    std::string section_title;
    int section_level{};
    std::string source_text;
    std::string normalized_text;
    std::string translated_text;
    std::vector<LayoutFragment> layout_fragments;
    std::vector<int> page_span;
    std::vector<int> column_span;
    Rect original_bbox;
    Rect preferred_bbox;
    double original_font_size{};
    std::string original_weight{"regular"};
    std::string original_alignment{"left"};
    double original_line_height{};
    std::string flow_id{"FLOW_001"};
    int reading_order{};
    std::string previous_id;
    std::string next_id;
    std::string translation_state{"PENDING"};
};

struct Document {
    std::string document_id;
    std::string source_path;
    std::string source_hash;
    std::string language{"en"};
    int page_count{};
    std::vector<Page> pages;
    std::vector<Paragraph> paragraphs;
};

[[nodiscard]] const char* to_string(RegionType type);
[[nodiscard]] bool is_translatable(RegionType type);
[[nodiscard]] bool is_locked(RegionType type);
[[nodiscard]] std::string to_json(const Document& document);
bool write_json(const Document& document, const std::filesystem::path& path, std::string& error);

}  // namespace palimpsest
