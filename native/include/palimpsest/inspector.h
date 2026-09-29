#pragma once

#include <filesystem>
#include <string>

namespace palimpsest {

struct InspectorOptions {
    double render_scale{2.0};
    bool write_original{true};
    bool write_regions{true};
    bool write_clean{true};
};

struct InspectorResult {
    bool ok{false};
    int page_count{};
    int region_count{};
    int paragraph_count{};
    std::filesystem::path output_dir;
    std::string message;
};

InspectorResult inspect_pdf(
    const std::filesystem::path& pdf_path,
    const std::filesystem::path& output_dir,
    const InspectorOptions& options = {});

}  // namespace palimpsest
