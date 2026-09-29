using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace Palimpsest_App.Translation;

public static class ReflowChinesePdfComposer
{
    private enum BlockKind
    {
        Text,
        Visual,
    }

    private sealed record PageInfo(
        int PageIndex,
        double Width,
        double Height,
        int ColumnCount,
        double GutterX,
        string OriginalAsset);

    private sealed record SourceBox(
        int PageIndex,
        int Column,
        int ReadingOrder,
        double X,
        double Y,
        double Width,
        double Height,
        double FontSize,
        string SpanMode,
        string RegionId,
        string ParentRegionId,
        string Type,
        string Text);

    private sealed record ReflowBlock(
        string Id,
        BlockKind Kind,
        string SemanticType,
        int SourcePage,
        int SourceColumn,
        int SourceReadingOrder,
        double SourceY,
        bool SpanAll,
        bool Splittable,
        bool KeepWithNext,
        string Text,
        double FontSize,
        double MarginBefore,
        double MarginAfter,
        bool Bold,
        string? SourceImage,
        double CropX,
        double CropY,
        double CropWidth,
        double CropHeight,
        string? Caption,
        double? CaptionFontSize,
        bool CaptionAbove);

    private sealed record LayoutTemplate(
        double PageWidth,
        double PageHeight,
        double MarginLeft,
        double MarginRight,
        double MarginTop,
        double MarginBottom,
        int Columns,
        double ColumnGap,
        double ColumnWidth,
        double BaseFontSize,
        double BaseLineHeight);

    public static async Task<ChinesePdfComposeResult> ComposeAsync(
        string paperIrPath,
        CancellationToken cancellationToken = default)
    {
        if (!File.Exists(paperIrPath))
            throw new FileNotFoundException("没有找到 paper_ir.json。", paperIrPath);

        paperIrPath = Path.GetFullPath(paperIrPath);
        var paperDirectory = Path.GetDirectoryName(paperIrPath) ?? Path.GetFullPath(".");
        using var paperIr = JsonDocument.Parse(await File.ReadAllTextAsync(paperIrPath, cancellationToken));
        var root = paperIr.RootElement;

        var pages = ParsePages(root, paperDirectory);
        if (pages.Count == 0)
            throw new InvalidOperationException("PaperIR 中没有页面。");

        var template = InferTemplate(root, pages);
        var blocks = BuildBlocks(root, pages, template);
        if (blocks.Count == 0)
            throw new InvalidOperationException("没有可用于中文版重排的文档块。");

        var sourcePath = GetString(root, "source_path");
        var outputPath = BuildOutputPath(sourcePath, paperDirectory);
        var qaPath = Path.Combine(paperDirectory, "pdf_export_qa_v2.json");
        var htmlPath = Path.Combine(paperDirectory, "_palimpsest_reflow_v2.html");
        var browserProfile = Path.Combine(paperDirectory, "_palimpsest_reflow_browser_profile");

        TryDeleteDirectory(browserProfile);
        Directory.CreateDirectory(browserProfile);
        Directory.CreateDirectory(Path.GetDirectoryName(outputPath) ?? paperDirectory);

        var html = BuildHtml(template, blocks);
        await File.WriteAllTextAsync(htmlPath, html, new UTF8Encoding(false), cancellationToken);

        var browser = FindBrowser()
            ?? throw new FileNotFoundException("没有找到 Microsoft Edge 或 Google Chrome，无法生成中文版 PDF。");
        var fileUri = new Uri(htmlPath).AbsoluteUri;

        var dump = await RunBrowserAsync(
            browser,
            [
                "--headless",
                "--disable-gpu",
                "--disable-background-networking",
                "--no-first-run",
                "--no-default-browser-check",
                "--allow-file-access-from-files",
                "--run-all-compositor-stages-before-draw",
                "--virtual-time-budget=1800",
                $"--user-data-dir={browserProfile}",
                "--dump-dom",
                fileUri,
            ],
            cancellationToken);

        var outputPages = ParseDataInt(dump.StdOut, "page-count", -1);
        var overlapCount = ParseDataInt(dump.StdOut, "overlap-count", -1);
        var splitCount = ParseDataInt(dump.StdOut, "split-count", -1);
        var visualCount = ParseDataInt(dump.StdOut, "visual-count", -1);
        var overflowCount = ParseDataInt(dump.StdOut, "overflow-count", -1);
        var outOfBoundsCount = ParseDataInt(dump.StdOut, "out-of-bounds-count", -1);
        var sparsePageCount = ParseDataInt(dump.StdOut, "sparse-page-count", -1);
        var meanOccupancyBp = ParseDataInt(dump.StdOut, "mean-occupancy-bp", -1);
        var minOccupancyBp = ParseDataInt(dump.StdOut, "min-occupancy-bp", -1);
        var pageOccupancyBp = ParseDataCsvInts(dump.StdOut, "page-occupancy-bp");
        var sparsePages = ParseDataCsvInts(dump.StdOut, "sparse-pages");
        var pageSemantics = ParsePageSemanticSummary(dump.StdOut);

        if (outputPages <= 0)
            throw new InvalidOperationException("Layout Engine V2 没有完成分页。");

        if (File.Exists(outputPath)) File.Delete(outputPath);
        var print = await RunBrowserAsync(
            browser,
            [
                "--headless",
                "--disable-gpu",
                "--disable-background-networking",
                "--no-first-run",
                "--no-default-browser-check",
                "--allow-file-access-from-files",
                "--run-all-compositor-stages-before-draw",
                "--virtual-time-budget=1800",
                "--no-pdf-header-footer",
                "--print-to-pdf-no-header",
                $"--user-data-dir={browserProfile}",
                $"--print-to-pdf={outputPath}",
                fileUri,
            ],
            cancellationToken);

        if (print.ExitCode != 0 || !File.Exists(outputPath) || new FileInfo(outputPath).Length < 1024)
        {
            throw new InvalidOperationException(
                $"Layout Engine V2 PDF 导出失败 (exit={print.ExitCode})：{Compact(print.StdErr + " " + print.StdOut)}");
        }

        var translatedCount = root.GetProperty("paragraphs")
            .EnumerateArray()
            .Count(p => !string.IsNullOrWhiteSpace(GetString(p, "translated_text")));

        var qa = new
        {
            layout_engine = "v2-greedy-reflow",
            output_pdf = outputPath,
            source_pages = pages.Count,
            output_pages = outputPages,
            page_delta = outputPages - pages.Count,
            translated_paragraphs = translatedCount,
            document_blocks = blocks.Count,
            visual_blocks = visualCount,
            split_paragraphs = splitCount,
            overlap_count = overlapCount,
            overflow_count = overflowCount,
            out_of_bounds_count = outOfBoundsCount,
            sparse_page_count = sparsePageCount,
            mean_occupancy_percent = meanOccupancyBp >= 0 ? meanOccupancyBp / 100.0 : -1,
            min_occupancy_percent = minOccupancyBp >= 0 ? minOccupancyBp / 100.0 : -1,
            sparse_pages = sparsePages,
            page_occupancy_percent = pageOccupancyBp.Select(value => value / 100.0).ToArray(),
            page_semantics = pageSemantics,
            columns = template.Columns,
            page_width = template.PageWidth,
            page_height = template.PageHeight,
            margins = new
            {
                left = template.MarginLeft,
                right = template.MarginRight,
                top = template.MarginTop,
                bottom = template.MarginBottom,
            },
            column_gap = template.ColumnGap,
            column_width = template.ColumnWidth,
            base_font_size = template.BaseFontSize,
            text_layer = true,
            whole_page_background = false,
            visual_policy = "source-crop-blocks",
            pagination = "deterministic-greedy",
            global_optimization = false,
        };
        await File.WriteAllTextAsync(
            qaPath,
            JsonSerializer.Serialize(qa, new JsonSerializerOptions { WriteIndented = true }),
            cancellationToken);

        TryDelete(htmlPath);
        TryDeleteDirectory(browserProfile);

        return new ChinesePdfComposeResult(
            outputPath,
            outputPages,
            translatedCount,
            Math.Max(0, overflowCount) + Math.Max(0, outOfBoundsCount) + Math.Max(0, overlapCount),
            qaPath);
    }

    private static List<PageInfo> ParsePages(JsonElement root, string paperDirectory)
    {
        var pages = new List<PageInfo>();
        foreach (var page in root.GetProperty("pages").EnumerateArray())
        {
            var background = GetString(page, "background_asset");
            var originalAsset = background.EndsWith("_clean.png", StringComparison.OrdinalIgnoreCase)
                ? background[..^"_clean.png".Length] + "_original.png"
                : Regex.Replace(background, @"_clean(?=\.[^.]+$)", "_original", RegexOptions.IgnoreCase);

            var originalPath = Path.GetFullPath(Path.Combine(paperDirectory, originalAsset));
            if (!File.Exists(originalPath))
                throw new FileNotFoundException($"缺少原页渲染图：{originalPath}", originalPath);

            pages.Add(new PageInfo(
                GetInt(page, "page_index", pages.Count),
                GetDouble(page, "width", 612),
                GetDouble(page, "height", 792),
                Math.Max(1, GetInt(page, "column_layout", 1)),
                GetDouble(page, "gutter_x", -1),
                new Uri(originalPath).AbsoluteUri));
        }
        return pages.OrderBy(p => p.PageIndex).ToList();
    }

    private static LayoutTemplate InferTemplate(JsonElement root, IReadOnlyList<PageInfo> pages)
    {
        var pageWidth = Median(pages.Select(p => p.Width).ToArray(), 612);
        var pageHeight = Median(pages.Select(p => p.Height).ToArray(), 792);
        var columns = pages
            .GroupBy(p => p.ColumnCount)
            .OrderByDescending(group => group.Count())
            .ThenByDescending(group => group.Key)
            .First().Key;

        var bodyBoxes = new List<SourceBox>();
        foreach (var page in root.GetProperty("pages").EnumerateArray())
        {
            var pageIndex = GetInt(page, "page_index", 0);
            if (!page.TryGetProperty("semantic_regions", out var regions)) continue;

            foreach (var region in regions.EnumerateArray())
            {
                var type = GetString(region, "type");
                if (type is not ("BODY" or "APPENDIX_BODY" or "HEADING" or "APPENDIX_HEADING" or "ABSTRACT"))
                    continue;
                bodyBoxes.Add(ParseSourceBox(region, pageIndex));
            }
        }

        var leftCandidates = bodyBoxes
            .Where(box => box.Column == 0 || columns == 1)
            .Select(box => box.X)
            .Where(value => value >= 0 && value < pageWidth * 0.45)
            .ToArray();
        var rightCandidates = bodyBoxes
            .Select(box => box.X + box.Width)
            .Where(value => value > pageWidth * 0.55 && value <= pageWidth)
            .ToArray();
        var topCandidates = bodyBoxes
            .Select(box => box.Y)
            .Where(value => value >= 20 && value < pageHeight * 0.35)
            .ToArray();
        var bottomCandidates = bodyBoxes
            .Select(box => box.Y + box.Height)
            .Where(value => value > pageHeight * 0.60 && value <= pageHeight - 10)
            .ToArray();

        var left = Clamp(Percentile(leftCandidates, 0.05, 48), 30, pageWidth * 0.18);
        var rightEdge = Clamp(Percentile(rightCandidates, 0.95, pageWidth - 48), pageWidth * 0.82, pageWidth - 30);
        var right = pageWidth - rightEdge;
        var top = Clamp(Percentile(topCandidates, 0.05, 55), 36, pageHeight * 0.14);
        var bottomEdge = Clamp(Percentile(bottomCandidates, 0.95, pageHeight - 44), pageHeight * 0.78, pageHeight - 24);
        var bottom = pageHeight - bottomEdge;

        var gap = columns > 1 ? Math.Max(14, Math.Min(24, pageWidth * 0.03)) : 0;
        var availableWidth = pageWidth - left - right - gap * (columns - 1);
        var columnWidth = availableWidth / columns;

        var fontCandidates = root.GetProperty("paragraphs")
            .EnumerateArray()
            .Where(p => GetString(p, "semantic_type") is "BODY" or "APPENDIX_BODY")
            .Select(p => GetDouble(p, "original_font_size", 0))
            .Where(size => size >= 6 && size <= 16)
            .ToArray();
        var baseFont = Clamp(Median(fontCandidates, 9.0), 8.0, 10.5);

        return new LayoutTemplate(
            pageWidth,
            pageHeight,
            left,
            right,
            top,
            bottom,
            columns,
            gap,
            columnWidth,
            baseFont,
            baseFont * 1.32);
    }

    private static List<ReflowBlock> BuildBlocks(
        JsonElement root,
        IReadOnlyList<PageInfo> pages,
        LayoutTemplate template)
    {
        var pageByIndex = pages.ToDictionary(p => p.PageIndex);
        var regionById = new Dictionary<string, SourceBox>(StringComparer.Ordinal);
        var semanticByPage = new Dictionary<int, List<SourceBox>>();
        var lockedByPage = new Dictionary<int, List<SourceBox>>();

        foreach (var page in root.GetProperty("pages").EnumerateArray())
        {
            var pageIndex = GetInt(page, "page_index", 0);
            var semantics = new List<SourceBox>();
            if (page.TryGetProperty("semantic_regions", out var semanticRegions))
            {
                foreach (var region in semanticRegions.EnumerateArray())
                {
                    var box = ParseSourceBox(region, pageIndex);
                    semantics.Add(box);
                    if (!string.IsNullOrWhiteSpace(box.RegionId))
                        regionById[box.RegionId] = box;
                }
            }
            semanticByPage[pageIndex] = semantics;

            var locked = new List<SourceBox>();
            if (page.TryGetProperty("locked_regions", out var lockedRegions))
            {
                foreach (var region in lockedRegions.EnumerateArray())
                    locked.Add(ParseSourceBox(region, pageIndex));
            }
            lockedByPage[pageIndex] = locked;
        }

        var pairedCaptions = new HashSet<string>(StringComparer.Ordinal);
        var visualOwnedRegionIds = new HashSet<string>(StringComparer.Ordinal);
        var translatedParagraphRegionIds = new HashSet<string>(StringComparer.Ordinal);
        var blocks = new List<ReflowBlock>();
        var blockCounter = 0;

        foreach (var paragraph in root.GetProperty("paragraphs").EnumerateArray())
        {
            var semanticType = GetString(paragraph, "semantic_type");
            var translated = GetString(paragraph, "translated_text").Trim();
            if (string.IsNullOrWhiteSpace(translated))
                continue;

            if (!paragraph.TryGetProperty("layout_fragments", out var fragments) ||
                fragments.ValueKind != JsonValueKind.Array ||
                fragments.GetArrayLength() == 0)
                continue;

            foreach (var fragment in fragments.EnumerateArray())
            {
                var ownedRegionId = GetString(fragment, "region_id");
                if (!string.IsNullOrWhiteSpace(ownedRegionId))
                    translatedParagraphRegionIds.Add(ownedRegionId);
            }

            var first = fragments[0];
            var pageIndex = GetInt(first, "page_index", 0);
            var column = GetInt(first, "column", 0);
            var bbox = first.GetProperty("bbox");
            var regionId = GetString(first, "region_id");
            var readingOrder = regionById.TryGetValue(regionId, out var originalRegion)
                ? originalRegion.ReadingOrder
                : GetInt(paragraph, "reading_order", 0);

            var sourceFont = GetDouble(paragraph, "original_font_size", template.BaseFontSize);
            var font = IsUsableSourceFontSize(sourceFont) ? sourceFont : template.BaseFontSize;

            var translatedHeading = semanticType is "HEADING" or "APPENDIX_HEADING";
            blocks.Add(new ReflowBlock(
                $"B{++blockCounter:00000}",
                BlockKind.Text,
                semanticType,
                pageIndex,
                column,
                readingOrder,
                GetDouble(bbox, "y", 0),
                false,
                semanticType is "BODY" or "APPENDIX_BODY",
                translatedHeading,
                NormalizeText(translated),
                font,
                translatedHeading ? 7 : 2.2,
                translatedHeading ? 5 : 4.2,
                translatedHeading,
                null,
                0, 0, 0, 0,
                null,
                null,
                false));
        }

        foreach (var pagePair in lockedByPage)
        {
            var pageIndex = pagePair.Key;
            if (!pageByIndex.TryGetValue(pageIndex, out var pageInfo)) continue;
            var semantics = semanticByPage.GetValueOrDefault(pageIndex) ?? [];

            foreach (var seedVisual in MergeLockedVisuals(pagePair.Value, pageInfo.Width))
            {
                var visual = ExpandVisualEnvelope(
                    seedVisual,
                    semantics,
                    template.BaseFontSize,
                    template.ColumnWidth,
                    visualOwnedRegionIds);
                var children = semantics
                    .Where(region => string.Equals(region.ParentRegionId, visual.RegionId, StringComparison.Ordinal))
                    .ToArray();
                var captionType = visual.Type == "TABLE" ? "TABLE_CAPTION" : "FIGURE_CAPTION";
                var caption = FindCaption(visual, semantics, captionType, pairedCaptions);
                var childOrders = children.Where(region => region.ReadingOrder >= 0).Select(region => region.ReadingOrder).ToArray();
                var readingOrder = caption is not null && caption.ReadingOrder >= 0
                    ? caption.ReadingOrder
                    : childOrders.Length > 0
                        ? childOrders.Min()
                        : FindNearestReadingOrder(visual, semantics);

                var spanAll = visual.SpanMode == "span" ||
                              visual.Column < 0 ||
                              visual.Width >= template.ColumnWidth * 1.35;

                blocks.Add(new ReflowBlock(
                    $"B{++blockCounter:00000}",
                    BlockKind.Visual,
                    visual.Type,
                    pageIndex,
                    InferColumn(visual, pageInfo.Width),
                    readingOrder,
                    visual.Y,
                    spanAll,
                    false,
                    false,
                    string.Empty,
                    template.BaseFontSize * 0.86,
                    5,
                    5,
                    false,
                    pageInfo.OriginalAsset,
                    visual.X,
                    visual.Y,
                    visual.Width,
                    visual.Height,
                    caption?.Text,
                    caption is not null && IsUsableSourceFontSize(caption.FontSize)
                        ? caption.FontSize
                        : caption is not null ? template.BaseFontSize * 0.82 : null,
                    caption is not null && caption.Y < visual.Y));

                if (caption is not null && !string.IsNullOrWhiteSpace(caption.RegionId))
                    pairedCaptions.Add(caption.RegionId);
            }
        }

        foreach (var pagePair in semanticByPage)
        {
            var pageIndex = pagePair.Key;
            if (!pageByIndex.TryGetValue(pageIndex, out var pageInfo)) continue;
            var locked = lockedByPage.GetValueOrDefault(pageIndex) ?? [];

            foreach (var formula in pagePair.Value.Where(region => region.Type == "FORMULA_DISPLAY"))
            {
                if (visualOwnedRegionIds.Contains(formula.RegionId)) continue;
                if (locked.Any(region =>
                        region.Type is "FIGURE" or "TABLE" &&
                        IntersectionRatio(formula, region) >= 0.55))
                    continue;

                blocks.Add(new ReflowBlock(
                    $"B{++blockCounter:00000}",
                    BlockKind.Visual,
                    "FORMULA_DISPLAY",
                    pageIndex,
                    InferColumn(formula, pageInfo.Width),
                    formula.ReadingOrder,
                    formula.Y,
                    formula.SpanMode == "span" || formula.Width >= template.ColumnWidth * 1.35,
                    false,
                    false,
                    string.Empty,
                    template.BaseFontSize,
                    5,
                    6,
                    false,
                    pageInfo.OriginalAsset,
                    formula.X,
                    formula.Y,
                    formula.Width,
                    formula.Height,
                    null,
                    null,
                    false));
            }
        }

        var excludedTextTypes = new HashSet<string>(StringComparer.Ordinal)
        {
            "BODY", "APPENDIX_BODY", "APPENDIX_HEADING",
            "FIGURE", "TABLE", "FORMULA_DISPLAY", "HEADER", "OTHER",
        };

        foreach (var pagePair in semanticByPage)
        {
            var pageIndex = pagePair.Key;
            var candidates = pagePair.Value
                .Where(region =>
                    !excludedTextTypes.Contains(region.Type) &&
                    !translatedParagraphRegionIds.Contains(region.RegionId) &&
                    !visualOwnedRegionIds.Contains(region.RegionId) &&
                    !string.IsNullOrWhiteSpace(region.Text) &&
                    !pairedCaptions.Contains(region.RegionId))
                .OrderBy(region => region.ReadingOrder)
                .ToArray();

            foreach (var group in GroupTextRegions(candidates))
            {
                var semanticType = group[0].Type;
                var text = string.Join(
                    semanticType is "TITLE" or "HEADING" ? "\n" : " ",
                    group.Select(region => region.Text.Trim()).Where(text => text.Length > 0));
                if (string.IsNullOrWhiteSpace(text)) continue;

                var union = Union(group);
                var sourceFont = Median(group.Select(region => region.FontSize).Where(size => size > 0).ToArray(), template.BaseFontSize);
                var font = IsUsableSourceFontSize(sourceFont) ? sourceFont : template.BaseFontSize;
                var isTitle = semanticType == "TITLE" && pageIndex == 0;
                var isHeading = semanticType == "HEADING" || (semanticType == "TITLE" && pageIndex > 0);
                var bold = isTitle || isHeading;

                var frontMatterSpan = pageIndex == 0 && semanticType is "TITLE" or "AUTHOR";
                var spanAll = union.SpanMode == "span" ||
                              union.Column < 0 ||
                              union.Width >= template.ColumnWidth * 1.35 ||
                              frontMatterSpan;

                blocks.Add(new ReflowBlock(
                    $"B{++blockCounter:00000}",
                    BlockKind.Text,
                    semanticType,
                    pageIndex,
                    InferColumn(union, pageByIndex[pageIndex].Width),
                    group.Min(region => region.ReadingOrder),
                    union.Y,
                    spanAll,
                    semanticType is "ABSTRACT" or "REFERENCE",
                    isTitle || isHeading,
                    NormalizeText(text),
                    font,
                    isTitle ? 6 : isHeading ? 7 : 2.5,
                    isTitle ? 7 : isHeading ? 5 : 3.5,
                    bold,
                    null,
                    0, 0, 0, 0,
                    null,
                    null,
                    false));
            }
        }

        return blocks
            .OrderBy(block => block.SourcePage)
            .ThenBy(block => block.SourceReadingOrder < 0 ? int.MaxValue / 2 + (int)Math.Round(block.SourceY) : block.SourceReadingOrder)
            .ThenBy(block => block.SourceColumn < 0 ? 0 : block.SourceColumn)
            .ThenBy(block => block.SourceY)
            .ThenBy(block => block.Id, StringComparer.Ordinal)
            .ToList();
    }

    private static readonly Regex PanelLabelRegex = new(
        @"^\s*\([A-Za-z]\)\s*$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);

    private static bool IsPanelLabel(SourceBox region) =>
        PanelLabelRegex.IsMatch(region.Text ?? string.Empty);

    private static bool IsVisualExpansionCandidate(SourceBox region, double baseFontSize)
    {
        if (region.Type == "FIGURE") return true;
        if (IsPanelLabel(region)) return true;

        // Broken vector/text extraction inside figures is frequently classified as
        // display math with an implausibly large font size. Real equations tend to
        // stay near the paper's body/math size, so only absorb the anomalous case.
        return region.Type == "FORMULA_DISPLAY" &&
               region.FontSize >= Math.Max(12.0, baseFontSize * 1.35);
    }

    private static bool VisualColumnsCompatible(SourceBox visual, SourceBox candidate)
    {
        if (visual.Column < 0 || candidate.Column < 0) return true;
        return visual.Column == candidate.Column;
    }

    private static SourceBox ExpandVisualEnvelope(
        SourceBox seed,
        IReadOnlyList<SourceBox> semantics,
        double baseFontSize,
        double columnWidth,
        ISet<string> claimedRegionIds)
    {
        var current = seed;
        var changed = true;
        var horizontalHalo = Math.Max(18.0, Math.Min(48.0, columnWidth * 0.20));
        var verticalHalo = Math.Max(12.0, baseFontSize * 2.5);

        while (changed)
        {
            changed = false;
            foreach (var candidate in semantics)
            {
                if (string.IsNullOrWhiteSpace(candidate.RegionId) ||
                    claimedRegionIds.Contains(candidate.RegionId) ||
                    !IsVisualExpansionCandidate(candidate, baseFontSize) ||
                    !VisualColumnsCompatible(current, candidate))
                    continue;

                var horizontalGap = Gap(
                    current.X,
                    current.X + current.Width,
                    candidate.X,
                    candidate.X + candidate.Width);
                var verticalGap = Gap(
                    current.Y,
                    current.Y + current.Height,
                    candidate.Y,
                    candidate.Y + candidate.Height);
                var horizontalOverlap = HorizontalOverlap(current, candidate);
                var verticalOverlap = Overlap(
                    current.Y,
                    current.Y + current.Height,
                    candidate.Y,
                    candidate.Y + candidate.Height);

                var stacked = verticalGap <= verticalHalo &&
                              horizontalOverlap >= Math.Min(current.Width, candidate.Width) * 0.18;
                var sideBySide = horizontalGap <= horizontalHalo &&
                                 verticalOverlap >= Math.Min(current.Height, candidate.Height) * 0.18;
                var overlaps = IntersectionRatio(candidate, current) > 0.02 ||
                               IntersectionRatio(current, candidate) > 0.02;

                var panelAttached = IsPanelLabel(candidate) &&
                                    verticalGap <= verticalHalo * 1.35 &&
                                    horizontalGap <= horizontalHalo * 1.35;

                if (!stacked && !sideBySide && !overlaps && !panelAttached)
                    continue;

                current = Union([current, candidate]);
                claimedRegionIds.Add(candidate.RegionId);
                changed = true;
            }
        }

        return current;
    }

    private static IReadOnlyList<SourceBox> MergeLockedVisuals(IReadOnlyList<SourceBox> input, double pageWidth)
    {
        var result = new List<SourceBox>();
        foreach (var group in input
                     .Where(region => region.Type is "FIGURE" or "TABLE")
                     .GroupBy(region => new { region.Type, Column = InferColumn(region, pageWidth) }))
        {
            var ordered = group.OrderBy(region => region.Y).ThenBy(region => region.X).ToList();
            while (ordered.Count > 0)
            {
                var current = ordered[0];
                ordered.RemoveAt(0);
                var changed = true;
                while (changed)
                {
                    changed = false;
                    for (var i = ordered.Count - 1; i >= 0; --i)
                    {
                        var candidate = ordered[i];
                        var verticalOverlap = Overlap(current.Y, current.Y + current.Height, candidate.Y, candidate.Y + candidate.Height);
                        var horizontalGap = Gap(current.X, current.X + current.Width, candidate.X, candidate.X + candidate.Width);
                        var verticalGap = Gap(current.Y, current.Y + current.Height, candidate.Y, candidate.Y + candidate.Height);
                        var sameRow = verticalOverlap >= Math.Min(current.Height, candidate.Height) * 0.28 && horizontalGap <= 16;
                        var stackedTight = HorizontalOverlap(current, candidate) >= Math.Min(current.Width, candidate.Width) * 0.62 &&
                                           verticalGap <= 8;
                        if (!sameRow && !stackedTight) continue;

                        current = Union([current, candidate]) with
                        {
                            RegionId = current.RegionId,
                            Type = current.Type,
                            Column = current.Column,
                            SpanMode = current.SpanMode == "span" || candidate.SpanMode == "span" ? "span" : current.SpanMode,
                        };
                        ordered.RemoveAt(i);
                        changed = true;
                    }
                }
                result.Add(current);
            }
        }
        return result;
    }

    private static SourceBox? FindCaption(
        SourceBox visual,
        IReadOnlyList<SourceBox> semantics,
        string captionType,
        ISet<string> alreadyPaired)
    {
        return semantics
            .Where(region =>
                region.Type == captionType &&
                !alreadyPaired.Contains(region.RegionId) &&
                HorizontalOverlap(visual, region) >= Math.Min(visual.Width, Math.Max(1, region.Width)) * 0.35)
            .Select(region => new
            {
                Region = region,
                Distance = VerticalDistance(visual, region),
            })
            .Where(item => item.Distance <= 72)
            .OrderBy(item => item.Distance)
            .ThenBy(item => item.Region.ReadingOrder)
            .Select(item => item.Region)
            .FirstOrDefault();
    }

    private static IReadOnlyList<IReadOnlyList<SourceBox>> GroupTextRegions(IReadOnlyList<SourceBox> input)
    {
        var groups = new List<IReadOnlyList<SourceBox>>();
        var current = new List<SourceBox>();

        foreach (var region in input)
        {
            if (current.Count == 0)
            {
                current.Add(region);
                continue;
            }

            var previous = current[^1];
            var sameType = previous.Type == region.Type;
            var sameColumn = previous.Column == region.Column;
            var closeOrder = previous.ReadingOrder < 0 || region.ReadingOrder < 0 ||
                             region.ReadingOrder - previous.ReadingOrder <= 3;
            var verticalGap = region.Y - (previous.Y + previous.Height);
            var closeGeometry = verticalGap <= Math.Max(8, Math.Max(previous.FontSize, region.FontSize) * 1.35);
            var overlapX = HorizontalOverlap(previous, region) >= Math.Min(previous.Width, region.Width) * 0.30;
            var merge = sameType && sameColumn && closeOrder && closeGeometry && overlapX &&
                        region.Type is not ("TITLE" or "HEADING");

            if (!merge)
            {
                groups.Add(current.ToArray());
                current = [];
            }
            current.Add(region);
        }

        if (current.Count > 0) groups.Add(current.ToArray());
        return groups;
    }

    private static string BuildHtml(LayoutTemplate template, IReadOnlyList<ReflowBlock> blocks)
    {
        var data = blocks.Select(block => new
        {
            id = block.Id,
            kind = block.Kind == BlockKind.Text ? "text" : "visual",
            semantic = block.SemanticType,
            span = block.SpanAll,
            splittable = block.Splittable,
            keepWithNext = block.KeepWithNext,
            text = block.Text,
            fontSize = block.FontSize,
            marginBefore = block.MarginBefore,
            marginAfter = block.MarginAfter,
            bold = block.Bold,
            sourceImage = block.SourceImage,
            crop = block.Kind == BlockKind.Visual
                ? new { x = block.CropX, y = block.CropY, width = block.CropWidth, height = block.CropHeight }
                : null,
            caption = block.Caption,
            captionFontSize = block.CaptionFontSize,
            captionAbove = block.CaptionAbove,
        }).ToArray();

        var json = JsonSerializer.Serialize(data).Replace("</", "<\\/");
        var html = new StringBuilder(196 * 1024);
        html.AppendLine("<!doctype html><html><head><meta charset=\"utf-8\">");
        html.Append("<style>");
        html.Append("@page{size:").Append(Pt(template.PageWidth)).Append(' ').Append(Pt(template.PageHeight)).Append(";margin:0}");
        html.Append("html,body{margin:0;padding:0;background:#e9e9e9;-webkit-print-color-adjust:exact;print-color-adjust:exact}");
        html.Append("#document{margin:0;padding:0}");
        html.Append(".page{position:relative;width:").Append(Pt(template.PageWidth))
            .Append(";height:").Append(Pt(template.PageHeight))
            .Append(";overflow:hidden;background:#fff;break-after:page;page-break-after:always;margin:0}");
        html.Append(".page:last-child{break-after:auto;page-break-after:auto}");
        html.Append(":root{--paper-serif:\"Times New Roman\",\"Noto Serif SC\",\"Songti SC\",SimSun,serif;--paper-sans:\"Times New Roman\",\"Noto Sans SC\",\"Microsoft YaHei\",sans-serif}");
        html.Append(".placed{position:absolute;box-sizing:border-box;color:#111}");
        html.Append(".text{white-space:pre-wrap;overflow:hidden;word-break:normal;overflow-wrap:anywhere;line-break:strict;text-align:left;");
        html.Append("font-family:var(--paper-serif);font-kerning:normal;text-rendering:optimizeLegibility;font-synthesis:none}");
        html.Append(".text[data-semantic=\"TITLE\"],.text[data-semantic=\"HEADING\"],.text[data-semantic=\"APPENDIX_HEADING\"]{font-family:var(--paper-sans)}");
        html.Append(".visual{overflow:hidden}");
        html.Append(".visual img{position:absolute;max-width:none;max-height:none;user-select:none}");
        html.Append(".visual-caption{font-family:var(--paper-serif);line-height:1.24;text-align:left;color:#111;margin:0;padding:0;font-kerning:normal;text-rendering:optimizeLegibility}");
        html.Append(".page-number{position:absolute;left:0;right:0;bottom:")
            .Append(Pt(Math.Max(8, template.MarginBottom * 0.24)))
            .Append(";text-align:center;font:").Append(Pt(template.BaseFontSize * 0.72))
            .Append(" \"Times New Roman\",serif;color:#555}");
        html.Append("#measure{position:absolute;left:-10000px;top:0;visibility:hidden;box-sizing:border-box}");
        html.Append("</style></head><body><div id=\"document\"></div><div id=\"measure\"></div><script>");
        html.Append("const BLOCKS=").Append(json).Append(';');
        html.Append("const CFG=").Append(JsonSerializer.Serialize(new
        {
            pageWidth = template.PageWidth,
            pageHeight = template.PageHeight,
            marginLeft = template.MarginLeft,
            marginRight = template.MarginRight,
            marginTop = template.MarginTop,
            marginBottom = template.MarginBottom,
            columns = template.Columns,
            columnGap = template.ColumnGap,
            columnWidth = template.ColumnWidth,
            baseFontSize = template.BaseFontSize,
            baseLineHeight = template.BaseLineHeight,
        })).Append(';');

        html.Append(@"
const doc=document.getElementById('document');
const measure=document.getElementById('measure');
let pages=[];
let page=null;
let pageIndex=-1;
let col=0;
let cursor=[];
let splitCount=0;
let visualCount=0;
let overflowCount=0;
let pendingSpanFloats=[];
let flushingSpanFloats=false;
const CSS_PX_TO_PT=72/96;

function pt(v){return String(v)+'pt';}
function columnX(c){return CFG.marginLeft+c*(CFG.columnWidth+CFG.columnGap);}
function usableBottom(){return CFG.pageHeight-CFG.marginBottom;}
function remaining(){return usableBottom()-cursor[col];}

function createPage(){
  pageIndex++;
  col=0;
  cursor=Array(CFG.columns).fill(CFG.marginTop);
  const el=document.createElement('section');
  el.className='page';
  el.dataset.page=String(pageIndex+1);
  const pn=document.createElement('div');
  pn.className='page-number';
  pn.textContent=String(pageIndex+1);
  el.appendChild(pn);
  doc.appendChild(el);
  page={el};
  pages.push(page);
}
function newPage(){
  createPage();
  flushPendingSpanFloats();
}
function advanceColumn(){
  if(col+1<CFG.columns){col++;return;}
  newPage();
}
function pageHasContent(){
  return cursor.some(y=>y>CFG.marginTop+0.5);
}
function currentMaxY(){return Math.max(...cursor);}
function currentMinY(){return Math.min(...cursor);}
function columnsAligned(){return currentMaxY()-currentMinY()<0.8;}

function styleText(el,b,width){
  el.className='placed text';
  el.dataset.semantic=b.semantic||'';
  el.style.width=pt(width);
  el.style.fontSize=pt(b.fontSize);
  el.style.lineHeight='1.32';
  el.style.fontWeight=b.bold?'700':'400';
  if(b.semantic==='TITLE')el.style.textAlign='center';
}
function measureText(b,text,width){
  measure.className='text';
  measure.dataset.semantic=b.semantic||'';
  measure.style.width=pt(width);
  measure.style.height='auto';
  measure.style.fontSize=pt(b.fontSize);
  measure.style.lineHeight='1.32';
  measure.style.fontWeight=b.bold?'700':'400';
  measure.style.whiteSpace='pre-wrap';
  measure.style.wordBreak='normal';
  measure.style.overflowWrap='anywhere';
  measure.textContent=text;
  return Math.ceil(measure.getBoundingClientRect().height*CSS_PX_TO_PT*100)/100;
}
function normalizeStart(s){return (s||'').replace(/^\s+/u,'');}
function isLow(code){return code>=0xDC00&&code<=0xDFFF;}
function isHigh(code){return code>=0xD800&&code<=0xDBFF;}
function safeCut(s,n){
  if(n<=0)return 0;
  if(n>=s.length)return s.length;
  let c=n;
  if(c>0&&c<s.length&&isLow(s.charCodeAt(c))&&isHigh(s.charCodeAt(c-1)))c--;
  const left=s.slice(0,c);
  const m=left.match(/([A-Za-z0-9_./:+-]+)$/);
  if(m&&m[1].length<c&&m[1].length<48)c-=m[1].length;
  const tail=left.slice(Math.max(0,c-1),c);
  if(/[（《「『【“]/.test(tail)&&c>1)c--;
  return Math.max(1,c);
}
function fitPrefix(b,text,width,maxHeight){
  if(!text||maxHeight<=b.fontSize*1.25)return 0;
  let lo=1,hi=text.length,best=0;
  while(lo<=hi){
    const mid=Math.floor((lo+hi)/2);
    const h=measureText(b,text.slice(0,mid),width);
    if(h<=maxHeight+0.5){best=mid;lo=mid+1}else hi=mid-1;
  }
  if(best<=0)return 0;
  let cut=safeCut(text,best);
  const closing=/[，。；：！？、）》」』】”’,.!?;:\)\]\}]/;
  while(cut<text.length&&closing.test(text[cut])){
    const candidate=cut+1;
    if(measureText(b,text.slice(0,candidate),width)>maxHeight+0.5)break;
    cut=candidate;
  }
  return cut;
}
function balanceSplitCut(b,text,width,maxHeight,cut){
  if(cut<=0||cut>=text.length)return cut;
  const lineH=b.fontSize*1.32;
  const rest=normalizeStart(text.slice(cut));
  if(!rest)return cut;
  if(measureText(b,rest,width)>=lineH*1.85)return cut;
  const reducedHeight=maxHeight-lineH*1.05;
  if(reducedHeight<lineH*2.05)return cut;
  const reduced=fitPrefix(b,text,width,reducedHeight);
  if(reduced<=0||reduced>=cut)return cut;
  const reducedRest=normalizeStart(text.slice(reduced));
  return reducedRest&&measureText(b,reducedRest,width)>=lineH*1.85?reduced:cut;
}
function placeTextChunk(b,text,x,y,width){
  const el=document.createElement('div');
  styleText(el,b,width);
  el.dataset.block=b.id;
  el.dataset.semantic=b.semantic;
  el.style.left=pt(x);
  el.style.top=pt(y);
  el.textContent=text;
  page.el.appendChild(el);
  const h=Math.ceil(el.getBoundingClientRect().height*CSS_PX_TO_PT*100)/100;
  el.style.height=pt(h+0.5);
  return h;
}
function ensureHeadingRoom(b,width){
  if(!b.keepWithNext)return;
  const h=measureText(b,b.text,width);
  const reserve=Math.max(CFG.baseLineHeight*2.2,CFG.baseFontSize*2.6);
  if(remaining()<b.marginBefore+h+b.marginAfter+reserve)advanceColumn();
}

function placeText(b){
  if(b.span){
    const width=CFG.pageWidth-CFG.marginLeft-CFG.marginRight;
    if(pageHasContent()&&!columnsAligned())newPage();
    const h0=measureText(b,b.text,width);
    if(currentMaxY()+b.marginBefore+h0+b.marginAfter>usableBottom())newPage();
    const y=currentMaxY()+b.marginBefore;
    const h=placeTextChunk(b,b.text,CFG.marginLeft,y,width);
    const next=y+h+b.marginAfter;
    cursor=cursor.map(()=>next);
    col=0;
    return;
  }

  ensureHeadingRoom(b,CFG.columnWidth);
  let rest=normalizeStart(b.text);
  let first=true;
  let safety=0;
  while(rest&&safety++<128){
    let y=cursor[col]+(first?b.marginBefore:0);
    let avail=usableBottom()-y-(first?b.marginAfter:0);

    const wholeH=measureText(b,rest,CFG.columnWidth);
    if(wholeH<=avail+0.5){
      const h=placeTextChunk(b,rest,columnX(col),y,CFG.columnWidth);
      cursor[col]=y+h+b.marginAfter;
      rest='';
      break;
    }

    if(!b.splittable){
      advanceColumn();
      first=true;
      continue;
    }

    const minChunkHeight=b.fontSize*1.32*2.05;
    if(avail<minChunkHeight){
      advanceColumn();
      first=false;
      continue;
    }

    let cut=fitPrefix(b,rest,CFG.columnWidth,avail);
    cut=balanceSplitCut(b,rest,CFG.columnWidth,avail,cut);
    if(cut<=0){
      advanceColumn();
      first=false;
      continue;
    }

    let chunk=rest.slice(0,cut).trimEnd();
    if(!chunk){
      advanceColumn();
      first=false;
      continue;
    }

    const h=placeTextChunk(b,chunk,columnX(col),y,CFG.columnWidth);
    cursor[col]=y+h;
    rest=normalizeStart(rest.slice(cut));
    if(rest){
      splitCount++;
      advanceColumn();
      first=false;
    }
  }
  if(rest)overflowCount++;
}

function visualMetrics(b,maxWidth){
  const crop=b.crop;
  if(!crop||crop.width<=0||crop.height<=0)return null;
  const upscale=b.semantic==='FIGURE'||b.semantic==='TABLE'?1.05:1.0;
  const scale=Math.min(maxWidth/crop.width,upscale);
  let imageH=crop.height*scale;
  let captionH=0;
  if(b.caption){
    const cap={...b,fontSize:(b.captionFontSize||CFG.baseFontSize*0.82),bold:false,semantic:'FIGURE_CAPTION'};
    captionH=measureText(cap,b.caption,maxWidth);
  }
  return {scale,imageW:crop.width*scale,imageH,captionH,total:imageH+(b.caption?5+captionH:0)};
}
function appendVisualCrop(b,x,y,width,metrics){
  const holder=document.createElement('div');
  holder.className='placed visual';
  holder.dataset.block=b.id;
  holder.dataset.semantic=b.semantic;
  holder.style.left=pt(x);
  holder.style.top=pt(y);
  holder.style.width=pt(width);
  holder.style.height=pt(metrics.total);

  let imageTop=0;
  if(b.caption&&b.captionAbove){
    const cap=document.createElement('div');
    cap.className='visual-caption';
    cap.textContent=b.caption;
    cap.style.fontSize=pt(b.captionFontSize||CFG.baseFontSize*0.82);
    cap.style.height=pt(metrics.captionH+1);
    holder.appendChild(cap);
    imageTop=metrics.captionH+5;
  }

  const viewport=document.createElement('div');
  viewport.style.position='absolute';
  viewport.style.left=pt((width-metrics.imageW)/2);
  viewport.style.top=pt(imageTop);
  viewport.style.width=pt(metrics.imageW);
  viewport.style.height=pt(metrics.imageH);
  viewport.style.overflow='hidden';

  const img=document.createElement('img');
  img.src=b.sourceImage;
  img.style.width=pt(CFG.pageWidth*metrics.scale);
  img.style.height=pt(CFG.pageHeight*metrics.scale);
  img.style.left=pt(-b.crop.x*metrics.scale);
  img.style.top=pt(-b.crop.y*metrics.scale);
  viewport.appendChild(img);
  holder.appendChild(viewport);

  if(b.caption&&!b.captionAbove){
    const cap=document.createElement('div');
    cap.className='visual-caption';
    cap.textContent=b.caption;
    cap.style.fontSize=pt(b.captionFontSize||CFG.baseFontSize*0.82);
    cap.style.position='absolute';
    cap.style.left='0';
    cap.style.right='0';
    cap.style.top=pt(metrics.imageH+5);
    cap.style.height=pt(metrics.captionH+1);
    holder.appendChild(cap);
  }

  page.el.appendChild(holder);
  visualCount++;
}
function placeSpanVisualNow(b){
  const width=CFG.pageWidth-CFG.marginLeft-CFG.marginRight;
  let metrics=visualMetrics(b,width);
  if(!metrics)return;
  const fullAvailable=usableBottom()-CFG.marginTop-b.marginBefore-b.marginAfter;
  if(metrics.total>fullAvailable){
    const ratio=Math.max(0.1,fullAvailable/metrics.total);
    metrics={...metrics,scale:metrics.scale*ratio,imageW:metrics.imageW*ratio,imageH:metrics.imageH*ratio,total:metrics.total*ratio};
  }
  if(pageHasContent()&&currentMaxY()+b.marginBefore+metrics.total+b.marginAfter>usableBottom())
    createPage();
  const y=currentMaxY()+b.marginBefore;
  appendVisualCrop(b,CFG.marginLeft,y,width,metrics);
  const next=y+metrics.total+b.marginAfter;
  cursor=cursor.map(()=>next);
  col=0;
}
function flushPendingSpanFloats(){
  if(flushingSpanFloats||pendingSpanFloats.length===0)return;
  flushingSpanFloats=true;
  try{
    while(pendingSpanFloats.length>0){
      const pending=pendingSpanFloats.shift();
      placeSpanVisualNow(pending);
    }
  }finally{
    flushingSpanFloats=false;
  }
}

function placeVisual(b){
  const width=b.span?(CFG.pageWidth-CFG.marginLeft-CFG.marginRight):CFG.columnWidth;
  let metrics=visualMetrics(b,width);
  if(!metrics)return;

  if(b.span){
    if(pendingSpanFloats.length>0 ||
       (pageHasContent()&&!columnsAligned()) ||
       (pageHasContent()&&currentMaxY()+b.marginBefore+metrics.total+b.marginAfter>usableBottom())){
      pendingSpanFloats.push(b);
      return;
    }
    placeSpanVisualNow(b);
    return;
  }

  let needed=b.marginBefore+metrics.total+b.marginAfter;
  if(needed>remaining()){
    advanceColumn();
    metrics=visualMetrics(b,width);
    needed=b.marginBefore+metrics.total+b.marginAfter;
  }
  if(needed>remaining()){
    const available=Math.max(40,remaining()-b.marginBefore-b.marginAfter);
    const ratio=Math.min(1,available/metrics.total);
    metrics={...metrics,scale:metrics.scale*ratio,imageW:metrics.imageW*ratio,imageH:metrics.imageH*ratio,total:metrics.total*ratio};
  }
  const y=cursor[col]+b.marginBefore;
  appendVisualCrop(b,columnX(col),y,width,metrics);
  cursor[col]=y+metrics.total+b.marginAfter;
}

function analyzePageQuality(){
  let sparse=0;
  let outOfBounds=0;
  let sumOccupancy=0;
  let minOccupancy=1;
  let counted=0;
  const pageBp=[];
  const sparsePages=[];

  for(let pi=0;pi<pages.length;pi++){
    const p=pages[pi];
    const pr=p.el.getBoundingClientRect();
    const sx=pr.width/CFG.pageWidth;
    const sy=pr.height/CFG.pageHeight;
    const printableW=Math.max(1,pr.width-(CFG.marginLeft+CFG.marginRight)*sx);
    const printableH=Math.max(1,pr.height-(CFG.marginTop+CFG.marginBottom)*sy);
    const printableArea=printableW*printableH;
    const items=[...p.el.querySelectorAll(':scope > .placed')];
    let occupied=0;

    for(const item of items){
      const r=item.getBoundingClientRect();
      if(r.left<pr.left-0.75||r.top<pr.top-0.75||r.right>pr.right+0.75||r.bottom>pr.bottom+0.75)
        outOfBounds++;
      const ix=Math.max(0,Math.min(r.right,pr.right)-Math.max(r.left,pr.left));
      const iy=Math.max(0,Math.min(r.bottom,pr.bottom)-Math.max(r.top,pr.top));
      occupied+=ix*iy;
    }

    const ratio=Math.max(0,Math.min(1,occupied/printableArea));
    sumOccupancy+=ratio;
    minOccupancy=Math.min(minOccupancy,ratio);
    counted++;
    pageBp.push(Math.round(ratio*10000));
    if(pi>0&&pi<pages.length-1&&ratio<0.16){sparse++;sparsePages.push(pi+1);}
  }

  return {
    sparse,
    outOfBounds,
    meanBp:counted?Math.round(sumOccupancy/counted*10000):0,
    minBp:counted?Math.round(minOccupancy*10000):0,
    pageBp,
    sparsePages
  };
}

function countOverlaps(){
  let count=0;
  for(const p of pages){
    const items=[...p.el.querySelectorAll(':scope > .placed')];
    for(let i=0;i<items.length;i++){
      const a=items[i].getBoundingClientRect();
      for(let j=i+1;j<items.length;j++){
        const b=items[j].getBoundingClientRect();
        const ix=Math.min(a.right,b.right)-Math.max(a.left,b.left);
        const iy=Math.min(a.bottom,b.bottom)-Math.max(a.top,b.top);
        if(ix>0.8&&iy>0.8)count++;
      }
    }
  }
  return count;
}

newPage();
for(const b of BLOCKS){
  if(b.kind==='visual'){
    if(!b.span&&pendingSpanFloats.length>0){
      if(pageHasContent())newPage();
      else flushPendingSpanFloats();
    }
    placeVisual(b);
  }else{
    placeText(b);
  }
}
if(pendingSpanFloats.length>0){
  if(pageHasContent())newPage();
  else flushPendingSpanFloats();
}
const quality=analyzePageQuality();
document.body.dataset.pageCount=String(pages.length);
document.body.dataset.overlapCount=String(countOverlaps());
document.body.dataset.splitCount=String(splitCount);
document.body.dataset.visualCount=String(visualCount);
document.body.dataset.overflowCount=String(overflowCount);
document.body.dataset.outOfBoundsCount=String(quality.outOfBounds);
document.body.dataset.sparsePageCount=String(quality.sparse);
document.body.dataset.meanOccupancyBp=String(quality.meanBp);
document.body.dataset.minOccupancyBp=String(quality.minBp);
document.body.dataset.pageOccupancyBp=quality.pageBp.join(',');
document.body.dataset.sparsePages=quality.sparsePages.join(',');
document.body.dataset.layoutReady='1';
");
        html.Append("</script></body></html>");
        return html.ToString();
    }

    private static bool IsUsableSourceFontSize(double size) =>
        double.IsFinite(size) && size >= 5.0 && size <= 72.0;

    private static SourceBox ParseSourceBox(JsonElement region, int pageIndex)
    {
        var bbox = region.TryGetProperty("bbox", out var value) ? value : default;
        return new SourceBox(
            pageIndex,
            GetInt(region, "column", -1),
            GetInt(region, "reading_order", -1),
            GetDouble(bbox, "x", 0),
            GetDouble(bbox, "y", 0),
            GetDouble(bbox, "width", 0),
            GetDouble(bbox, "height", 0),
            GetDouble(region, "font_size", 0),
            GetString(region, "span_mode"),
            GetString(region, "region_id"),
            GetString(region, "parent_region_id"),
            GetString(region, "type"),
            GetString(region, "text"));
    }

    private static int FindNearestReadingOrder(SourceBox visual, IReadOnlyList<SourceBox> semantics)
    {
        var nearest = semantics
            .Where(region => region.ReadingOrder >= 0)
            .Select(region => new
            {
                region.ReadingOrder,
                Distance = Math.Abs((region.Y + region.Height * 0.5) - (visual.Y + visual.Height * 0.5)) +
                           Math.Abs((region.X + region.Width * 0.5) - (visual.X + visual.Width * 0.5)) * 0.18,
            })
            .OrderBy(item => item.Distance)
            .FirstOrDefault();
        return nearest?.ReadingOrder ?? (int)Math.Round(visual.Y * 10);
    }

    private static int InferColumn(SourceBox box, double pageWidth)
    {
        if (box.Column >= 0) return box.Column;
        return box.X + box.Width * 0.5 < pageWidth * 0.5 ? 0 : 1;
    }

    private static SourceBox Union(IReadOnlyList<SourceBox> regions)
    {
        var first = regions[0];
        var left = regions.Min(r => r.X);
        var top = regions.Min(r => r.Y);
        var right = regions.Max(r => r.X + r.Width);
        var bottom = regions.Max(r => r.Y + r.Height);
        return first with
        {
            X = left,
            Y = top,
            Width = right - left,
            Height = bottom - top,
            FontSize = Median(regions.Select(r => r.FontSize).Where(v => v > 0).ToArray(), first.FontSize),
            Text = string.Join(" ", regions.Select(r => r.Text)),
        };
    }

    private static double IntersectionRatio(SourceBox a, SourceBox b)
    {
        var width = Math.Max(0, Math.Min(a.X + a.Width, b.X + b.Width) - Math.Max(a.X, b.X));
        var height = Math.Max(0, Math.Min(a.Y + a.Height, b.Y + b.Height) - Math.Max(a.Y, b.Y));
        var intersection = width * height;
        var area = Math.Max(1, a.Width * a.Height);
        return intersection / area;
    }

    private static double HorizontalOverlap(SourceBox a, SourceBox b) =>
        Math.Max(0, Math.Min(a.X + a.Width, b.X + b.Width) - Math.Max(a.X, b.X));

    private static double VerticalDistance(SourceBox a, SourceBox b)
    {
        if (a.Y + a.Height < b.Y) return b.Y - (a.Y + a.Height);
        if (b.Y + b.Height < a.Y) return a.Y - (b.Y + b.Height);
        return 0;
    }

    private static double Overlap(double a0, double a1, double b0, double b1) =>
        Math.Max(0, Math.Min(a1, b1) - Math.Max(a0, b0));

    private static double Gap(double a0, double a1, double b0, double b1)
    {
        if (a1 < b0) return b0 - a1;
        if (b1 < a0) return a0 - b1;
        return 0;
    }

    private static double Percentile(double[] values, double percentile, double fallback)
    {
        if (values.Length == 0) return fallback;
        Array.Sort(values);
        var position = Math.Clamp(percentile, 0, 1) * (values.Length - 1);
        var lower = (int)Math.Floor(position);
        var upper = (int)Math.Ceiling(position);
        if (lower == upper) return values[lower];
        var weight = position - lower;
        return values[lower] * (1 - weight) + values[upper] * weight;
    }

    private static double Median(double[] values, double fallback)
    {
        if (values.Length == 0) return fallback;
        Array.Sort(values);
        var mid = values.Length / 2;
        return values.Length % 2 == 0 ? (values[mid - 1] + values[mid]) * 0.5 : values[mid];
    }

    private static double Clamp(double value, double min, double max) =>
        Math.Max(min, Math.Min(max, value));

    private static string NormalizeText(string text) =>
        Regex.Replace(text.Replace("\r\n", "\n").Replace('\r', '\n'), "[ \t]+", " ").Trim();

    private static string BuildOutputPath(string sourcePath, string paperDirectory)
    {
        if (!string.IsNullOrWhiteSpace(sourcePath))
        {
            try
            {
                var directory = Path.GetDirectoryName(sourcePath);
                var stem = Path.GetFileNameWithoutExtension(sourcePath);
                if (!string.IsNullOrWhiteSpace(directory) &&
                    Directory.Exists(directory) &&
                    !string.IsNullOrWhiteSpace(stem))
                    return Path.Combine(directory, stem + "_zh-CN.pdf");
            }
            catch
            {
            }
        }
        return Path.Combine(paperDirectory, "translated_zh-CN.pdf");
    }

    private static string? FindBrowser()
    {
        string[] candidates =
        [
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Microsoft", "Edge", "Application", "msedge.exe"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Microsoft", "Edge", "Application", "msedge.exe"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "Google", "Chrome", "Application", "chrome.exe"),
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Google", "Chrome", "Application", "chrome.exe"),
        ];
        return candidates.FirstOrDefault(File.Exists);
    }

    private static async Task<(int ExitCode, string StdOut, string StdErr)> RunBrowserAsync(
        string browser,
        IReadOnlyList<string> arguments,
        CancellationToken cancellationToken)
    {
        var start = new ProcessStartInfo
        {
            FileName = browser,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        foreach (var argument in arguments) start.ArgumentList.Add(argument);

        using var process = Process.Start(start)
            ?? throw new InvalidOperationException("无法启动浏览器排版引擎。");
        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeoutCts.CancelAfter(TimeSpan.FromSeconds(40));
        var stdoutTask = process.StandardOutput.ReadToEndAsync();
        var stderrTask = process.StandardError.ReadToEndAsync();

        try
        {
            await process.WaitForExitAsync(timeoutCts.Token);
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            try { process.Kill(entireProcessTree: true); } catch { }
            throw new TimeoutException("Layout Engine V2 浏览器排版超过 40 秒，已自动终止。");
        }

        cancellationToken.ThrowIfCancellationRequested();
        return (process.ExitCode, await stdoutTask, await stderrTask);
    }

    private static int ParseDataInt(string dom, string name, int fallback)
    {
        if (string.IsNullOrWhiteSpace(dom)) return fallback;
        var pattern = $"data-{Regex.Escape(name)}=[\"'](?<n>-?\\d+)[\"']";
        var match = Regex.Match(dom, pattern, RegexOptions.IgnoreCase);
        return match.Success && int.TryParse(match.Groups["n"].Value, out var value) ? value : fallback;
    }

    private static SortedDictionary<int, string[]> ParsePageSemanticSummary(string dom)
    {
        var result = new SortedDictionary<int, string[]>();
        if (string.IsNullOrWhiteSpace(dom)) return result;

        var pageMatches = Regex.Matches(
            dom,
            "<section[^>]*data-page=[\"'](?<page>\\d+)[\"'][^>]*>(?<body>.*?)</section>",
            RegexOptions.IgnoreCase | RegexOptions.Singleline);

        foreach (Match pageMatch in pageMatches)
        {
            if (!int.TryParse(pageMatch.Groups["page"].Value, out var page)) continue;
            var counts = Regex.Matches(
                    pageMatch.Groups["body"].Value,
                    "data-semantic=[\"'](?<semantic>[^\"']+)[\"']",
                    RegexOptions.IgnoreCase)
                .Cast<Match>()
                .Select(match => match.Groups["semantic"].Value)
                .GroupBy(value => value, StringComparer.OrdinalIgnoreCase)
                .OrderBy(group => group.Key, StringComparer.OrdinalIgnoreCase)
                .Select(group => $"{group.Key}:{group.Count()}")
                .ToArray();
            result[page] = counts;
        }

        return result;
    }

    private static int[] ParseDataCsvInts(string dom, string name)
    {
        if (string.IsNullOrWhiteSpace(dom)) return Array.Empty<int>();
        var pattern = $"data-{Regex.Escape(name)}=[\"'](?<v>[0-9, -]*)[\"']";
        var match = Regex.Match(dom, pattern, RegexOptions.IgnoreCase);
        if (!match.Success) return Array.Empty<int>();
        return match.Groups["v"].Value
            .Split(',', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
            .Select(value => int.TryParse(value, out var parsed) ? parsed : int.MinValue)
            .Where(value => value != int.MinValue)
            .ToArray();
    }

    private static string Pt(double value) =>
        value.ToString("0.###", CultureInfo.InvariantCulture) + "pt";

    private static string GetString(JsonElement element, string property) =>
        element.ValueKind == JsonValueKind.Object &&
        element.TryGetProperty(property, out var value) &&
        value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? string.Empty
            : string.Empty;

    private static int GetInt(JsonElement element, string property, int fallback) =>
        element.ValueKind == JsonValueKind.Object &&
        element.TryGetProperty(property, out var value) &&
        value.TryGetInt32(out var result)
            ? result
            : fallback;

    private static double GetDouble(JsonElement element, string property, double fallback) =>
        element.ValueKind == JsonValueKind.Object &&
        element.TryGetProperty(property, out var value) &&
        value.TryGetDouble(out var result)
            ? result
            : fallback;

    private static string Compact(string value)
    {
        value = Regex.Replace(value ?? string.Empty, "\\s+", " ").Trim();
        return value.Length <= 900 ? value : value[..900] + "…";
    }

    private static void TryDelete(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); } catch { }
    }

    private static void TryDeleteDirectory(string path)
    {
        try { if (Directory.Exists(path)) Directory.Delete(path, true); } catch { }
    }
}
