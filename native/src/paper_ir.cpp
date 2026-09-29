#include <palimpsest/paper_ir.h>

#include <fstream>
#include <iomanip>
#include <sstream>

namespace palimpsest {
namespace {

std::string escape_json(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
            case '\\': out << "\\\\"; break;
            case '"': out << "\\\""; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (ch < 0x20) {
                    out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch) << std::dec;
                } else {
                    out << static_cast<char>(ch);
                }
        }
    }
    return out.str();
}

void append_rect(std::ostringstream& out, const Rect& rect) {
    out << "{\"x\":" << rect.x
        << ",\"y\":" << rect.y
        << ",\"width\":" << rect.width
        << ",\"height\":" << rect.height << "}";
}

template <typename T>
void append_int_array(std::ostringstream& out, const std::vector<T>& values) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out << ',';
        out << values[i];
    }
    out << ']';
}

}  // namespace

const char* to_string(const RegionType type) {
    switch (type) {
        case RegionType::Title: return "TITLE";
        case RegionType::Author: return "AUTHOR";
        case RegionType::Affiliation: return "AFFILIATION";
        case RegionType::Abstract: return "ABSTRACT";
        case RegionType::Keywords: return "KEYWORDS";
        case RegionType::Heading: return "HEADING";
        case RegionType::Body: return "BODY";
        case RegionType::AppendixHeading: return "APPENDIX_HEADING";
        case RegionType::AppendixBody: return "APPENDIX_BODY";
        case RegionType::Footnote: return "FOOTNOTE";
        case RegionType::Figure: return "FIGURE";
        case RegionType::FigureCaption: return "FIGURE_CAPTION";
        case RegionType::Table: return "TABLE";
        case RegionType::TableCaption: return "TABLE_CAPTION";
        case RegionType::FormulaDisplay: return "FORMULA_DISPLAY";
        case RegionType::Algorithm: return "ALGORITHM";
        case RegionType::Code: return "CODE";
        case RegionType::Header: return "HEADER";
        case RegionType::Footer: return "FOOTER";
        case RegionType::Reference: return "REFERENCE";
        case RegionType::Other: return "OTHER";
    }
    return "OTHER";
}

bool is_translatable(const RegionType type) {
    return type == RegionType::Abstract || type == RegionType::Body || type == RegionType::AppendixHeading || type == RegionType::AppendixBody;
}

bool is_locked(const RegionType type) {
    return !is_translatable(type) && type != RegionType::Other;
}

std::string to_json(const Document& document) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "{\n";
    out << "  \"schema_version\": \"0.4\",\n";
    out << "  \"document_id\": \"" << escape_json(document.document_id) << "\",\n";
    out << "  \"source_path\": \"" << escape_json(document.source_path) << "\",\n";
    out << "  \"source_hash\": \"" << escape_json(document.source_hash) << "\",\n";
    out << "  \"language\": \"" << escape_json(document.language) << "\",\n";
    out << "  \"page_count\": " << document.page_count << ",\n";
    out << "  \"pages\": [\n";
    for (std::size_t p = 0; p < document.pages.size(); ++p) {
        const auto& page = document.pages[p];
        out << "    {\n";
        out << "      \"page_index\": " << page.page_index << ",\n";
        out << "      \"width\": " << page.width << ",\n";
        out << "      \"height\": " << page.height << ",\n";
        out << "      \"rotation\": " << page.rotation << ",\n";
        out << "      \"column_layout\": " << page.column_count << ",\n";
        out << "      \"gutter_x\": " << page.gutter_x << ",\n";
        out << "      \"background_asset\": \"" << escape_json(page.background_asset) << "\",\n";
        out << "      \"locked_regions\": [\n";
        for (std::size_t r = 0; r < page.locked_regions.size(); ++r) {
            const auto& region = page.locked_regions[r];
            out << "        {\"region_id\":\"" << escape_json(region.region_id)
                << "\",\"type\":\"" << to_string(region.type)
                << "\",\"text\":\"" << escape_json(region.text)
                << "\",\"bbox\":";
            append_rect(out, region.bbox);
            out << ",\"font_name\":\"" << escape_json(region.font_name)
                << "\",\"font_size\":" << region.font_size
                << ",\"column\":" << region.column
                << ",\"reading_order\":" << region.reading_order
                << ",\"locked\":" << (region.locked ? "true" : "false")
                << ",\"confidence\":" << region.confidence
                << ",\"source_kind\":\"" << escape_json(region.source_kind)
                << "\",\"locked_reason\":\"" << escape_json(region.locked_reason)
                << "\",\"parent_region_id\":\"" << escape_json(region.parent_region_id)
                << "\",\"span_mode\":\"" << escape_json(region.span_mode) << "\"}";
            if (r + 1 != page.locked_regions.size()) out << ',';
            out << '\n';
        }
        out << "      ],\n";
        out << "      \"semantic_regions\": [\n";
        for (std::size_t r = 0; r < page.semantic_regions.size(); ++r) {
            const auto& region = page.semantic_regions[r];
            out << "        {\"region_id\":\"" << escape_json(region.region_id)
                << "\",\"type\":\"" << to_string(region.type)
                << "\",\"text\":\"" << escape_json(region.text)
                << "\",\"bbox\":";
            append_rect(out, region.bbox);
            out << ",\"font_name\":\"" << escape_json(region.font_name)
                << "\",\"font_size\":" << region.font_size
                << ",\"column\":" << region.column
                << ",\"reading_order\":" << region.reading_order
                << ",\"locked\":" << (region.locked ? "true" : "false")
                << ",\"confidence\":" << region.confidence
                << ",\"source_kind\":\"" << escape_json(region.source_kind)
                << "\",\"locked_reason\":\"" << escape_json(region.locked_reason)
                << "\",\"parent_region_id\":\"" << escape_json(region.parent_region_id)
                << "\",\"span_mode\":\"" << escape_json(region.span_mode) << "\"}";
            if (r + 1 != page.semantic_regions.size()) out << ',';
            out << '\n';
        }
        out << "      ]\n";
        out << "    }";
        if (p + 1 != document.pages.size()) out << ',';
        out << '\n';
    }
    out << "  ],\n";
    out << "  \"paragraphs\": [\n";
    for (std::size_t i = 0; i < document.paragraphs.size(); ++i) {
        const auto& paragraph = document.paragraphs[i];
        out << "    {\n";
        out << "      \"paragraph_id\": \"" << escape_json(paragraph.paragraph_id) << "\",\n";
        out << "      \"semantic_type\": \"" << to_string(paragraph.semantic_type) << "\",\n";
        out << "      \"section_id\": \"" << escape_json(paragraph.section_id) << "\",\n";
        out << "      \"section_title\": \"" << escape_json(paragraph.section_title) << "\",\n";
        out << "      \"section_level\": " << paragraph.section_level << ",\n";
        out << "      \"source_text\": \"" << escape_json(paragraph.source_text) << "\",\n";
        out << "      \"normalized_text\": \"" << escape_json(paragraph.normalized_text) << "\",\n";
        out << "      \"translated_text\": \"" << escape_json(paragraph.translated_text) << "\",\n";
        out << "      \"layout_fragments\": [\n";
        for (std::size_t f = 0; f < paragraph.layout_fragments.size(); ++f) {
            const auto& fragment = paragraph.layout_fragments[f];
            out << "        {\"page_index\":" << fragment.page_index
                << ",\"column\":" << fragment.column
                << ",\"bbox\":";
            append_rect(out, fragment.bbox);
            out << ",\"source_text\":\"" << escape_json(fragment.source_text)
                << "\",\"region_id\":\"" << escape_json(fragment.region_id)
                << "\",\"font_size\":" << fragment.font_size
                << ",\"span_mode\":\"" << escape_json(fragment.span_mode) << "\"}";
            if (f + 1 != paragraph.layout_fragments.size()) out << ',';
            out << '\n';
        }
        out << "      ],\n";
        out << "      \"page_span\": "; append_int_array(out, paragraph.page_span); out << ",\n";
        out << "      \"column_span\": "; append_int_array(out, paragraph.column_span); out << ",\n";
        out << "      \"original_bbox\": "; append_rect(out, paragraph.original_bbox); out << ",\n";
        out << "      \"preferred_bbox\": "; append_rect(out, paragraph.preferred_bbox); out << ",\n";
        out << "      \"original_font_size\": " << paragraph.original_font_size << ",\n";
        out << "      \"original_line_height\": " << paragraph.original_line_height << ",\n";
        out << "      \"flow_id\": \"" << paragraph.flow_id << "\",\n";
        out << "      \"reading_order\": " << paragraph.reading_order << ",\n";
        out << "      \"previous_id\": \"" << escape_json(paragraph.previous_id) << "\",\n";
        out << "      \"next_id\": \"" << escape_json(paragraph.next_id) << "\",\n";
        out << "      \"translation_state\": \"" << paragraph.translation_state << "\"\n";
        out << "    }";
        if (i + 1 != document.paragraphs.size()) out << ',';
        out << '\n';
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

bool write_json(const Document& document, const std::filesystem::path& path, std::string& error) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "Unable to create PaperIR JSON at: " + path.string();
        return false;
    }
    const auto json = to_json(document);
    file.write(json.data(), static_cast<std::streamsize>(json.size()));
    if (!file) {
        error = "Failed while writing PaperIR JSON: " + path.string();
        return false;
    }
    return true;
}

}  // namespace palimpsest
