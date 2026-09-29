#include <palimpsest/c_api.h>
#include <palimpsest/inspector.h>
#include <palimpsest/paper_ir.h>

#include "pdfium_runtime.h"
#include "formula_layout.h"
#include "abstract_layout.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <numeric>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(_WIN32)
#include <objbase.h>
#include <wincodec.h>
#endif

namespace palimpsest {
namespace {

constexpr int kRenderAnnotations = 0x01;
constexpr int kRenderLcdText = 0x02;

struct Glyph {
    std::uint32_t codepoint{};
    Rect bbox;
    std::string font_name;
    double font_size{};
    int font_flags{};
    int font_weight{};
    int source_index{};
};

struct Raster {
    int width{};
    int height{};
    int stride{};
    std::vector<std::uint8_t> bgra;
};

struct LineRecord {
    std::string text;
    Rect bbox;
    std::string font_name;
    double font_size{};
    int font_weight{};
    double math_font_ratio{};
    int column{-1};
    RegionType type{RegionType::Other};
    int reading_order{-1};
    double confidence{0.5};
    std::string locked_reason;
    int parent_locked_index{-1};
    std::string span_mode{"column"};
};

struct ColumnLayout {
    int count{1};
    double gutter_x{-1.0};
    double gutter_left{-1.0};
    double gutter_right{-1.0};
};

struct VisualCandidate {
    Rect bbox;
    RegionType type{RegionType::Figure};
    double confidence{0.0};
    std::string reason;
    std::string source_kind{"render_residual"};
    int column{-1};
    std::string span_mode{"column"};
};

struct BlockRecord {
    std::string text;
    Rect bbox;
    std::string font_name;
    double font_size{};
    int column{-1};
    RegionType type{RegionType::Other};
    int reading_order{-1};
    double confidence{0.5};
    bool locked{false};
    std::string locked_reason;
    int parent_locked_index{-1};
    std::string span_mode{"column"};
};

std::string join_line_text(std::string current, const std::string& next);
double horizontal_overlap_ratio(const Rect& a, const Rect& b);
bool ends_sentence(const std::string& text);
bool begins_upper_ascii(const std::string& text);
bool looks_like_equation_number_text(const std::string& text);

std::string path_to_utf8(const std::filesystem::path& path) {
#if defined(_WIN32)
    const auto wide = path.native();
    if (wide.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return "<unprintable-path>";
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), size, nullptr, nullptr);
    return result;
#else
    return path.string();
#endif
}

std::filesystem::path path_from_utf8(const char* value) {
#if defined(_WIN32)
    if (value == nullptr || *value == '\0') return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, wide.data(), size);
    wide.pop_back();
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(value);
#endif
}

std::vector<std::uint8_t> read_all_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Unable to open input PDF: " + path_to_utf8(path));
    }
    file.seekg(0, std::ios::end);
    const auto size = file.tellg();
    if (size <= 0) {
        throw std::runtime_error("Input PDF is empty: " + path_to_utf8(path));
    }
    file.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file) {
        throw std::runtime_error("Failed while reading input PDF: " + path_to_utf8(path));
    }
    return bytes;
}

std::string utf8_from_codepoint(const std::uint32_t cp) {
    switch (cp) {
        case 0x00A0: return " ";
        case 0xFB00: return "ff";
        case 0xFB01: return "fi";
        case 0xFB02: return "fl";
        case 0xFB03: return "ffi";
        case 0xFB04: return "ffl";
        case 0xFB05:
        case 0xFB06: return "st";
        default: break;
    }
    std::string out;
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return out;
}

double median(std::vector<double> values, const double fallback = 10.0) {
    if (values.empty()) return fallback;
    const auto mid = values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2);
    std::nth_element(values.begin(), mid, values.end());
    double result = *mid;
    if (values.size() % 2 == 0) {
        const auto lower = std::max_element(values.begin(), mid);
        result = (result + *lower) * 0.5;
    }
    return result;
}

Rect union_rect(const Rect& a, const Rect& b) {
    if (a.width <= 0 || a.height <= 0) return b;
    if (b.width <= 0 || b.height <= 0) return a;
    const double left = std::min(a.x, b.x);
    const double top = std::min(a.y, b.y);
    const double right = std::max(a.right(), b.right());
    const double bottom = std::max(a.bottom(), b.bottom());
    return {left, top, right - left, bottom - top};
}

std::string trim_copy(std::string text) {
    auto not_space = [](const unsigned char ch) { return !std::isspace(ch); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), not_space));
    text.erase(std::find_if(text.rbegin(), text.rend(), not_space).base(), text.end());
    return text;
}

std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool starts_with_any(const std::string& lower, const std::initializer_list<std::string_view> prefixes) {
    for (const auto prefix : prefixes) {
        if (lower.rfind(prefix, 0) == 0) return true;
    }
    return false;
}

bool contains_any(const std::string& lower, const std::initializer_list<std::string_view> needles) {
    for (const auto needle : needles) {
        if (lower.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::string read_font_name(detail::PdfiumRuntime& api, detail::FPDF_TEXTPAGE text_page, const int index, int& flags) {
    flags = 0;
    const auto needed = api.TextGetFontInfo(text_page, index, nullptr, 0, &flags);
    if (needed == 0) return {};
    std::vector<char> buffer(static_cast<std::size_t>(needed) + 1, '\0');
    if (api.TextGetFontInfo(text_page, index, buffer.data(), needed, &flags) == 0) return {};
    return std::string(buffer.data());
}

bool is_math_font_name(const std::string& font_name) {
    const auto font = lower_ascii(font_name);
    return contains_any(font, {
        "math", "symbol", "cmsy", "cmmi", "cmex", "msam", "msbm", "stmary", "rsfs",
        "eufm", "eurm", "txsy", "txmi", "ntxmi", "mathtime", "stixmath", "mtmi"
    });
}

std::vector<Glyph> extract_glyphs(detail::PdfiumRuntime& api, detail::FPDF_TEXTPAGE text_page, const double page_height) {
    std::vector<Glyph> glyphs;
    const int count = api.TextCountChars(text_page);
    glyphs.reserve(static_cast<std::size_t>(std::max(0, count)));
    for (int i = 0; i < count; ++i) {
        const auto cp = api.TextGetUnicode(text_page, i);
        if (cp == 0 || cp == '\r' || cp == '\n' || (cp < 0x20 && cp != '\t')) continue;
        double left{}, right{}, bottom{}, top{};
        if (!api.TextGetCharBox(text_page, i, &left, &right, &bottom, &top)) continue;
        if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(bottom) || !std::isfinite(top)) continue;
        if (right < left || top < bottom) continue;
        const double fs = api.TextGetFontSize(text_page, i);
        int font_flags = 0;
        auto font_name = read_font_name(api, text_page, i, font_flags);
        const int font_weight = api.TextGetFontWeight(text_page, i);
        glyphs.push_back({cp, {left, page_height - top, right - left, top - bottom}, std::move(font_name), fs > 0 ? fs : 10.0,
                          font_flags, font_weight > 0 ? font_weight : 400, i});
    }
    return glyphs;
}

std::vector<LineRecord> build_lines(std::vector<Glyph> glyphs, const double page_width) {
    if (glyphs.empty()) return {};
    std::sort(glyphs.begin(), glyphs.end(), [](const Glyph& a, const Glyph& b) {
        const double ay = a.bbox.y + a.bbox.height * 0.5;
        const double by = b.bbox.y + b.bbox.height * 0.5;
        if (std::abs(ay - by) > 1.0) return ay < by;
        if (std::abs(a.bbox.x - b.bbox.x) > 0.15) return a.bbox.x < b.bbox.x;
        return a.source_index < b.source_index;
    });

    struct Bucket { std::vector<Glyph> glyphs; double center_y{}; double mean_font{}; };
    std::vector<Bucket> buckets;
    for (const auto& glyph : glyphs) {
        const double center = glyph.bbox.y + glyph.bbox.height * 0.5;
        Bucket* best = nullptr;
        double best_distance = std::numeric_limits<double>::max();
        for (auto it = buckets.rbegin(); it != buckets.rend() && std::distance(buckets.rbegin(), it) < 8; ++it) {
            const double tolerance = std::max(1.6, std::max(it->mean_font, glyph.font_size) * 0.42);
            const double distance = std::abs(center - it->center_y);
            if (distance <= tolerance && distance < best_distance) {
                best = &*it;
                best_distance = distance;
            }
        }
        if (best == nullptr) {
            buckets.push_back({{glyph}, center, glyph.font_size});
        } else {
            best->glyphs.push_back(glyph);
            const double n = static_cast<double>(best->glyphs.size());
            best->center_y += (center - best->center_y) / n;
            best->mean_font += (glyph.font_size - best->mean_font) / n;
        }
    }

    std::vector<LineRecord> lines;
    lines.reserve(buckets.size() * 2);
    for (auto& bucket : buckets) {
        std::sort(bucket.glyphs.begin(), bucket.glyphs.end(), [](const Glyph& a, const Glyph& b) {
            if (std::abs(a.bbox.x - b.bbox.x) > 0.15) return a.bbox.x < b.bbox.x;
            return a.source_index < b.source_index;
        });
        std::vector<std::vector<Glyph>> segments;
        std::vector<Glyph> current;
        double previous_content_right = 0.0;
        bool have_previous_content = false;
        double current_font_sum = 0.0;
        int current_font_count = 0;
        for (const auto& glyph : bucket.glyphs) {
            const bool whitespace = glyph.codepoint == ' ' || glyph.codepoint == '\t';
            if (!whitespace && have_previous_content) {
                const double gap = glyph.bbox.x - previous_content_right;
                const double split_threshold = std::max(6.0, std::min(page_width * 0.018, std::max(bucket.mean_font, glyph.font_size) * 0.70));
                const double current_font = current_font_count > 0 ? current_font_sum / current_font_count : bucket.mean_font;
                const bool crosses_page_middle = previous_content_right < page_width * 0.54 && glyph.bbox.x > page_width * 0.46;
                const bool strong_size_jump = std::abs(glyph.font_size - current_font) > std::max(0.9, current_font * 0.14);
                if ((gap > split_threshold || (crosses_page_middle && strong_size_jump)) && !current.empty()) {
                    segments.push_back(std::move(current));
                    current.clear();
                    current_font_sum = 0.0;
                    current_font_count = 0;
                    have_previous_content = false;
                }
            }
            current.push_back(glyph);
            if (!whitespace) {
                previous_content_right = glyph.bbox.right();
                have_previous_content = true;
                current_font_sum += glyph.font_size;
                ++current_font_count;
            }
        }
        if (!current.empty()) segments.push_back(std::move(current));

        for (const auto& segment : segments) {
            std::string text;
            Rect bbox{};
            std::vector<double> fonts;
            std::unordered_map<std::string, int> font_counts;
            std::unordered_map<std::string, int> font_weights;
            int visible_glyphs = 0;
            int math_glyphs = 0;
            double prev_right = 0.0;
            bool have_prev = false;
            for (const auto& glyph : segment) {
                const bool explicit_space = glyph.codepoint == ' ' || glyph.codepoint == '\t';
                if (explicit_space) {
                    continue;
                }
                fonts.push_back(glyph.font_size);
                ++visible_glyphs;
                if (!glyph.font_name.empty()) {
                    ++font_counts[glyph.font_name];
                    font_weights[glyph.font_name] += glyph.font_weight;
                    if (is_math_font_name(glyph.font_name)) ++math_glyphs;
                }
                bbox = union_rect(bbox, glyph.bbox);
                if (have_prev) {
                    const double gap = glyph.bbox.x - prev_right;
                    const double threshold = std::max(1.2, std::min(glyph.font_size, bucket.mean_font) * 0.24);
                    if (gap > threshold && !text.empty() && text.back() != ' ') text.push_back(' ');
                }
                text += utf8_from_codepoint(glyph.codepoint);
                prev_right = std::max(prev_right, glyph.bbox.right());
                have_prev = true;
            }
            text = trim_copy(text);
            if (!text.empty()) {
                LineRecord line;
                line.text = std::move(text);
                line.bbox = bbox;
                line.font_size = median(fonts);
                if (!font_counts.empty()) {
                    const auto dominant = std::max_element(font_counts.begin(), font_counts.end(), [](const auto& a, const auto& b) {
                        return a.second < b.second;
                    });
                    line.font_name = dominant->first;
                    line.font_weight = font_weights[dominant->first] / std::max(1, dominant->second);
                }
                line.math_font_ratio = visible_glyphs > 0 ? static_cast<double>(math_glyphs) / visible_glyphs : 0.0;
                lines.push_back(std::move(line));
            }
        }
    }
    std::sort(lines.begin(), lines.end(), [](const LineRecord& a, const LineRecord& b) {
        if (std::abs(a.bbox.y - b.bbox.y) > 1.5) return a.bbox.y < b.bbox.y;
        return a.bbox.x < b.bbox.x;
    });
    return lines;
}

double quantile(std::vector<double> values, const double q) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(std::clamp(q, 0.0, 1.0) * static_cast<double>(values.size() - 1));
    return values[index];
}

ColumnLayout detect_columns(std::vector<LineRecord>& lines, const double page_width, const double page_height, const double body_font) {
    std::vector<double> left_right_edges;
    std::vector<double> right_left_edges;
    for (const auto& line : lines) {
        if (line.bbox.y < page_height * 0.12 || line.bbox.bottom() > page_height * 0.94) continue;
        if (line.font_size > body_font * 1.45 || line.bbox.width > page_width * 0.58 || line.text.size() < 8) continue;
        const double center = line.bbox.x + line.bbox.width * 0.5;
        if (center < page_width * 0.47 && line.bbox.right() < page_width * 0.56) left_right_edges.push_back(line.bbox.right());
        if (center > page_width * 0.53 && line.bbox.x > page_width * 0.44) right_left_edges.push_back(line.bbox.x);
    }

    ColumnLayout layout;
    if (left_right_edges.size() < 5 || right_left_edges.size() < 5) {
        for (auto& line : lines) { line.column = 0; line.span_mode = "single"; }
        return layout;
    }

    const double gutter_left = quantile(left_right_edges, 0.70);
    const double gutter_right = quantile(right_left_edges, 0.30);
    const double gap = gutter_right - gutter_left;
    const double gutter_x = (gutter_left + gutter_right) * 0.5;
    if (gap < std::max(8.0, page_width * 0.012) || gutter_x < page_width * 0.34 || gutter_x > page_width * 0.66) {
        for (auto& line : lines) { line.column = 0; line.span_mode = "single"; }
        return layout;
    }

    layout.count = 2;
    layout.gutter_left = gutter_left;
    layout.gutter_right = gutter_right;
    layout.gutter_x = gutter_x;
    for (auto& line : lines) {
        const double center = line.bbox.x + line.bbox.width * 0.5;
        if (line.bbox.width > page_width * 0.62 || (line.bbox.x < gutter_left && line.bbox.right() > gutter_right)) {
            line.column = -1;
            line.span_mode = "span";
        } else {
            line.column = center < gutter_x ? 0 : 1;
            line.span_mode = "column";
        }
    }
    return layout;
}

int prose_word_count(const std::string& text) {
    static const std::unordered_set<std::string> math_words = {
        "do", "log", "ln", "exp", "sin", "cos", "tan", "sign", "softmax", "sigmoid", "relu",
        "mha", "ffn", "mmd", "nll", "kl", "ce", "min", "max", "argmin", "argmax", "mean", "std",
        "var", "cov", "det", "tr", "rank", "diag"
    };
    int count = 0;
    std::string word;
    static const std::unordered_set<std::string> short_prose_words = {
        "as", "an", "is", "in", "of", "to", "by", "on", "at", "it", "we", "if", "or", "be"
    };
    auto flush = [&]() {
        if (word.size() >= 2) {
            auto lower = lower_ascii(word);
            const bool short_acronym = word.size() <= 5 && std::all_of(word.begin(), word.end(), [](const unsigned char c) {
                return std::isupper(c) != 0;
            });
            const bool prose_sized = word.size() >= 3 || short_prose_words.contains(lower);
            if (prose_sized && !short_acronym && math_words.find(lower) == math_words.end()) ++count;
        }
        word.clear();
    };
    for (const unsigned char ch : text) {
        if (ch < 128 && std::isalpha(ch)) word.push_back(static_cast<char>(ch));
        else flush();
    }
    flush();
    return count;
}

bool has_formula_prose_breaker(const std::string& text) {
    const auto lower = lower_ascii(text);
    static const std::array<std::string_view, 31> breakers = {
        " where ", " when ", " which ", " this ", " these ", " those ", " we ", " our ", " using ", " with ",
        " from ", " into ", " each ", " attention ", " head", " keyword", " figure", " fig ", " table", " baseline",
        " dataset", " result", " model", " method", " training", " cross-modal", " biases", " estimate ", " affected ",
        " confounder", " according "
    };
    const std::string padded = " " + lower + " ";
    return std::any_of(breakers.begin(), breakers.end(), [&](const std::string_view needle) {
        return padded.find(needle) != std::string::npos;
    });
}

bool looks_like_formula(const LineRecord& line, const double page_width) {
    int letters = 0, digits = 0, visible = 0, strong_operators = 0, minus_count = 0;
    bool has_equals = false;
    for (const unsigned char ch : line.text) {
        if (ch < 128 && std::isalpha(ch)) ++letters;
        if (std::isspace(ch)) continue;
        ++visible;
        if (std::isdigit(ch)) ++digits;
        if (ch == '=') has_equals = true;
        if (ch == '-') ++minus_count;
        if (std::string_view("=+*/<>^_\\|").find(static_cast<char>(ch)) != std::string_view::npos) ++strong_operators;
    }
    const int prose_words = prose_word_count(line.text);
    if (visible < 2 || prose_words > 2 || has_formula_prose_breaker(line.text)) return false;
    const double alpha_ratio = static_cast<double>(letters) / visible;
    const double center = line.bbox.x + line.bbox.width * 0.5;
    const bool centered = std::abs(center - page_width * 0.5) < page_width * 0.22;
    const bool compact_display = line.bbox.width < page_width * 0.48;
    const bool equation_structure = has_equals || strong_operators >= 2 || (minus_count >= 2 && prose_words <= 2);
    return (centered || compact_display) && line.bbox.width < page_width * 0.82 && equation_structure &&
        alpha_ratio < 0.82 && digits + strong_operators + minus_count >= 2;
}

bool is_formula_candidate_line(const LineRecord& line, const double page_width) {
    if (has_formula_prose_breaker(line.text) || prose_word_count(line.text) > 2) return false;
    if (looks_like_formula(line, page_width)) return true;

    const bool math_font = line.math_font_ratio >= 0.45 || is_math_font_name(line.font_name);
    if (math_font && line.text.size() <= 96 && line.bbox.width <= page_width * 0.55) return true;

    int operators = 0;
    for (const unsigned char ch : line.text) {
        if (std::string_view("=+*/<>^_\\|").find(static_cast<char>(ch)) != std::string_view::npos) ++operators;
    }
    return operators >= 1 && prose_word_count(line.text) <= 1 &&
        line.text.size() <= 80 && line.bbox.width <= page_width * 0.65;
}

bool is_formula_prose_anchor(const LineRecord& line, const bool formula_candidate) {
    if (formula_candidate) return false;
    switch (line.type) {
        case RegionType::Body:
        case RegionType::Abstract:
        case RegionType::Keywords:
        case RegionType::Heading:
        case RegionType::AppendixHeading:
        case RegionType::AppendixBody:
        case RegionType::Footnote:
            return prose_word_count(line.text) >= 1;
        default:
            return false;
    }
}

void resolve_formula_layouts(std::vector<LineRecord>& lines, const double page_width, const double body_font) {
    if (lines.empty()) return;

    std::vector<bool> candidates(lines.size(), false);
    std::vector<detail::FormulaLayoutNode> nodes;
    nodes.reserve(lines.size());

    for (std::size_t i = 0; i < lines.size(); ++i) {
        candidates[i] = is_formula_candidate_line(lines[i], page_width);
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        nodes.push_back({
            lines[i].bbox,
            lines[i].column,
            is_formula_prose_anchor(lines[i], candidates[i]),
            looks_like_equation_number_text(lines[i].text),
        });
    }

    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (!candidates[i]) continue;

        const auto decision = detail::resolve_formula_layout(nodes, i, body_font, page_width);
        if (decision.kind == detail::FormulaLayoutKind::Display) {
            lines[i].type = RegionType::FormulaDisplay;
            lines[i].confidence = decision.has_equation_number ? 0.97 :
                ((lines[i].math_font_ratio >= 0.45 || is_math_font_name(lines[i].font_name)) ? 0.94 : 0.88);
            lines[i].locked_reason = decision.has_equation_number
                ? "display_formula_equation_number"
                : "display_formula_relation";
            continue;
        }

        if (decision.kind == detail::FormulaLayoutKind::Inline &&
            decision.anchor_index != detail::kNoFormulaAnchor &&
            decision.anchor_index < lines.size()) {
            lines[i].type = lines[decision.anchor_index].type;
            if (lines[i].type == RegionType::Other) lines[i].type = RegionType::Body;
            lines[i].confidence = std::max(lines[i].confidence, 0.90);
            lines[i].locked_reason = "inline_formula_relation";
            continue;
        }

        // Ambiguous math fragments stay in the prose flow instead of being
        // destructively locked as display equations.
        if (lines[i].type == RegionType::FormulaDisplay) lines[i].type = RegionType::Body;
        if (lines[i].locked_reason.rfind("display_formula_", 0) == 0) lines[i].locked_reason.clear();
    }
}

bool looks_like_abstract_anchor_text(const std::string& text) {
    const auto lower = lower_ascii(trim_copy(text));
    constexpr std::string_view keyword = "abstract";
    if (lower.size() < keyword.size() || lower.compare(0, keyword.size(), keyword) != 0) return false;
    if (lower.size() == keyword.size()) return true;
    const auto next = static_cast<unsigned char>(lower[keyword.size()]);
    return next >= 128 || std::isspace(next) || !std::isalnum(next);
}

void classify_lines(std::vector<LineRecord>& lines, const double page_width, const double page_height, const double body_font, const int page_index) {
    double top_max_font = body_font;
    for (const auto& line : lines) {
        if (line.bbox.y < page_height * 0.28) top_max_font = std::max(top_max_font, line.font_size);
    }

    double title_bottom = -1.0;
    for (auto& line : lines) {
        if (line.bbox.y < page_height * 0.24 && line.font_size >= top_max_font * 0.88 && line.font_size >= body_font * 1.35) {
            line.type = RegionType::Title;
            line.confidence = 0.98;
            title_bottom = std::max(title_bottom, line.bbox.bottom());
        }
    }

    double abstract_start = page_height * 0.40;
    if (page_index == 0) {
        for (const auto& line : lines) {
            const auto lower = lower_ascii(trim_copy(line.text));
            if (looks_like_abstract_anchor_text(line.text)) {
                abstract_start = std::min(abstract_start, line.bbox.y);
            }
        }
    }

    bool in_references = false;
    bool in_abstract = false;
    for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
        auto& line = lines[line_index];
        const auto lower = lower_ascii(trim_copy(line.text));
        const double center = line.bbox.x + line.bbox.width * 0.5;
        const bool centered = std::abs(center - page_width * 0.5) < page_width * 0.14;
        const bool first_page_identity_zone = page_index == 0 && title_bottom > 0.0 && line.bbox.y > title_bottom - body_font && line.bbox.y < abstract_start - body_font * 0.25;
        static const std::regex explicit_section_heading(R"(^\s*(\d+|[IVXLCDM]+)(\.\d+)*[\.)]?\s+[A-Z])");
        static const std::regex lettered_subsection_heading(R"(^\s*[A-Z]\.\s+[A-Z])");
        const bool strong_section_heading = line.text.size() < 90 && std::regex_search(line.text, explicit_section_heading);
        const bool strong_subsection_heading = line.text.size() < 80 && std::regex_search(line.text, lettered_subsection_heading);
        const bool heading_size = line.font_size >= body_font * 1.20 && line.text.size() < 120;
        const bool abstract_stop_heading = strong_section_heading || strong_subsection_heading;

        if (line.type == RegionType::Title) {
            continue;
        } else if (line.bbox.y < page_height * 0.035 && line.font_size <= body_font * 1.15) {
            line.type = RegionType::Header;
            line.confidence = 0.96;
            line.locked_reason = "page_margin_header";
        } else if (line.bbox.bottom() > page_height * 0.965 && line.font_size <= body_font * 1.15) {
            line.type = RegionType::Footer;
            line.confidence = 0.96;
            line.locked_reason = "page_margin_footer";
        } else if (in_references) {
            line.type = RegionType::Reference;
            line.confidence = 0.97;
            line.locked_reason = "references_section";
        } else if (lower == "references" || lower == "reference") {
            line.type = RegionType::Reference;
            line.confidence = 0.99;
            line.locked_reason = "references_heading";
            in_references = true;
        } else if (starts_with_any(lower, {"figure ", "fig. ", "fig ", "figure:", "fig.:", "figure "})) {
            line.type = RegionType::FigureCaption;
            line.confidence = 0.99;
            line.locked_reason = "figure_caption";
        } else if (starts_with_any(lower, {"table ", "table:", "table "})) {
            line.type = RegionType::TableCaption;
            line.confidence = 0.99;
            line.locked_reason = "table_caption";
        } else if (starts_with_any(lower, {"algorithm ", "algorithm:"})) {
            line.type = RegionType::Algorithm;
            line.confidence = 0.94;
            line.locked_reason = "algorithm_anchor";
        } else if (line.text.size() <= 3 && line.bbox.width <= body_font * 5.5 && line.font_size <= body_font * 1.25) {
            line.type = RegionType::Other;
            line.confidence = 0.72;
            line.locked_reason = "isolated_compact_token";
        } else if (first_page_identity_zone && centered) {
            if (contains_any(lower, {"university", "institute", "department", "laboratory", "school", "college", "research center", "hospital", "academy", "@"})) {
                line.type = RegionType::Affiliation;
                line.confidence = 0.91;
            } else {
                line.type = RegionType::Author;
                line.confidence = 0.88;
            }
        } else if (looks_like_abstract_anchor_text(line.text)) {
            in_abstract = true;
            if (lower == "abstract" || line.text.size() < 14) {
                line.type = RegionType::Heading;
                line.confidence = 0.98;
            } else {
                line.type = RegionType::Abstract;
                line.confidence = 0.96;
            }
        } else if (starts_with_any(lower, {"keywords", "index terms", "key words"})) {
            line.type = RegionType::Keywords;
            line.confidence = 0.96;
            in_abstract = false;
        } else if (in_abstract && abstract_stop_heading) {
            line.type = RegionType::Heading;
            line.confidence = 0.94;
            in_abstract = false;
        } else if (in_abstract) {
            line.type = RegionType::Abstract;
            line.confidence = 0.91;
        } else {
            const bool heading_numbered = strong_section_heading || strong_subsection_heading;
            if (heading_size || heading_numbered) {
                line.type = RegionType::Heading;
                line.confidence = heading_numbered ? 0.94 : 0.84;
                in_abstract = false;
            } else if (line.bbox.y > page_height * 0.82 && line.font_size < body_font * 0.86) {
                line.type = RegionType::Footnote;
                line.confidence = 0.78;
            } else {
                line.type = RegionType::Body;
                line.confidence = 0.72;
            }
        }
    }
}

void resolve_front_matter_abstract(
    std::vector<LineRecord>& lines,
    const double page_width,
    const double page_height,
    const double body_font,
    const int page_index) {
    if (page_index != 0 || lines.empty()) return;

    std::vector<detail::AbstractLayoutNode> nodes;
    nodes.reserve(lines.size());
    for (const auto& line : lines) {
        nodes.push_back({
            line.bbox,
            line.column,
            line.font_size,
            prose_word_count(line.text),
            line.type == RegionType::Title,
            line.type == RegionType::Author,
            line.type == RegionType::Affiliation,
            line.type == RegionType::Keywords,
            line.type == RegionType::Heading,
            looks_like_abstract_anchor_text(line.text),
        });
    }

    const auto decision = detail::resolve_abstract_layout(nodes, body_font, page_width, page_height);
    for (const auto index : decision.abstract_indices) {
        if (index >= lines.size()) continue;
        auto& line = lines[index];
        if (line.type == RegionType::Title || line.type == RegionType::Author ||
            line.type == RegionType::Affiliation || line.type == RegionType::Keywords ||
            line.type == RegionType::Header || line.type == RegionType::Footer) {
            continue;
        }
        line.type = RegionType::Abstract;
        line.confidence = std::max(line.confidence, decision.inferred_without_label ? 0.90 : 0.96);
        line.locked_reason = decision.inferred_without_label
            ? "abstract_structure_inferred"
            : "abstract_structure_anchor";
    }
}

void suppress_isolated_text_noise(std::vector<LineRecord>& lines) {
    for (auto& line : lines) {
        if (line.locked_reason == "inline_formula_relation") continue;
        if (line.type != RegionType::Body && line.type != RegionType::AppendixBody && line.type != RegionType::Abstract) continue;
        const auto text = trim_copy(line.text);
        if (text.empty() || text.size() > 2) continue;
        line.type = RegionType::Other;
        line.confidence = 0.86;
        line.locked_reason = "isolated_pdf_text_noise";
    }
}

void suppress_margin_artifacts(std::vector<LineRecord>& lines, const double page_width, const double body_font) {
    for (auto& line : lines) {
        if (line.type == RegionType::Title || line.type == RegionType::Header || line.type == RegionType::Footer) continue;
        const bool left_strip = line.bbox.x < page_width * 0.055 && line.bbox.right() < page_width * 0.075;
        const bool narrow = line.bbox.width < page_width * 0.035;
        const bool display_sized = line.font_size >= body_font * 1.30;
        const bool verticalish = line.bbox.height > line.bbox.width * 1.10;
        if (left_strip && narrow && (display_sized || verticalish)) {
            line.type = RegionType::Other;
            line.confidence = 0.98;
            line.locked_reason = "left_margin_artifact";
        }
    }
}

void refine_first_page_footnotes(std::vector<LineRecord>& lines, const double page_height, const double body_font, const int page_index) {
    if (page_index != 0) return;

    for (auto& line : lines) {
        if (line.type != RegionType::Body && line.type != RegionType::Other) continue;
        const auto lower = lower_ascii(trim_copy(line.text));
        const bool lower_half = line.bbox.y > page_height * 0.52;
        const bool clearly_smaller = line.font_size <= body_font * 0.88;
        const bool footnote_anchor = contains_any(lower, {
            "corresponding author", "supported in part", "is with the school", "are with the school",
            "is with the department", "are with the department", "e-mail:", "email:", "@"
        });
        if ((lower_half && clearly_smaller) || footnote_anchor) {
            line.type = RegionType::Footnote;
            line.confidence = 0.96;
            line.locked_reason = footnote_anchor ? "first_page_author_footnote_anchor" : "first_page_small_footnote";
        }
    }

    for (int pass = 0; pass < 2; ++pass) {
        for (auto& line : lines) {
            if (line.type != RegionType::Body || line.bbox.y <= page_height * 0.52 || line.font_size > body_font * 0.92) continue;
            for (const auto& footnote : lines) {
                if (footnote.type != RegionType::Footnote || footnote.column != line.column) continue;
                const double gap = std::max(footnote.bbox.y - line.bbox.bottom(), line.bbox.y - footnote.bbox.bottom());
                if (gap >= -body_font * 0.4 && gap <= body_font * 1.6) {
                    line.type = RegionType::Footnote;
                    line.confidence = 0.92;
                    line.locked_reason = "first_page_footnote_cluster";
                    break;
                }
            }
        }
    }
}

void extend_keywords(std::vector<LineRecord>& lines, const double body_font) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].type != RegionType::Keywords) continue;
        auto previous = i;
        for (std::size_t step = 0; step < 3; ++step) {
            std::size_t best = lines.size();
            double best_gap = std::numeric_limits<double>::max();
            for (std::size_t j = 0; j < lines.size(); ++j) {
                if (j == previous || lines[j].type != RegionType::Body) continue;
                if (lines[j].column != lines[previous].column) continue;
                const double gap = lines[j].bbox.y - lines[previous].bbox.bottom();
                if (gap < -0.5 || gap > body_font * 1.65) continue;
                if (horizontal_overlap_ratio(lines[j].bbox, lines[previous].bbox) < 0.20 &&
                    std::abs(lines[j].bbox.x - lines[previous].bbox.x) > body_font * 2.0) continue;
                if (gap < best_gap) { best_gap = gap; best = j; }
            }
            if (best == lines.size()) break;
            lines[best].type = RegionType::Keywords;
            lines[best].confidence = 0.90;
            previous = best;
        }
    }
}

void extend_locked_captions(std::vector<LineRecord>& lines, const double body_font) {
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const auto caption_type = lines[i].type;
        if (caption_type != RegionType::FigureCaption && caption_type != RegionType::TableCaption) continue;

        const int max_steps = caption_type == RegionType::TableCaption ? 3 : 6;
        const std::size_t min_chars = caption_type == RegionType::TableCaption ? 32 : 14;
        auto previous = i;

        std::size_t horizontal = lines.size();
        double smallest_horizontal_gap = std::numeric_limits<double>::max();
        const double anchor_center_y = lines[i].bbox.y + lines[i].bbox.height * 0.5;
        for (std::size_t j = 0; j < lines.size(); ++j) {
            if (j == i || lines[j].column != lines[i].column) continue;
            if (lines[j].type != RegionType::Body && lines[j].type != RegionType::AppendixBody && lines[j].type != RegionType::Other && lines[j].type != RegionType::Footnote) continue;
            const double center_y = lines[j].bbox.y + lines[j].bbox.height * 0.5;
            if (std::abs(center_y - anchor_center_y) > 1.8) continue;
            const double gap = lines[j].bbox.x - lines[i].bbox.right();
            if (gap < -1.0 || gap > body_font * 4.0) continue;
            if (std::abs(lines[j].font_size - lines[i].font_size) > std::max(0.8, body_font * 0.12)) continue;
            if (lines[j].text.size() < min_chars) continue;
            if (gap < smallest_horizontal_gap) {
                smallest_horizontal_gap = gap;
                horizontal = j;
            }
        }
        if (horizontal != lines.size()) {
            lines[horizontal].type = caption_type;
            lines[horizontal].confidence = 0.99;
            lines[horizontal].locked_reason = caption_type == RegionType::FigureCaption
                ? "figure_caption_same_line"
                : "table_caption_same_line";
            previous = horizontal;
        }

        for (int step = 0; step < max_steps; ++step) {
            std::size_t best = lines.size();
            double best_gap = std::numeric_limits<double>::max();
            for (std::size_t j = 0; j < lines.size(); ++j) {
                if (j == previous || lines[j].column != lines[previous].column) continue;
                if (lines[j].type != RegionType::Body && lines[j].type != RegionType::AppendixBody && lines[j].type != RegionType::Other && lines[j].type != RegionType::Footnote) continue;
                const double gap = lines[j].bbox.y - lines[previous].bbox.bottom();
                if (gap < -0.5 || gap > std::max(5.0, body_font * 1.25)) continue;
                if (lines[j].text.size() < min_chars) continue;
                const int letters = static_cast<int>(std::count_if(lines[j].text.begin(), lines[j].text.end(), [](const unsigned char c) {
                    return c < 128 && std::isalpha(c);
                }));
                if (letters < 8) continue;
                if (std::abs(lines[j].font_size - lines[i].font_size) > std::max(0.8, body_font * 0.12)) continue;
                if (horizontal_overlap_ratio(lines[j].bbox, lines[previous].bbox) < 0.12 &&
                    std::abs(lines[j].bbox.x - lines[previous].bbox.x) > body_font * 3.0) continue;

                const double center_y = lines[j].bbox.y + lines[j].bbox.height * 0.5;
                bool has_same_row_peer = false;
                for (std::size_t k = 0; k < lines.size(); ++k) {
                    if (k == j || lines[k].column != lines[j].column) continue;
                    const double peer_center = lines[k].bbox.y + lines[k].bbox.height * 0.5;
                    if (std::abs(peer_center - center_y) <= 1.8 && horizontal_overlap_ratio(lines[k].bbox, lines[j].bbox) < 0.25) {
                        has_same_row_peer = true;
                        break;
                    }
                }
                if (has_same_row_peer) continue;
                if (gap < best_gap) { best_gap = gap; best = j; }
            }
            if (best == lines.size()) break;
            lines[best].type = caption_type;
            lines[best].confidence = 0.97;
            lines[best].locked_reason = caption_type == RegionType::FigureCaption
                ? "figure_caption_continuation"
                : "table_caption_continuation";
            previous = best;
        }
    }
}

bool looks_like_equation_number_text(const std::string& text) {
    static const std::regex equation_number(R"(^\s*[\(\[]?\d+[A-Za-z]?[\)\]]?\s*$)");
    return text.size() <= 10 && std::regex_match(text, equation_number);
}

bool formula_columns_compatible(const LineRecord& a, const LineRecord& b) {
    return a.column == b.column || a.column < 0 || b.column < 0;
}

double horizontal_gap_between(const Rect& a, const Rect& b) {
    return std::max({0.0, b.x - a.right(), a.x - b.right()});
}

double vertical_gap_between(const Rect& a, const Rect& b) {
    return std::max({0.0, b.y - a.bottom(), a.y - b.bottom()});
}

double vertical_overlap_fraction(const Rect& a, const Rect& b) {
    const double overlap = std::max(0.0, std::min(a.bottom(), b.bottom()) - std::max(a.y, b.y));
    return overlap / std::max(1.0, std::min(a.height, b.height));
}

void coalesce_display_formulas(std::vector<LineRecord>& lines, const double page_width, const double body_font) {
    if (lines.empty()) return;
    std::vector<int> group_for(lines.size(), -1);
    std::vector<std::vector<std::size_t>> groups;

    for (std::size_t seed = 0; seed < lines.size(); ++seed) {
        if (lines[seed].type != RegionType::FormulaDisplay || group_for[seed] >= 0) continue;
        const int group_id = static_cast<int>(groups.size());
        groups.push_back({seed});
        group_for[seed] = group_id;
        Rect box = lines[seed].bbox;
        bool changed = true;
        while (changed) {
            changed = false;
            for (std::size_t j = 0; j < lines.size(); ++j) {
                if (group_for[j] >= 0) continue;
                const auto type = lines[j].type;
                if (type == RegionType::Header || type == RegionType::Footer || type == RegionType::Reference ||
                    type == RegionType::FigureCaption || type == RegionType::TableCaption || type == RegionType::Footnote ||
                    type == RegionType::Figure || type == RegionType::Table) continue;
                if (!formula_columns_compatible(lines[seed], lines[j])) continue;
                const bool equation_number = looks_like_equation_number_text(lines[j].text);
                if (type != RegionType::FormulaDisplay && !equation_number) continue;

                const double v_overlap = vertical_overlap_fraction(box, lines[j].bbox);
                const double v_gap = vertical_gap_between(box, lines[j].bbox);
                const double h_gap = horizontal_gap_between(box, lines[j].bbox);
                const bool same_row = v_overlap >= 0.14 && h_gap <= page_width * 0.26;
                const bool stacked = v_gap <= std::max(6.0, body_font * 1.35) && h_gap <= body_font * 7.0;
                const bool number_at_edge = equation_number && (
                    (v_overlap >= 0.08 && h_gap <= page_width * 0.42) ||
                    (v_gap <= std::max(6.0, body_font * 1.10) && h_gap <= body_font * 2.5));
                const bool related = type == RegionType::FormulaDisplay
                    ? (same_row || stacked)
                    : number_at_edge;
                if (!related) continue;

                group_for[j] = group_id;
                groups.back().push_back(j);
                box = union_rect(box, lines[j].bbox);
                changed = true;
            }
        }
    }

    if (groups.empty()) return;
    std::vector<LineRecord> rebuilt;
    rebuilt.reserve(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const int group_id = group_for[i];
        if (group_id < 0) {
            rebuilt.push_back(lines[i]);
            continue;
        }
        const auto& group = groups[static_cast<std::size_t>(group_id)];
        if (i != *std::min_element(group.begin(), group.end())) continue;

        std::vector<std::size_t> ordered = group;
        std::sort(ordered.begin(), ordered.end(), [&](const std::size_t a, const std::size_t b) {
            if (std::abs(lines[a].bbox.y - lines[b].bbox.y) > 1.5) return lines[a].bbox.y < lines[b].bbox.y;
            return lines[a].bbox.x < lines[b].bbox.x;
        });
        LineRecord merged = lines[ordered.front()];
        merged.text.clear();
        merged.bbox = {};
        merged.type = RegionType::FormulaDisplay;
        merged.confidence = 0.98;
        merged.locked_reason = "display_formula_group";
        merged.math_font_ratio = 0.0;
        bool same_column = true;
        const int first_column = lines[ordered.front()].column;
        for (const auto index : ordered) {
            if (!merged.text.empty()) merged.text.push_back(' ');
            merged.text += lines[index].text;
            merged.bbox = union_rect(merged.bbox, lines[index].bbox);
            merged.math_font_ratio = std::max(merged.math_font_ratio, lines[index].math_font_ratio);
            if (lines[index].column != first_column) same_column = false;
        }
        merged.column = same_column ? first_column : -1;
        merged.span_mode = same_column ? lines[ordered.front()].span_mode : "span";
        rebuilt.push_back(std::move(merged));
    }
    std::sort(rebuilt.begin(), rebuilt.end(), [](const LineRecord& a, const LineRecord& b) {
        if (std::abs(a.bbox.y - b.bbox.y) > 1.5) return a.bbox.y < b.bbox.y;
        return a.bbox.x < b.bbox.x;
    });
    lines = std::move(rebuilt);
}

bool is_reference_heading_text(const std::string& text) {
    const auto lower = lower_ascii(trim_copy(text));
    return lower == "references" || lower == "reference";
}

bool is_explicit_appendix_heading_text(const std::string& text) {
    const auto lower = lower_ascii(trim_copy(text));
    return lower == "appendix" || lower == "appendices" || starts_with_any(lower, {
        "appendix ", "appendix:", "supplementary material", "supplemental material", "supplementary", "supplemental"
    });
}

bool looks_like_appendix_anchor(const LineRecord& line, const double page_width, const double body_font, const bool allow_lettered) {
    if (is_explicit_appendix_heading_text(line.text)) return true;
    if (!allow_lettered || line.text.size() > 72) return false;
    static const std::regex lettered_appendix(R"(^\s*[A-Z](?:\s*\.\s*\d+)*[\.)]?\s+[A-Z])");
    if (!std::regex_search(line.text, lettered_appendix)) return false;
    const auto lower = lower_ascii(line.text);
    if (!contains_any(lower, {"proof", "proofs", "additional", "experiment", "experiments", "implementation", "derivation", "derivations", "ablation", "ablations", "details", "analysis", "analyses", "theorem", "theorems", "lemma", "lemmas", "dataset", "datasets", "training", "results", "discussion"})) return false;
    if (line.text.find(',') != std::string::npos || line.text.find('[') != std::string::npos || line.text.find('"') != std::string::npos ||
        contains_any(lower, {"et al", "proceedings", "transactions", "journal", "conference"})) return false;
    const double center = line.bbox.x + line.bbox.width * 0.5;
    const bool centered = std::abs(center - page_width * 0.5) < page_width * 0.18;
    const bool compact_heading_shape = line.bbox.width <= page_width * 0.34;
    return (line.type == RegionType::Heading && compact_heading_shape) ||
        line.font_size >= body_font * 1.10 || (centered && compact_heading_shape);
}

bool looks_like_appendix_subheading(const LineRecord& line) {
    static const std::regex appendix_subheading(R"(^\s*[A-Z](?:\s*\.\s*\d+)+[\.)]?\s+[A-Z])");
    return line.text.size() < 120 && std::regex_search(line.text, appendix_subheading);
}

void classify_as_appendix(LineRecord& line, const double page_width, const double body_font) {
    if (line.type == RegionType::Header || line.type == RegionType::Footer || line.type == RegionType::FigureCaption ||
        line.type == RegionType::TableCaption || line.type == RegionType::FormulaDisplay || line.type == RegionType::Algorithm ||
        line.type == RegionType::Code || line.type == RegionType::Footnote) return;
    if (looks_like_appendix_anchor(line, page_width, body_font, true) || line.type == RegionType::Heading || looks_like_appendix_subheading(line)) {
        line.type = RegionType::AppendixHeading;
        line.confidence = std::max(line.confidence, 0.95);
        line.locked_reason.clear();
        return;
    }
    if (line.type == RegionType::Other && trim_copy(line.text).size() <= 3) return;
    line.type = RegionType::AppendixBody;
    line.confidence = std::max(line.confidence, 0.88);
    line.locked_reason.clear();
}

void apply_document_section_state(
    std::vector<LineRecord>& lines,
    const double page_width,
    const double body_font,
    bool& references_started,
    bool& appendix_started) {
    std::vector<std::size_t> order(lines.size());
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](const std::size_t a, const std::size_t b) {
        if (std::abs(lines[a].bbox.y - lines[b].bbox.y) > 1.5) return lines[a].bbox.y < lines[b].bbox.y;
        return lines[a].bbox.x < lines[b].bbox.x;
    });

    const bool inherited_references = references_started;
    const bool inherited_appendix = appendix_started;
    std::optional<std::size_t> reference_pos;
    std::optional<std::size_t> appendix_pos;
    bool reference_seen = inherited_references;
    for (std::size_t pos = 0; pos < order.size(); ++pos) {
        const auto& line = lines[order[pos]];
        if (!reference_pos && is_reference_heading_text(line.text)) {
            reference_pos = pos;
            reference_seen = true;
        }
        if (!appendix_pos && looks_like_appendix_anchor(line, page_width, body_font, reference_seen || inherited_appendix)) {
            appendix_pos = pos;
        }
    }

    if (inherited_appendix) {
        if (reference_pos) {
            for (std::size_t pos = 0; pos < order.size(); ++pos) {
                auto& line = lines[order[pos]];
                if (pos < *reference_pos) {
                    classify_as_appendix(line, page_width, body_font);
                } else if (line.type != RegionType::Header && line.type != RegionType::Footer) {
                    line.type = RegionType::Reference;
                    line.confidence = 0.99;
                    line.locked_reason = pos == *reference_pos ? "references_heading" : "references_section";
                }
            }
            appendix_started = false;
            references_started = true;
        } else {
            for (auto& line : lines) classify_as_appendix(line, page_width, body_font);
        }
        return;
    }

    if (inherited_references) {
        if (appendix_pos) {
            for (std::size_t pos = 0; pos < order.size(); ++pos) {
                auto& line = lines[order[pos]];
                if (pos < *appendix_pos) {
                    if (line.type == RegionType::Header || line.type == RegionType::Footer) continue;
                    line.type = RegionType::Reference;
                    line.confidence = 0.99;
                    line.locked_reason = "references_section_continuation";
                } else {
                    classify_as_appendix(line, page_width, body_font);
                }
            }
            references_started = false;
            appendix_started = true;
        } else {
            for (auto& line : lines) {
                if (line.type == RegionType::Header || line.type == RegionType::Footer) continue;
                line.type = RegionType::Reference;
                line.confidence = 0.99;
                line.locked_reason = "references_section_continuation";
            }
        }
        return;
    }

    if (appendix_pos && (!reference_pos || *appendix_pos < *reference_pos)) {
        for (std::size_t pos = *appendix_pos; pos < order.size(); ++pos) {
            if (reference_pos && pos >= *reference_pos) break;
            classify_as_appendix(lines[order[pos]], page_width, body_font);
        }
        appendix_started = !reference_pos;
        if (reference_pos) {
            for (std::size_t pos = *reference_pos; pos < order.size(); ++pos) {
                auto& line = lines[order[pos]];
                if (line.type == RegionType::Header || line.type == RegionType::Footer) continue;
                line.type = RegionType::Reference;
                line.confidence = 0.99;
                line.locked_reason = pos == *reference_pos ? "references_heading" : "references_section";
            }
            references_started = true;
        }
        return;
    }

    if (reference_pos) {
        for (std::size_t pos = *reference_pos; pos < order.size(); ++pos) {
            if (appendix_pos && pos >= *appendix_pos) break;
            auto& line = lines[order[pos]];
            if (line.type == RegionType::Header || line.type == RegionType::Footer) continue;
            line.type = RegionType::Reference;
            line.confidence = 0.99;
            line.locked_reason = pos == *reference_pos ? "references_heading" : "references_section";
        }
        references_started = true;
        if (appendix_pos) {
            for (std::size_t pos = *appendix_pos; pos < order.size(); ++pos) classify_as_appendix(lines[order[pos]], page_width, body_font);
            references_started = false;
            appendix_started = true;
        }
    }
}

std::vector<std::size_t> reading_order_indices(
    const std::vector<LineRecord>& lines,
    const int columns,
    const std::vector<VisualCandidate>& visual_locked) {
    std::vector<std::size_t> result;
    result.reserve(lines.size());
    if (columns == 1) {
        for (std::size_t i = 0; i < lines.size(); ++i) result.push_back(i);
        std::sort(result.begin(), result.end(), [&](const std::size_t a, const std::size_t b) {
            if (std::abs(lines[a].bbox.y - lines[b].bbox.y) > 1.5) return lines[a].bbox.y < lines[b].bbox.y;
            return lines[a].bbox.x < lines[b].bbox.x;
        });
        return result;
    }

    struct SeparatorBand {
        double top{};
        double bottom{};
        std::vector<std::size_t> span_lines;
    };
    std::vector<SeparatorBand> bands;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].column < 0) bands.push_back({lines[i].bbox.y, lines[i].bbox.bottom(), {i}});
    }
    for (const auto& visual : visual_locked) {
        if (visual.span_mode == "span") bands.push_back({visual.bbox.y, visual.bbox.bottom(), {}});
    }
    std::sort(bands.begin(), bands.end(), [](const SeparatorBand& a, const SeparatorBand& b) { return a.top < b.top; });
    std::vector<SeparatorBand> merged;
    for (auto& band : bands) {
        if (!merged.empty() && band.top <= merged.back().bottom + 2.0) {
            merged.back().bottom = std::max(merged.back().bottom, band.bottom);
            merged.back().span_lines.insert(merged.back().span_lines.end(), band.span_lines.begin(), band.span_lines.end());
        } else {
            merged.push_back(std::move(band));
        }
    }

    double segment_top = -1.0;
    auto append_segment = [&](const double bottom) {
        for (int col = 0; col < 2; ++col) {
            std::vector<std::size_t> segment;
            for (std::size_t i = 0; i < lines.size(); ++i) {
                if (lines[i].column != col) continue;
                if (lines[i].bbox.y >= segment_top && lines[i].bbox.y < bottom) segment.push_back(i);
            }
            std::sort(segment.begin(), segment.end(), [&](const std::size_t a, const std::size_t b) {
                if (std::abs(lines[a].bbox.y - lines[b].bbox.y) > 1.5) return lines[a].bbox.y < lines[b].bbox.y;
                return lines[a].bbox.x < lines[b].bbox.x;
            });
            result.insert(result.end(), segment.begin(), segment.end());
        }
    };
    for (auto& band : merged) {
        append_segment(band.top);
        std::sort(band.span_lines.begin(), band.span_lines.end(), [&](const std::size_t a, const std::size_t b) {
            if (std::abs(lines[a].bbox.y - lines[b].bbox.y) > 1.5) return lines[a].bbox.y < lines[b].bbox.y;
            return lines[a].bbox.x < lines[b].bbox.x;
        });
        result.insert(result.end(), band.span_lines.begin(), band.span_lines.end());
        segment_top = std::max(segment_top, band.bottom);
    }
    append_segment(std::numeric_limits<double>::max());
    return result;
}

Raster render_page(detail::PdfiumRuntime& api, detail::FPDF_PAGE page, const double page_width, const double page_height, const double scale) {
    Raster raster;
    raster.width = std::max(1, static_cast<int>(std::lround(page_width * scale)));
    raster.height = std::max(1, static_cast<int>(std::lround(page_height * scale)));
    auto bitmap = api.BitmapCreate(raster.width, raster.height, 1);
    if (bitmap == nullptr) throw std::runtime_error("FPDFBitmap_Create failed.");
    api.BitmapFillRect(bitmap, 0, 0, raster.width, raster.height, 0xFFFFFFFFu);
    api.RenderPageBitmap(bitmap, page, 0, 0, raster.width, raster.height, 0, kRenderAnnotations | kRenderLcdText);
    raster.stride = api.BitmapGetStride(bitmap);
    const auto* source = static_cast<const std::uint8_t*>(api.BitmapGetBuffer(bitmap));
    if (source == nullptr || raster.stride <= 0) {
        api.BitmapDestroy(bitmap);
        throw std::runtime_error("PDFium returned an invalid render buffer.");
    }
    raster.bgra.assign(source, source + static_cast<std::size_t>(raster.stride) * raster.height);
    api.BitmapDestroy(bitmap);
    return raster;
}

double rect_area(const Rect& rect) {
    return std::max(0.0, rect.width) * std::max(0.0, rect.height);
}

double intersection_area(const Rect& a, const Rect& b) {
    const double left = std::max(a.x, b.x);
    const double top = std::max(a.y, b.y);
    const double right = std::min(a.right(), b.right());
    const double bottom = std::min(a.bottom(), b.bottom());
    return std::max(0.0, right - left) * std::max(0.0, bottom - top);
}

double horizontal_overlap_ratio(const Rect& a, const Rect& b) {
    const double overlap = std::max(0.0, std::min(a.right(), b.right()) - std::max(a.x, b.x));
    return overlap / std::max(1.0, std::min(a.width, b.width));
}

double vertical_overlap_ratio(const Rect& a, const Rect& b) {
    const double overlap = std::max(0.0, std::min(a.bottom(), b.bottom()) - std::max(a.y, b.y));
    return overlap / std::max(1.0, std::min(a.height, b.height));
}

bool near_or_overlapping(const Rect& a, const Rect& b, const double gap) {
    const double horizontal_gap = std::max({0.0, b.x - a.right(), a.x - b.right()});
    const double vertical_gap = std::max({0.0, b.y - a.bottom(), a.y - b.bottom()});
    return intersection_area(a, b) > 0.0 ||
        (horizontal_gap <= gap && vertical_overlap_ratio(a, b) > 0.35) ||
        (vertical_gap <= gap && horizontal_overlap_ratio(a, b) > 0.35);
}

std::vector<VisualCandidate> detect_visual_locked_regions(
    const Raster& raster,
    const std::vector<LineRecord>& lines,
    const double page_width,
    const double page_height,
    const double scale,
    const ColumnLayout& layout) {
    if (raster.width <= 0 || raster.height <= 0 || raster.bgra.empty()) return {};

    std::vector<std::uint8_t> text_mask(static_cast<std::size_t>(raster.width) * raster.height, 0);
    for (const auto& line : lines) {
        const int pad = std::max(2, static_cast<int>(std::lround(scale * 1.2)));
        const int left = std::clamp(static_cast<int>(std::floor(line.bbox.x * scale)) - pad, 0, raster.width - 1);
        const int top = std::clamp(static_cast<int>(std::floor(line.bbox.y * scale)) - pad, 0, raster.height - 1);
        const int right = std::clamp(static_cast<int>(std::ceil(line.bbox.right() * scale)) + pad, 0, raster.width - 1);
        const int bottom = std::clamp(static_cast<int>(std::ceil(line.bbox.bottom() * scale)) + pad, 0, raster.height - 1);
        for (int y = top; y <= bottom; ++y) {
            auto* row = text_mask.data() + static_cast<std::size_t>(y) * raster.width;
            std::fill(row + left, row + right + 1, static_cast<std::uint8_t>(1));
        }
    }

    constexpr int tile = 16;
    const int grid_w = (raster.width + tile - 1) / tile;
    const int grid_h = (raster.height + tile - 1) / tile;
    struct TileStat { bool active{}; int dark{}; int available{}; };
    std::vector<TileStat> stats(static_cast<std::size_t>(grid_w) * grid_h);
    for (int gy = 0; gy < grid_h; ++gy) {
        for (int gx = 0; gx < grid_w; ++gx) {
            auto& stat = stats[static_cast<std::size_t>(gy) * grid_w + gx];
            const int x0 = gx * tile;
            const int y0 = gy * tile;
            const int x1 = std::min(raster.width, x0 + tile);
            const int y1 = std::min(raster.height, y0 + tile);
            for (int y = y0; y < y1; ++y) {
                const auto* row = raster.bgra.data() + static_cast<std::size_t>(y) * raster.stride;
                const auto* mask_row = text_mask.data() + static_cast<std::size_t>(y) * raster.width;
                for (int x = x0; x < x1; ++x) {
                    if (mask_row[x]) continue;
                    ++stat.available;
                    const auto* px = row + static_cast<std::size_t>(x) * 4;
                    const int luminance = (static_cast<int>(px[2]) * 299 + static_cast<int>(px[1]) * 587 + static_cast<int>(px[0]) * 114) / 1000;
                    if (luminance < 225) ++stat.dark;
                }
            }
            const double density = stat.available > 0 ? static_cast<double>(stat.dark) / stat.available : 0.0;
            stat.active = stat.dark >= 5 && density >= 0.025;
        }
    }

    std::vector<std::uint8_t> visited(stats.size(), 0);
    std::vector<VisualCandidate> raw;
    constexpr std::array<int, 8> dx = {-1,0,1,-1,1,-1,0,1};
    constexpr std::array<int, 8> dy = {-1,-1,-1,0,0,1,1,1};
    for (int sy = 0; sy < grid_h; ++sy) {
        for (int sx = 0; sx < grid_w; ++sx) {
            const auto seed = static_cast<std::size_t>(sy) * grid_w + sx;
            if (visited[seed] || !stats[seed].active) continue;
            visited[seed] = 1;
            std::vector<std::pair<int,int>> stack{{sx, sy}};
            int min_x = sx, max_x = sx, min_y = sy, max_y = sy;
            int tile_count = 0, dark = 0, available = 0;
            while (!stack.empty()) {
                const auto [x, y] = stack.back();
                stack.pop_back();
                const auto index = static_cast<std::size_t>(y) * grid_w + x;
                ++tile_count;
                dark += stats[index].dark;
                available += stats[index].available;
                min_x = std::min(min_x, x); max_x = std::max(max_x, x);
                min_y = std::min(min_y, y); max_y = std::max(max_y, y);
                for (std::size_t n = 0; n < dx.size(); ++n) {
                    const int nx = x + dx[n], ny = y + dy[n];
                    if (nx < 0 || ny < 0 || nx >= grid_w || ny >= grid_h) continue;
                    const auto ni = static_cast<std::size_t>(ny) * grid_w + nx;
                    if (visited[ni] || !stats[ni].active) continue;
                    visited[ni] = 1;
                    stack.emplace_back(nx, ny);
                }
            }
            if (tile_count < 4) continue;
            Rect bbox{
                min_x * tile / scale,
                min_y * tile / scale,
                std::min(raster.width, (max_x + 1) * tile) / scale - min_x * tile / scale,
                std::min(raster.height, (max_y + 1) * tile) / scale - min_y * tile / scale,
            };
            if (bbox.width < page_width * 0.12 || bbox.height < page_height * 0.035 || bbox.y < page_height * 0.025 || bbox.bottom() > page_height * 0.975) continue;
            const double area_ratio = rect_area(bbox) / std::max(1.0, page_width * page_height);
            const double residual_density = available > 0 ? static_cast<double>(dark) / available : 0.0;

            RegionType anchored_type = RegionType::Other;
            double anchor_confidence = 0.0;
            for (const auto& line : lines) {
                if (horizontal_overlap_ratio(bbox, line.bbox) < 0.35) continue;
                if (line.type == RegionType::FigureCaption) {
                    const double gap = line.bbox.y - bbox.bottom();
                    if (gap >= -5.0 && gap <= 65.0) { anchored_type = RegionType::Figure; anchor_confidence = 0.97; break; }
                }
                if (line.type == RegionType::TableCaption) {
                    const double gap = bbox.y - line.bbox.bottom();
                    if (gap >= -5.0 && gap <= 60.0) { anchored_type = RegionType::Table; anchor_confidence = 0.97; break; }
                }
            }

            if (anchored_type == RegionType::Other && (area_ratio < 0.012 || residual_density < 0.075)) continue;
            VisualCandidate candidate;
            candidate.bbox = bbox;
            candidate.type = anchored_type == RegionType::Other ? RegionType::Figure : anchored_type;
            candidate.confidence = anchored_type == RegionType::Other ? std::clamp(0.68 + residual_density * 0.8 + area_ratio * 2.0, 0.68, 0.90) : anchor_confidence;
            candidate.reason = anchored_type == RegionType::Figure ? "visual_residual_near_figure_caption" :
                anchored_type == RegionType::Table ? "visual_residual_near_table_caption" : "visual_residual_component";
            raw.push_back(std::move(candidate));
        }
    }

    std::vector<VisualCandidate> merged;
    for (auto candidate : raw) {
        bool did_merge = false;
        for (auto& existing : merged) {
            if (existing.type == candidate.type && near_or_overlapping(existing.bbox, candidate.bbox, 10.0)) {
                existing.bbox = union_rect(existing.bbox, candidate.bbox);
                existing.confidence = std::max(existing.confidence, candidate.confidence);
                if (candidate.reason.find("caption") != std::string::npos) existing.reason = candidate.reason;
                did_merge = true;
                break;
            }
        }
        if (!did_merge) merged.push_back(std::move(candidate));
    }

    for (auto& candidate : merged) {
        const double pad = 5.0;
        candidate.bbox.x = std::max(0.0, candidate.bbox.x - pad);
        candidate.bbox.y = std::max(0.0, candidate.bbox.y - pad);
        candidate.bbox.width = std::min(page_width - candidate.bbox.x, candidate.bbox.width + pad * 2.0);
        candidate.bbox.height = std::min(page_height - candidate.bbox.y, candidate.bbox.height + pad * 2.0);
        const double center = candidate.bbox.x + candidate.bbox.width * 0.5;
        if (layout.count == 2 && candidate.bbox.x < layout.gutter_left && candidate.bbox.right() > layout.gutter_right) {
            candidate.column = -1;
            candidate.span_mode = "span";
        } else if (layout.count == 2) {
            candidate.column = center < layout.gutter_x ? 0 : 1;
            candidate.span_mode = "column";
        } else {
            candidate.column = 0;
            candidate.span_mode = "single";
        }
    }
    return merged;
}

std::vector<VisualCandidate> detect_table_locked_regions(
    const std::vector<LineRecord>& lines,
    const double page_width,
    const double page_height,
    const double body_font,
    const ColumnLayout& layout) {
    std::vector<VisualCandidate> tables;
    for (const auto& caption : lines) {
        if (caption.type != RegionType::TableCaption) continue;

        double max_bottom = std::min(page_height * 0.96, caption.bbox.y + page_height * 0.38);
        for (const auto& other : lines) {
            if (&other == &caption || other.type != RegionType::TableCaption) continue;
            if (other.column != caption.column || other.bbox.y <= caption.bbox.y + 4.0) continue;
            max_bottom = std::min(max_bottom, other.bbox.y - body_font * 0.8);
        }
        Rect box = caption.bbox;
        int included = 0;
        int distinct_rows = 0;
        double last_row_y = caption.bbox.bottom();
        bool started = false;

        std::vector<const LineRecord*> candidates;
        for (const auto& line : lines) {
            if (&line == &caption || line.bbox.y <= caption.bbox.bottom() || line.bbox.y >= max_bottom) continue;
            if (line.type == RegionType::Header || line.type == RegionType::Footer || line.type == RegionType::FigureCaption || line.type == RegionType::TableCaption) continue;
            if (caption.column >= 0 && line.column != caption.column) continue;
            candidates.push_back(&line);
        }
        std::sort(candidates.begin(), candidates.end(), [](const LineRecord* a, const LineRecord* b) {
            if (std::abs(a->bbox.y - b->bbox.y) > 1.5) return a->bbox.y < b->bbox.y;
            return a->bbox.x < b->bbox.x;
        });

        for (const auto* line : candidates) {
            const double gap = line->bbox.y - last_row_y;
            const bool table_sized = line->font_size <= body_font * 0.92 || line->bbox.height <= body_font * 0.86;
            const bool compact = line->text.size() <= 42 || line->bbox.width <= page_width * 0.24;
            const bool prose_like = prose_word_count(line->text) >= 8 && line->text.size() >= 60;
            if (started && included >= 6 && prose_like && !table_sized) break;
            if (!started) {
                if (line->bbox.y - caption.bbox.bottom() > body_font * 5.5) break;
                if (!table_sized && !compact) continue;
                started = true;
            } else if (gap > body_font * 3.2) {
                break;
            }
            if (!table_sized && !compact && gap > body_font * 1.8) break;

            if (std::abs(line->bbox.y - last_row_y) > body_font * 0.35) ++distinct_rows;
            last_row_y = std::max(last_row_y, line->bbox.bottom());
            box = union_rect(box, line->bbox);
            ++included;
        }

        if (included < 7 || distinct_rows < 3 || box.height < body_font * 4.5) continue;

        const double pad = 4.0;
        box.x = std::max(0.0, box.x - pad);
        box.y = std::max(0.0, box.y - pad);
        box.width = std::min(page_width - box.x, box.width + pad * 2.0);
        box.height = std::min(page_height - box.y, box.height + pad * 2.0);
        if (layout.count == 2 && caption.column == 0 && layout.gutter_left > 0.0) {
            const double right = std::min(box.right(), layout.gutter_left - 2.0);
            box.width = std::max(1.0, right - box.x);
        } else if (layout.count == 2 && caption.column == 1 && layout.gutter_right > 0.0) {
            const double right = box.right();
            box.x = std::max(box.x, layout.gutter_right + 2.0);
            box.width = std::max(1.0, std::min(page_width, right) - box.x);
        }

        VisualCandidate candidate;
        candidate.bbox = box;
        candidate.type = RegionType::Table;
        candidate.confidence = 0.96;
        candidate.reason = "table_caption_compact_grid";
        candidate.source_kind = "pdf_text_grid";
        candidate.column = caption.column;
        candidate.span_mode = caption.column < 0 ? "span" : (layout.count == 1 ? "single" : "column");
        tables.push_back(std::move(candidate));
    }
    return tables;
}

void append_visual_candidates(std::vector<VisualCandidate>& target, std::vector<VisualCandidate> additions) {
    for (auto& candidate : additions) {
        bool merged = false;
        for (auto& existing : target) {
            if (existing.type != candidate.type) continue;
            const double overlap = intersection_area(existing.bbox, candidate.bbox);
            const double smaller = std::max(1.0, std::min(rect_area(existing.bbox), rect_area(candidate.bbox)));
            if (overlap / smaller < 0.45 && !near_or_overlapping(existing.bbox, candidate.bbox, 4.0)) continue;
            existing.bbox = union_rect(existing.bbox, candidate.bbox);
            if (candidate.confidence > existing.confidence) {
                existing.confidence = candidate.confidence;
                existing.reason = candidate.reason;
                existing.source_kind = candidate.source_kind;
            }
            merged = true;
            break;
        }
        if (!merged) target.push_back(std::move(candidate));
    }
}

void apply_visual_locked_regions(std::vector<LineRecord>& lines, const std::vector<VisualCandidate>& candidates) {
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const auto& candidate = candidates[index];
        for (auto& line : lines) {
            if (line.type == RegionType::FigureCaption || line.type == RegionType::TableCaption || line.type == RegionType::Header || line.type == RegionType::Footer || line.type == RegionType::Reference) continue;
            const double line_area = rect_area(line.bbox);
            const double overlap = line_area > 0.0 ? intersection_area(line.bbox, candidate.bbox) / line_area : 0.0;
            const double center_x = line.bbox.x + line.bbox.width * 0.5;
            const double center_y = line.bbox.y + line.bbox.height * 0.5;
            const bool center_inside = center_x >= candidate.bbox.x && center_x <= candidate.bbox.right() && center_y >= candidate.bbox.y && center_y <= candidate.bbox.bottom();
            if (!center_inside && overlap < 0.60) continue;
            line.type = candidate.type;
            line.confidence = std::max(line.confidence, candidate.confidence);
            line.locked_reason = candidate.reason;
            line.parent_locked_index = static_cast<int>(index);
            line.column = candidate.column;
            line.span_mode = candidate.span_mode;
        }
    }
}

bool block_singleton_type(const RegionType type) {
    return type == RegionType::FormulaDisplay || type == RegionType::Algorithm || type == RegionType::Code;
}

std::vector<BlockRecord> build_blocks(const std::vector<LineRecord>& lines, const std::vector<std::size_t>& order) {
    std::vector<BlockRecord> blocks;
    const LineRecord* previous_line = nullptr;
    for (const auto index : order) {
        const auto& line = lines[index];
        bool join = false;
        if (!blocks.empty() && previous_line != nullptr) {
            auto& current = blocks.back();
            const double gap = line.bbox.y - previous_line->bbox.bottom();
            const double max_gap = std::max(4.0, std::max(line.font_size, previous_line->font_size) * 1.45);
            const bool same_identity = current.type == line.type && current.column == line.column && current.parent_locked_index == line.parent_locked_index;
            const bool same_flow_shape = std::abs(line.bbox.x - previous_line->bbox.x) <= std::max(8.0, line.font_size * 1.45) ||
                horizontal_overlap_ratio(line.bbox, previous_line->bbox) >= 0.55;
            const bool centered_family = line.type == RegionType::Title || line.type == RegionType::Author || line.type == RegionType::Affiliation ||
                line.type == RegionType::FigureCaption || line.type == RegionType::TableCaption;
            const bool prose_body_pair = (line.type == RegionType::Body && previous_line->type == RegionType::Body) ||
                (line.type == RegionType::AppendixBody && previous_line->type == RegionType::AppendixBody);
            const bool clear_body_boundary = prose_body_pair && ends_sentence(previous_line->text) && begins_upper_ascii(line.text);
            const bool inline_formula_pair =
                (line.locked_reason == "inline_formula_relation" || previous_line->locked_reason == "inline_formula_relation") &&
                formula_columns_compatible(line, *previous_line) &&
                detail::formula_same_text_row(line.bbox, previous_line->bbox, std::max(line.font_size, previous_line->font_size));
            const bool normal_flow_pair =
                gap >= -line.font_size * 0.35 && gap <= max_gap && (same_flow_shape || centered_family);
            join = same_identity && !block_singleton_type(line.type) && !clear_body_boundary &&
                (normal_flow_pair || inline_formula_pair);
        }

        if (!join) {
            BlockRecord block;
            block.text = line.text;
            block.bbox = line.bbox;
            block.font_name = line.font_name;
            block.font_size = line.font_size;
            block.column = line.column;
            block.type = line.type;
            block.reading_order = line.reading_order;
            block.confidence = line.confidence;
            block.locked = is_locked(line.type);
            block.locked_reason = line.locked_reason.empty() && block.locked ? "semantic_nontranslatable" : line.locked_reason;
            block.parent_locked_index = line.parent_locked_index;
            block.span_mode = line.span_mode;
            blocks.push_back(std::move(block));
        } else {
            auto& block = blocks.back();
            block.text = join_line_text(block.text, line.text);
            block.bbox = union_rect(block.bbox, line.bbox);
            block.font_size = std::min(block.font_size, line.font_size);
            block.confidence = std::min(block.confidence, line.confidence);
        }
        previous_line = &line;
    }
    return blocks;
}

#if defined(_WIN32)
bool save_png(const Raster& raster, const std::filesystem::path& path, std::string& error) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool should_uninit = SUCCEEDED(init);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* properties = nullptr;
    auto cleanup = [&]() {
        if (properties) properties->Release();
        if (frame) frame->Release();
        if (encoder) encoder->Release();
        if (stream) stream->Release();
        if (factory) factory->Release();
        if (should_uninit) CoUninitialize();
    };
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory, reinterpret_cast<void**>(&factory));
    if (SUCCEEDED(hr)) hr = factory->CreateStream(&stream);
    if (SUCCEEDED(hr)) hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (SUCCEEDED(hr)) hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(&frame, &properties);
    if (SUCCEEDED(hr)) hr = frame->Initialize(properties);
    if (SUCCEEDED(hr)) hr = frame->SetSize(static_cast<UINT>(raster.width), static_cast<UINT>(raster.height));
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
    if (SUCCEEDED(hr) && format != GUID_WICPixelFormat32bppBGRA) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = frame->WritePixels(static_cast<UINT>(raster.height), static_cast<UINT>(raster.stride), static_cast<UINT>(raster.bgra.size()), const_cast<BYTE*>(raster.bgra.data()));
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = encoder->Commit();
    cleanup();
    if (FAILED(hr)) {
        std::ostringstream out;
        out << "WIC PNG encode failed (HRESULT 0x" << std::hex << static_cast<unsigned long>(hr) << ") for " << path.string();
        error = out.str();
        return false;
    }
    return true;
}
#else
bool save_png(const Raster&, const std::filesystem::path&, std::string& error) {
    error = "PNG export is currently implemented for Windows only.";
    return false;
}
#endif

std::array<std::uint8_t, 4> region_color(const RegionType type) {
    switch (type) {
        case RegionType::Title: return {210, 70, 180, 255};
        case RegionType::Heading: return {80, 100, 235, 255};
        case RegionType::Body: return {70, 180, 80, 255};
        case RegionType::AppendixHeading: return {100, 80, 220, 255};
        case RegionType::AppendixBody: return {70, 180, 80, 255};
        case RegionType::Abstract: return {220, 150, 60, 255};
        case RegionType::FormulaDisplay: return {70, 70, 220, 255};
        case RegionType::FigureCaption:
        case RegionType::TableCaption: return {210, 120, 40, 255};
        case RegionType::Reference: return {135, 135, 135, 255};
        default: return {180, 90, 180, 255};
    }
}

void set_pixel(Raster& raster, const int x, const int y, const std::array<std::uint8_t, 4>& color) {
    if (x < 0 || y < 0 || x >= raster.width || y >= raster.height) return;
    auto* px = raster.bgra.data() + static_cast<std::size_t>(y) * raster.stride + static_cast<std::size_t>(x) * 4;
    px[0] = color[0]; px[1] = color[1]; px[2] = color[2]; px[3] = color[3];
}

void draw_region_box(Raster& raster, const Rect& rect, const double scale, const RegionType type) {
    const int left = std::clamp(static_cast<int>(std::floor(rect.x * scale)), 0, raster.width - 1);
    const int top = std::clamp(static_cast<int>(std::floor(rect.y * scale)), 0, raster.height - 1);
    const int right = std::clamp(static_cast<int>(std::ceil(rect.right() * scale)), 0, raster.width - 1);
    const int bottom = std::clamp(static_cast<int>(std::ceil(rect.bottom() * scale)), 0, raster.height - 1);
    const auto color = region_color(type);
    for (int t = 0; t < 2; ++t) {
        for (int x = left; x <= right; ++x) { set_pixel(raster, x, top + t, color); set_pixel(raster, x, bottom - t, color); }
        for (int y = top; y <= bottom; ++y) { set_pixel(raster, left + t, y, color); set_pixel(raster, right - t, y, color); }
    }
}

std::array<std::uint8_t, 4> estimate_background(const Raster& raster, const int left, const int top, const int right, const int bottom) {
    struct Sample { std::uint8_t b,g,r,a; int luminance; };
    std::vector<Sample> samples;
    const int ring = 7;
    const int step = std::max(1, (right - left + bottom - top) / 120);
    auto sample = [&](const int x, const int y) {
        if (x < 0 || y < 0 || x >= raster.width || y >= raster.height) return;
        const auto* px = raster.bgra.data() + static_cast<std::size_t>(y) * raster.stride + static_cast<std::size_t>(x) * 4;
        samples.push_back({px[0], px[1], px[2], px[3], (static_cast<int>(px[2]) * 299 + static_cast<int>(px[1]) * 587 + static_cast<int>(px[0]) * 114) / 1000});
    };
    for (int x = left; x <= right; x += step) { sample(x, top - ring); sample(x, bottom + ring); }
    for (int y = top; y <= bottom; y += step) { sample(left - ring, y); sample(right + ring, y); }
    if (samples.empty()) return {255,255,255,255};
    std::sort(samples.begin(), samples.end(), [](const Sample& a, const Sample& b) { return a.luminance < b.luminance; });
    const std::size_t begin = samples.size() / 3;
    std::vector<int> bs, gs, rs, as;
    for (std::size_t i = begin; i < samples.size(); ++i) {
        bs.push_back(samples[i].b); gs.push_back(samples[i].g); rs.push_back(samples[i].r); as.push_back(samples[i].a);
    }
    auto med = [](std::vector<int>& v) -> std::uint8_t {
        if (v.empty()) return 255;
        auto mid = v.begin() + static_cast<std::ptrdiff_t>(v.size()/2);
        std::nth_element(v.begin(), mid, v.end());
        return static_cast<std::uint8_t>(*mid);
    };
    return {med(bs), med(gs), med(rs), med(as)};
}

void erase_region(Raster& raster, const Rect& rect, const double scale) {
    const int pad = std::max(2, static_cast<int>(std::lround(scale * 1.2)));
    const int left = std::clamp(static_cast<int>(std::floor(rect.x * scale)) - pad, 0, raster.width - 1);
    const int top = std::clamp(static_cast<int>(std::floor(rect.y * scale)) - pad, 0, raster.height - 1);
    const int right = std::clamp(static_cast<int>(std::ceil(rect.right() * scale)) + pad, 0, raster.width - 1);
    const int bottom = std::clamp(static_cast<int>(std::ceil(rect.bottom() * scale)) + pad, 0, raster.height - 1);
    const auto background = estimate_background(raster, left, top, right, bottom);
    for (int y = top; y <= bottom; ++y) {
        auto* row = raster.bgra.data() + static_cast<std::size_t>(y) * raster.stride;
        for (int x = left; x <= right; ++x) {
            auto* px = row + static_cast<std::size_t>(x) * 4;
            px[0]=background[0]; px[1]=background[1]; px[2]=background[2]; px[3]=255;
        }
    }
}

bool ends_sentence(const std::string& text) {
    if (text.empty()) return true;
    for (auto it = text.rbegin(); it != text.rend(); ++it) {
        if (std::isspace(static_cast<unsigned char>(*it))) continue;
        return *it == '.' || *it == '!' || *it == '?' || *it == ':' || *it == ';';
    }
    return true;
}

bool begins_lower_ascii(const std::string& text) {
    for (const unsigned char c : text) {
        if (std::isspace(c) || std::ispunct(c)) continue;
        return c < 128 && std::islower(c);
    }
    return false;
}

bool begins_upper_ascii(const std::string& text) {
    for (const unsigned char c : text) {
        if (std::isspace(c) || std::ispunct(c)) continue;
        return c < 128 && std::isupper(c);
    }
    return false;
}

std::string join_line_text(std::string current, const std::string& next) {
    if (current.empty()) return next;
    if (next.empty()) return current;
    if (current.back() == '-' && begins_lower_ascii(next)) {
        current.pop_back();
        return current + next;
    }
    if (current.back() != ' ') current.push_back(' ');
    current += next;
    return current;
}

bool ascii_word_char(const unsigned char c) {
    return c < 128 && std::isalpha(c);
}

std::string lower_word(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

std::vector<std::string> extract_ascii_words(const std::string& text) {
    std::vector<std::string> words;
    std::string current;
    for (const unsigned char c : text) {
        if (ascii_word_char(c)) {
            current.push_back(static_cast<char>(std::tolower(c)));
        } else if (!current.empty()) {
            words.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) words.push_back(std::move(current));
    return words;
}

std::string normalize_surface_spacing(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    bool pending_space = false;
    for (const unsigned char c : input) {
        if (std::isspace(c)) {
            pending_space = !out.empty();
            continue;
        }
        const char ch = static_cast<char>(c);
        const bool closes = std::string_view(",.;:!?%)]}").find(ch) != std::string_view::npos;
        const bool previous_opens = !out.empty() && std::string_view("([{").find(out.back()) != std::string_view::npos;
        if (pending_space && !closes && !previous_opens && !out.empty()) out.push_back(' ');
        out.push_back(ch);
        pending_space = false;
    }
    return trim_copy(out);
}

bool known_join_candidate(
    const std::string& left,
    const std::string& right,
    const std::unordered_map<std::string, int>& vocabulary) {
    if (left.size() < 2 || right.size() < 2) return false;
    const auto candidate = lower_word(left + right);
    if (const auto it = vocabulary.find(candidate); it != vocabulary.end() && it->second > 0) return true;

    auto known = [&](const std::string& value) {
        const auto it = vocabulary.find(value);
        return it != vocabulary.end() && it->second > 0;
    };
    if (candidate.size() > 4 && candidate.ends_with('s') && known(candidate.substr(0, candidate.size() - 1))) return true;
    if (candidate.size() > 5 && candidate.ends_with("es") && known(candidate.substr(0, candidate.size() - 2))) return true;
    if (candidate.size() > 5 && candidate.ends_with("ed") && known(candidate.substr(0, candidate.size() - 2))) return true;
    if (candidate.size() > 6 && candidate.ends_with("ing")) {
        const auto stem = candidate.substr(0, candidate.size() - 3);
        if (known(stem) || known(stem + "e")) return true;
    }
    return false;
}

std::string repair_known_split_words(
    std::string text,
    const std::unordered_map<std::string, int>& vocabulary) {
    static const std::regex split_pair(R"(\b([A-Za-z]{2,}) ([A-Za-z]{2,})\b)");
    for (int pass = 0; pass < 4; ++pass) {
        std::smatch match;
        std::string remaining = text;
        std::string rebuilt;
        bool changed = false;
        while (std::regex_search(remaining, match, split_pair)) {
            rebuilt += match.prefix().str();
            const auto left = match[1].str();
            const auto right = match[2].str();
            if (known_join_candidate(left, right, vocabulary)) {
                rebuilt += left + right;
                changed = true;
            } else {
                rebuilt += match[0].str();
            }
            remaining = match.suffix().str();
        }
        rebuilt += remaining;
        text = std::move(rebuilt);
        if (!changed) break;
    }
    return text;
}

std::string remove_standalone_bracket_artifacts(std::string text) {
    static const std::regex isolated_bracket(R"(\s+[\[\]]\s+)");
    return std::regex_replace(text, isolated_bracket, " ");
}

std::string remove_document_confirmed_single_letter_noise(
    const std::string& text,
    const std::unordered_set<std::string>& bigrams) {
    static const std::regex triplet(R"(\b([A-Za-z]{3,}) ([B-HJ-Zb-hj-z]) ([A-Za-z]{3,})\b)");
    std::string remaining = text;
    std::string rebuilt;
    std::smatch match;
    while (std::regex_search(remaining, match, triplet)) {
        rebuilt += match.prefix().str();
        const auto left = match[1].str();
        const auto middle = match[2].str();
        const auto right = match[3].str();
        const auto bigram = lower_word(left) + " " + lower_word(right);
        if (bigrams.contains(bigram)) rebuilt += left + " " + right;
        else rebuilt += left + " " + middle + " " + right;
        remaining = match.suffix().str();
    }
    rebuilt += remaining;
    return rebuilt;
}

void normalize_paragraph_text(std::vector<Paragraph>& paragraphs) {
    std::unordered_map<std::string, int> vocabulary;
    std::unordered_set<std::string> bigrams;
    for (const auto& paragraph : paragraphs) {
        const auto words = extract_ascii_words(paragraph.source_text);
        for (const auto& word : words) {
            if (word.size() >= 2) ++vocabulary[word];
        }
        for (std::size_t i = 1; i < words.size(); ++i) {
            if (words[i - 1].size() >= 2 && words[i].size() >= 2) bigrams.insert(words[i - 1] + " " + words[i]);
        }
    }

    for (auto& paragraph : paragraphs) {
        auto normalized = normalize_surface_spacing(paragraph.source_text);
        normalized = remove_standalone_bracket_artifacts(std::move(normalized));
        normalized = repair_known_split_words(std::move(normalized), vocabulary);
        normalized = remove_document_confirmed_single_letter_noise(normalized, bigrams);
        paragraph.normalized_text = normalize_surface_spacing(normalized);
    }
}

void append_unique(std::vector<int>& values, const int value) {
    if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
}

bool ends_with_hyphen(const std::string& text) {
    for (auto it = text.rbegin(); it != text.rend(); ++it) {
        if (std::isspace(static_cast<unsigned char>(*it))) continue;
        return *it == '-';
    }
    return false;
}

double continuation_score(
    const Paragraph& current,
    const Region& next,
    const Page& previous_page,
    const Page& next_page,
    const Rect& previous_bbox,
    const int previous_column,
    const bool cross_page) {
    const bool body_pair = (current.semantic_type == RegionType::Body && next.type == RegionType::Body) ||
        (current.semantic_type == RegionType::AppendixBody && next.type == RegionType::AppendixBody);
    if (!body_pair) return 0.0;
    double score = 0.0;
    if (!ends_sentence(current.source_text)) score += 0.32;
    if (begins_lower_ascii(next.text)) score += 0.18;
    if (ends_with_hyphen(current.source_text)) score += 0.18;
    if (cross_page) {
        if (previous_bbox.bottom() >= previous_page.height * 0.72) score += 0.12;
        if (next.bbox.y <= next_page.height * 0.30) score += 0.12;
        if (previous_column == next.column || (previous_page.column_count == 2 && previous_column == 1 && next.column == 0)) score += 0.06;
    } else if (previous_page.column_count == 2 && previous_column != next.column) {
        if (previous_bbox.bottom() >= previous_page.height * 0.68) score += 0.12;
        if (next.bbox.y <= previous_page.height * 0.34) score += 0.12;
    }
    return score;
}

double page_body_font_size(const Page& page) {
    std::vector<double> sizes;
    for (const auto& region : page.semantic_regions) {
        if (region.type != RegionType::Body) continue;
        if (region.font_size < 5.0 || region.font_size > 24.0) continue;
        sizes.push_back(region.font_size);
    }
    return median(std::move(sizes), 10.0);
}

double axis_gap(const double a0, const double a1, const double b0, const double b1) {
    if (a1 < b0) return b0 - a1;
    if (b1 < a0) return a0 - b1;
    return 0.0;
}

bool near_locked_visual(const Page& page, const Region& region, const double body_font) {
    const double x_halo = std::max(18.0, body_font * 3.2);
    const double y_halo = std::max(12.0, body_font * 2.2);
    for (const auto& visual : page.locked_regions) {
        if (visual.type != RegionType::Figure && visual.type != RegionType::Table) continue;
        const double x_gap = axis_gap(region.bbox.x, region.bbox.right(), visual.bbox.x, visual.bbox.right());
        const double y_gap = axis_gap(region.bbox.y, region.bbox.bottom(), visual.bbox.y, visual.bbox.bottom());
        if (x_gap <= x_halo && y_gap <= y_halo) return true;
    }
    return false;
}

double heading_capital_ratio(const std::string& text) {
    int words = 0;
    int capitalized = 0;
    bool in_word = false;
    for (std::size_t i = 0; i < text.size() && words < 6; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (std::isalpha(c)) {
            if (!in_word) {
                ++words;
                if (std::isupper(c)) ++capitalized;
                in_word = true;
            }
        } else {
            in_word = false;
        }
    }
    return words == 0 ? 0.0 : static_cast<double>(capitalized) / words;
}

bool is_all_caps_heading_phrase(const std::string& text) {
    int letters = 0;
    int lowercase = 0;
    for (const auto raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (!std::isalpha(c)) continue;
        ++letters;
        if (std::islower(c)) ++lowercase;
    }
    return letters >= 3 && lowercase == 0;
}

bool is_valid_roman_numeral(const std::string& value) {
    static const std::regex roman(
        R"(^(?=[MDCLXVI]+$)M{0,4}(CM|CD|D?C{0,3})(XC|XL|L?X{0,3})(IX|IV|V?I{0,3})$)",
        std::regex::icase);
    return std::regex_match(value, roman);
}

bool looks_like_section_anchor_text(const std::string& text) {
    const auto value = trim_copy(text);
    if (value.empty()) return false;

    static const std::regex panel_label(R"(^\s*\([A-Za-z]\))");
    static const std::regex roman_numbered(R"(^\s*([IVXLCDM]+)[\.)]\s+(.+)$)", std::regex::icase);
    static const std::regex decimal_numbered(R"(^\s*\d+(?:\.\d+)*[\.)]?\s+\S)");
    static const std::regex letter_numbered(R"(^\s*[A-Z][\.)]\s+(.+)$)");
    static const std::regex broken_roman_fragment(R"(^\s*[IVXLCDM]+\s+[A-Z]\s*$)", std::regex::icase);

    if (std::regex_search(value, panel_label)) return false;
    if (std::regex_search(value, broken_roman_fragment)) return false;

    std::smatch roman_match;
    if (std::regex_search(value, roman_match, roman_numbered)) {
        const auto numeral = roman_match[1].str();
        const auto remainder = roman_match[2].str();
        if (is_valid_roman_numeral(numeral) &&
            (numeral.size() > 1 || is_all_caps_heading_phrase(remainder))) {
            return true;
        }
    }
    if (std::regex_search(value, decimal_numbered)) return true;

    std::smatch letter_match;
    if (std::regex_search(value, letter_match, letter_numbered)) {
        const auto remainder = letter_match[1].str();
        const auto colon = remainder.find(':');
        const auto comma = remainder.find(',');
        if (comma != std::string::npos &&
            (colon == std::string::npos || comma < colon) &&
            prose_word_count(remainder) > 4) {
            return false;
        }
        const auto ratio = heading_capital_ratio(remainder);
        return prose_word_count(remainder) <= 2 || ratio >= 0.45 || colon != std::string::npos;
    }

    const int words = prose_word_count(value);
    if (words == 0 || words > 10 || value.size() > 120) return false;
    const auto sentence_break = value.find(". ");
    if (sentence_break != std::string::npos && words > 3) return false;
    if ((value.ends_with('.') || value.ends_with('?') || value.ends_with('!')) && words > 4) return false;
    return heading_capital_ratio(value) >= 0.45;
}

bool is_section_anchor_region(const Page& page, const Region& region, const double body_font) {
    if (region.type != RegionType::Heading) return false;
    if (!looks_like_section_anchor_text(region.text)) return false;

    const bool strongly_numbered = looks_like_section_anchor_text(region.text) &&
        (std::regex_search(region.text, std::regex(R"(^\s*\d+(?:\.\d+)*[\.)]?\s+)")) ||
         std::regex_search(region.text, std::regex(R"(^\s*[A-Z][\.)]\s+)")) ||
         std::regex_search(region.text, std::regex(R"(^\s*[IVXLCDM]+[\.)]\s+)", std::regex::icase)));
    if (!strongly_numbered && region.font_size > std::max(13.0, body_font * 1.45)) return false;
    if (near_locked_visual(page, region, body_font) && !strongly_numbered) return false;
    return true;
}

std::string canonical_section_title(const std::string& text) {
    auto value = normalize_surface_spacing(text);
    const auto colon = value.find(':');
    if (colon != std::string::npos && colon < 120 && prose_word_count(value.substr(colon + 1)) >= 3) {
        value.resize(colon + 1);
    }
    return value;
}

int infer_section_level(const std::string& text) {
    const auto value = trim_copy(text);
    static const std::regex numeric_paren(R"(^\s*\d+\)\s+)");
    static const std::regex decimal(R"(^\s*(\d+(?:\.\d+)*)[\.]?\s+)");
    static const std::regex roman(R"(^\s*([IVXLCDM]+)[\.)]\s+(.+)$)", std::regex::icase);
    static const std::regex letter(R"(^\s*[A-Z][\.)]\s+)");

    if (std::regex_search(value, numeric_paren)) return 3;

    std::smatch match;
    if (std::regex_search(value, match, decimal)) {
        const auto number = match[1].str();
        return 1 + static_cast<int>(std::count(number.begin(), number.end(), '.'));
    }
    if (std::regex_search(value, match, roman)) {
        const auto numeral = match[1].str();
        const auto remainder = match[2].str();
        if (is_valid_roman_numeral(numeral) &&
            (numeral.size() > 1 || is_all_caps_heading_phrase(remainder))) return 1;
    }
    if (std::regex_search(value, letter)) return 2;
    return 1;
}

void assign_section_scope(std::vector<Paragraph>& paragraphs) {
    std::string current_id;
    std::string current_title;
    int current_level = 0;
    int section_counter = 0;

    auto next_section_id = [&]() {
        std::ostringstream s;
        s << "SEC_" << std::setw(3) << std::setfill('0') << ++section_counter;
        return s.str();
    };

    for (auto& paragraph : paragraphs) {
        if (paragraph.semantic_type == RegionType::Heading ||
            paragraph.semantic_type == RegionType::AppendixHeading) {
            current_id = next_section_id();
            current_title = canonical_section_title(paragraph.source_text);
            current_level = infer_section_level(current_title);
            paragraph.section_id = current_id;
            paragraph.section_title = current_title;
            paragraph.section_level = current_level;
            continue;
        }

        if ((paragraph.semantic_type == RegionType::Body ||
             paragraph.semantic_type == RegionType::AppendixBody) &&
            !current_id.empty()) {
            paragraph.section_id = current_id;
            paragraph.section_title = current_title;
            paragraph.section_level = current_level;
        }
    }
}

std::vector<Paragraph> build_paragraphs(const std::vector<Page>& pages) {
    struct Ref { int page; const Page* page_info; const Region* region; };
    std::vector<Ref> ordered;
    for (const auto& page : pages) {
        std::vector<const Region*> page_regions;
        const double body_font = page_body_font_size(page);
        for (const auto& region : page.semantic_regions) {
            if (is_translatable(region.type) || is_section_anchor_region(page, region, body_font))
                page_regions.push_back(&region);
        }
        std::sort(page_regions.begin(), page_regions.end(), [](const Region* a, const Region* b) {
            return a->reading_order < b->reading_order;
        });
        for (const auto* region : page_regions) ordered.push_back({page.page_index, &page, region});
    }

    std::vector<Paragraph> paragraphs;
    Paragraph current;
    bool active = false;
    int last_page = -1;
    int last_column = -99;
    Rect last_bbox{};
    const Page* last_page_info = nullptr;

    auto flush = [&]() {
        if (!active) return;
        current.paragraph_id = "P" + [&] {
            std::ostringstream s; s << std::setw(5) << std::setfill('0') << (paragraphs.size() + 1); return s.str();
        }();
        current.normalized_text = current.source_text;
        current.reading_order = static_cast<int>(paragraphs.size());
        paragraphs.push_back(current);
        current = Paragraph{};
        active = false;
    };

    for (const auto& ref : ordered) {
        const auto& region = *ref.region;
        const bool single_line_type = region.type == RegionType::Title || region.type == RegionType::Heading || region.type == RegionType::AppendixHeading;
        bool join = active && current.semantic_type == region.type && !single_line_type;
        if (join && ref.page == last_page && region.column == last_column) {
            const double gap = region.bbox.y - last_bbox.bottom();
            const bool clear_sentence_boundary = ends_sentence(current.source_text) && begins_upper_ascii(region.text);
            join = gap < std::max(region.font_size * 1.25, 6.0) && !clear_sentence_boundary;
        } else if (join && ref.page == last_page && region.column != last_column && last_page_info != nullptr) {
            join = continuation_score(current, region, *last_page_info, *ref.page_info, last_bbox, last_column, false) >= 0.50;
        } else if (join && ref.page == last_page + 1 && last_page_info != nullptr) {
            join = continuation_score(current, region, *last_page_info, *ref.page_info, last_bbox, last_column, true) >= 0.50;
        } else if (join) {
            join = false;
        }

        if (!join) {
            flush();
            current.semantic_type = region.type;
            current.source_text = region.text;
            current.page_span = {ref.page};
            current.column_span = {region.column};
            current.original_bbox = region.bbox;
            current.preferred_bbox = region.bbox;
            current.original_font_size = region.font_size;
            current.original_line_height = region.bbox.height;
            current.layout_fragments.push_back(LayoutFragment{
                ref.page,
                region.column,
                region.bbox,
                region.text,
                region.region_id,
                region.font_size,
                region.span_mode,
            });
            active = true;
        } else {
            current.source_text = join_line_text(current.source_text, region.text);
            append_unique(current.page_span, ref.page);
            append_unique(current.column_span, region.column);
            if (ref.page == current.page_span.front()) current.original_bbox = union_rect(current.original_bbox, region.bbox);
            current.original_font_size = std::min(current.original_font_size, region.font_size);
            current.original_line_height = std::max(current.original_line_height, region.bbox.height);
            current.layout_fragments.push_back(LayoutFragment{
                ref.page,
                region.column,
                region.bbox,
                region.text,
                region.region_id,
                region.font_size,
                region.span_mode,
            });
        }
        last_page = ref.page;
        last_column = region.column;
        last_bbox = region.bbox;
        last_page_info = ref.page_info;
        if (single_line_type) flush();
    }
    flush();
    assign_section_scope(paragraphs);
    for (std::size_t i = 0; i < paragraphs.size(); ++i) {
        if (i > 0) paragraphs[i].previous_id = paragraphs[i - 1].paragraph_id;
        if (i + 1 < paragraphs.size()) paragraphs[i].next_id = paragraphs[i + 1].paragraph_id;
    }
    return paragraphs;
}

// Small self-contained SHA-256 implementation used to make cache/document ids deterministic.
std::string sha256_hex(const std::vector<std::uint8_t>& input) {
    constexpr std::array<std::uint32_t, 64> k = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
    };
    auto rotr = [](const std::uint32_t x, const int n) { return (x >> n) | (x << (32 - n)); };
    std::vector<std::uint8_t> data = input;
    const std::uint64_t bit_len = static_cast<std::uint64_t>(data.size()) * 8u;
    data.push_back(0x80u);
    while ((data.size() % 64u) != 56u) data.push_back(0u);
    for (int i = 7; i >= 0; --i) data.push_back(static_cast<std::uint8_t>((bit_len >> (i * 8)) & 0xffu));
    std::array<std::uint32_t, 8> h = {0x6a09e667u,0xbb67ae85u,0x3c6ef372u,0xa54ff53au,0x510e527fu,0x9b05688cu,0x1f83d9abu,0x5be0cd19u};
    for (std::size_t offset = 0; offset < data.size(); offset += 64) {
        std::array<std::uint32_t, 64> w{};
        for (int i = 0; i < 16; ++i) {
            const auto p = offset + static_cast<std::size_t>(i) * 4;
            w[i] = (static_cast<std::uint32_t>(data[p]) << 24) | (static_cast<std::uint32_t>(data[p+1]) << 16) |
                   (static_cast<std::uint32_t>(data[p+2]) << 8) | static_cast<std::uint32_t>(data[p+3]);
        }
        for (int i = 16; i < 64; ++i) {
            const auto s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
            const auto s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        auto [a,b,c,d,e,f,g,hh] = h;
        for (int i = 0; i < 64; ++i) {
            const auto s1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            const auto ch = (e & f) ^ ((~e) & g);
            const auto temp1 = hh + s1 + ch + k[i] + w[i];
            const auto s0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            const auto maj = (a & b) ^ (a & c) ^ (b & c);
            const auto temp2 = s0 + maj;
            hh=g; g=f; f=e; e=d+temp1; d=c; c=b; b=a; a=temp1+temp2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto part : h) out << std::setw(8) << part;
    return out.str();
}

std::filesystem::path page_asset(const int page_index, const std::string_view suffix) {
    std::ostringstream name;
    name << "page_" << std::setw(3) << std::setfill('0') << (page_index + 1) << '_' << suffix << ".png";
    return std::filesystem::path(name.str());
}

}  // namespace

InspectorResult inspect_pdf(const std::filesystem::path& pdf_path, const std::filesystem::path& output_dir, const InspectorOptions& options) {
    InspectorResult result;
    result.output_dir = output_dir;
    try {
        if (!std::filesystem::exists(pdf_path) || !std::filesystem::is_regular_file(pdf_path)) {
            throw std::runtime_error("Input PDF does not exist: " + path_to_utf8(pdf_path));
        }
        std::filesystem::create_directories(output_dir);
        auto bytes = read_all_bytes(pdf_path);
        if (bytes.size() < 5 || std::memcmp(bytes.data(), "%PDF-", 5) != 0) {
            throw std::runtime_error("Input file does not start with a PDF header.");
        }

        detail::PdfiumRuntime api;
        std::string runtime_error;
        if (!api.load(runtime_error)) throw std::runtime_error(runtime_error);
        auto document_handle = api.LoadMemDocument64(bytes.data(), bytes.size(), nullptr);
        if (document_handle == nullptr) {
            std::ostringstream error;
            error << "PDFium failed to open the document (error " << api.GetLastError() << ").";
            throw std::runtime_error(error.str());
        }

        Document document;
        document.source_path = path_to_utf8(pdf_path);
        document.source_hash = sha256_hex(bytes);
        document.document_id = document.source_hash.substr(0, 16);
        document.page_count = api.GetPageCount(document_handle);
        if (document.page_count <= 0) {
            api.CloseDocument(document_handle);
            throw std::runtime_error("PDF contains no readable pages.");
        }

        int locked_region_count = 0;
        bool references_started = false;
        bool appendix_started = false;
        for (int page_index = 0; page_index < document.page_count; ++page_index) {
            auto page_handle = api.LoadPage(document_handle, page_index);
            if (page_handle == nullptr) continue;
            const double page_width = api.GetPageWidthF(page_handle);
            const double page_height = api.GetPageHeightF(page_handle);
            auto text_handle = api.TextLoadPage(page_handle);
            std::vector<LineRecord> lines;
            if (text_handle != nullptr) {
                lines = build_lines(extract_glyphs(api, text_handle, page_height), page_width);
                api.TextClosePage(text_handle);
            }

            std::vector<double> body_size_samples;
            for (const auto& line : lines) {
                if (line.bbox.y > page_height * 0.12 && line.bbox.bottom() < page_height * 0.93 &&
                    line.bbox.width > page_width * 0.12 && line.text.size() >= 18 && line.math_font_ratio < 0.35) {
                    body_size_samples.push_back(line.font_size);
                }
            }
            const double body_font = median(body_size_samples, 10.0);
            const auto layout = detect_columns(lines, page_width, page_height, body_font);
            classify_lines(lines, page_width, page_height, body_font, page_index);
            resolve_front_matter_abstract(lines, page_width, page_height, body_font, page_index);
            resolve_formula_layouts(lines, page_width, body_font);
            apply_document_section_state(lines, page_width, body_font, references_started, appendix_started);
            suppress_isolated_text_noise(lines);
            suppress_margin_artifacts(lines, page_width, body_font);
            refine_first_page_footnotes(lines, page_height, body_font, page_index);
            extend_keywords(lines, body_font);
            extend_locked_captions(lines, body_font);
            Raster original = render_page(api, page_handle, page_width, page_height, options.render_scale);
            auto visual_locked = detect_visual_locked_regions(original, lines, page_width, page_height, options.render_scale, layout);
            visual_locked.erase(
                std::remove_if(visual_locked.begin(), visual_locked.end(), [](const VisualCandidate& candidate) { return candidate.type == RegionType::Table; }),
                visual_locked.end());
            append_visual_candidates(visual_locked, detect_table_locked_regions(lines, page_width, page_height, body_font, layout));
            apply_visual_locked_regions(lines, visual_locked);
            coalesce_display_formulas(lines, page_width, body_font);
            const auto order = reading_order_indices(lines, layout.count, visual_locked);
            for (std::size_t i = 0; i < order.size(); ++i) lines[order[i]].reading_order = static_cast<int>(i);
            const auto blocks = build_blocks(lines, order);

            Page page;
            page.page_index = page_index;
            page.width = page_width;
            page.height = page_height;
            page.column_count = layout.count;
            page.gutter_x = layout.gutter_x;
            page.background_asset = page_asset(page_index, "clean").generic_string();

            for (std::size_t i = 0; i < visual_locked.size(); ++i) {
                Region region;
                std::ostringstream id;
                id << 'V' << std::setw(3) << std::setfill('0') << (page_index + 1) << '_' << std::setw(3) << std::setfill('0') << (i + 1);
                region.region_id = id.str();
                region.type = visual_locked[i].type;
                region.bbox = visual_locked[i].bbox;
                region.column = visual_locked[i].column;
                region.locked = true;
                region.confidence = visual_locked[i].confidence;
                region.source_kind = visual_locked[i].source_kind;
                region.locked_reason = visual_locked[i].reason;
                region.span_mode = visual_locked[i].span_mode;
                page.locked_regions.push_back(std::move(region));
            }

            for (std::size_t i = 0; i < blocks.size(); ++i) {
                const auto& block = blocks[i];
                Region region;
                std::ostringstream id;
                id << 'B' << std::setw(3) << std::setfill('0') << (page_index + 1) << '_' << std::setw(4) << std::setfill('0') << (i + 1);
                region.region_id = id.str();
                region.type = block.type;
                region.text = block.text;
                region.bbox = block.bbox;
                region.font_name = block.font_name;
                region.font_size = block.font_size;
                region.column = block.column;
                region.reading_order = block.reading_order;
                region.locked = block.locked;
                region.confidence = block.confidence;
                region.source_kind = "pdf_text_block";
                region.locked_reason = block.locked_reason;
                region.span_mode = block.span_mode;
                if (block.parent_locked_index >= 0 && static_cast<std::size_t>(block.parent_locked_index) < page.locked_regions.size()) {
                    region.parent_region_id = page.locked_regions[static_cast<std::size_t>(block.parent_locked_index)].region_id;
                }
                page.semantic_regions.push_back(std::move(region));
            }

            std::string image_error;
            if (options.write_original && !save_png(original, output_dir / page_asset(page_index, "original"), image_error)) {
                api.ClosePage(page_handle); api.CloseDocument(document_handle); throw std::runtime_error(image_error);
            }
            if (options.write_regions) {
                auto regions = original;
                for (const auto& region : page.locked_regions) draw_region_box(regions, region.bbox, options.render_scale, region.type);
                for (const auto& region : page.semantic_regions) draw_region_box(regions, region.bbox, options.render_scale, region.type);
                if (!save_png(regions, output_dir / page_asset(page_index, "regions"), image_error)) {
                    api.ClosePage(page_handle); api.CloseDocument(document_handle); throw std::runtime_error(image_error);
                }
            }
            if (options.write_clean) {
                auto clean = original;
                for (const auto& line : lines) if (is_translatable(line.type)) erase_region(clean, line.bbox, options.render_scale);
                if (!save_png(clean, output_dir / page_asset(page_index, "clean"), image_error)) {
                    api.ClosePage(page_handle); api.CloseDocument(document_handle); throw std::runtime_error(image_error);
                }
            }
            result.region_count += static_cast<int>(page.semantic_regions.size() + page.locked_regions.size());
            locked_region_count += static_cast<int>(page.locked_regions.size());
            document.pages.push_back(std::move(page));
            api.ClosePage(page_handle);
        }
        api.CloseDocument(document_handle);

        document.paragraphs = build_paragraphs(document.pages);
        normalize_paragraph_text(document.paragraphs);
        result.paragraph_count = static_cast<int>(document.paragraphs.size());
        result.page_count = document.page_count;
        std::string json_error;
        if (!write_json(document, output_dir / "paper_ir.json", json_error)) throw std::runtime_error(json_error);
        result.ok = true;
        std::ostringstream summary;
        summary << "Inspected " << result.page_count << " pages, " << result.region_count << " regions (" << locked_region_count
                << " visual locked), " << result.paragraph_count << " paragraphs.";
        result.message = summary.str();
    } catch (const std::exception& ex) {
        result.ok = false;
        result.message = ex.what();
    }
    return result;
}

extern "C" const char* palimpsest_version() {
    return "0.4.0";
}

extern "C" int palimpsest_inspect_pdf_utf8(const char* pdf_path, const char* output_dir, char* error_buffer, const std::size_t error_buffer_size) {
    if (pdf_path == nullptr || output_dir == nullptr) {
        if (error_buffer && error_buffer_size) {
            const char* message = "pdf_path and output_dir are required.";
            std::strncpy(error_buffer, message, error_buffer_size - 1);
            error_buffer[error_buffer_size - 1] = '\0';
        }
        return 2;
    }
    const auto input_path = path_from_utf8(pdf_path);
    const auto output_path = path_from_utf8(output_dir);
    if (input_path.empty() || output_path.empty()) {
        if (error_buffer && error_buffer_size) {
            const char* message = "pdf_path or output_dir is not valid UTF-8.";
            std::strncpy(error_buffer, message, error_buffer_size - 1);
            error_buffer[error_buffer_size - 1] = '\0';
        }
        return 2;
    }
    const auto result = inspect_pdf(input_path, output_path);
    if (!result.ok && error_buffer && error_buffer_size) {
        std::strncpy(error_buffer, result.message.c_str(), error_buffer_size - 1);
        error_buffer[error_buffer_size - 1] = '\0';
    }
    return result.ok ? 0 : 1;
}

}  // namespace palimpsest
