using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Net;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace Palimpsest_App.Translation;

public sealed record ChinesePdfComposeResult(
    string OutputPath,
    int PageCount,
    int ParagraphCount,
    int OverflowParagraphs,
    string QaPath);

public static class ChinesePdfComposer
{
    private sealed record PdfPage(int PageIndex, double Width, double Height, string BackgroundAsset);
    private sealed record FragmentSpec(
        string ElementId,
        int PageIndex,
        double X,
        double Y,
        double Width,
        double Height,
        double FontSize,
        bool Bold);
    private sealed record ParagraphSpec(string ParagraphId, string Text, IReadOnlyList<FragmentSpec> Fragments);

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

        var pages = root.GetProperty("pages")
            .EnumerateArray()
            .Select(ParsePage)
            .OrderBy(page => page.PageIndex)
            .ToArray();

        if (pages.Length == 0)
            throw new InvalidOperationException("PaperIR 中没有页面。");

        var firstWidth = pages[0].Width;
        var firstHeight = pages[0].Height;
        if (pages.Any(page => Math.Abs(page.Width - firstWidth) > 0.5 || Math.Abs(page.Height - firstHeight) > 0.5))
            throw new InvalidOperationException("当前 Windows PDF Composer 暂不支持同一论文混合页面尺寸。");

        var paragraphSpecs = new List<ParagraphSpec>();
        foreach (var paragraph in root.GetProperty("paragraphs").EnumerateArray())
        {
            cancellationToken.ThrowIfCancellationRequested();

            var translated = GetString(paragraph, "translated_text").Trim();
            if (string.IsNullOrWhiteSpace(translated)) continue;

            var paragraphId = GetString(paragraph, "paragraph_id");
            var semanticType = GetString(paragraph, "semantic_type");
            var originalFontSize = GetDouble(paragraph, "original_font_size", 9.5);
            var bold = semanticType is "APPENDIX_HEADING" or "HEADING" or "TITLE";
            var fragments = ParseFragments(paragraph, paragraphId, originalFontSize, bold);

            if (fragments.Count == 0)
            {
                throw new InvalidOperationException(
                    $"段落 {paragraphId} 没有 layout_fragments。请用当前版本重新执行一次“分析论文结构”，再进行翻译。");
            }

            paragraphSpecs.Add(new ParagraphSpec(
                paragraphId,
                NormalizeTranslatedText(translated),
                fragments));
        }

        var sourcePath = GetString(root, "source_path");
        var outputPath = BuildOutputPath(sourcePath, paperDirectory);
        var qaPath = Path.Combine(paperDirectory, "pdf_export_qa.json");
        var htmlPath = Path.Combine(paperDirectory, "_palimpsest_pdf_compose.html");
        var browserProfile = Path.Combine(paperDirectory, "_palimpsest_browser_profile");

        Directory.CreateDirectory(Path.GetDirectoryName(outputPath) ?? paperDirectory);
        TryDeleteDirectory(browserProfile);
        Directory.CreateDirectory(browserProfile);

        var html = BuildHtml(pages, paragraphSpecs, paperDirectory, firstWidth, firstHeight);
        await File.WriteAllTextAsync(htmlPath, html, new UTF8Encoding(false), cancellationToken);

        var browser = FindBrowser()
            ?? throw new FileNotFoundException("没有找到 Microsoft Edge 或 Google Chrome，无法生成最终 PDF。");

        var fileUri = new Uri(Path.GetFullPath(htmlPath)).AbsoluteUri;
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
                "--virtual-time-budget=1200",
                $"--user-data-dir={browserProfile}",
                "--dump-dom",
                fileUri,
            ],
            cancellationToken);

        var overflowParagraphs = ParseOverflowCount(dump.StdOut);

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
                "--virtual-time-budget=1200",
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
                $"浏览器 PDF 导出失败 (exit={print.ExitCode})：{Compact(print.StdErr + " " + print.StdOut)}");
        }

        var qa = new
        {
            output_pdf = outputPath,
            pages = pages.Length,
            translated_paragraphs = paragraphSpecs.Count,
            overflow_paragraphs = overflowParagraphs,
            text_layer = true,
            background_mode = "clean_page_template",
            browser = browser,
            paper_ir_schema = GetString(root, "schema_version"),
        };
        await File.WriteAllTextAsync(
            qaPath,
            JsonSerializer.Serialize(qa, new JsonSerializerOptions { WriteIndented = true }),
            cancellationToken);

        TryDelete(htmlPath);
        TryDeleteDirectory(browserProfile);

        return new ChinesePdfComposeResult(
            outputPath,
            pages.Length,
            paragraphSpecs.Count,
            overflowParagraphs,
            qaPath);
    }

    private static string BuildHtml(
        IReadOnlyList<PdfPage> pages,
        IReadOnlyList<ParagraphSpec> paragraphs,
        string paperDirectory,
        double pageWidth,
        double pageHeight)
    {
        var paragraphData = paragraphs.Select(paragraph => new
        {
            id = paragraph.ParagraphId,
            text = paragraph.Text,
            fragments = paragraph.Fragments.Select(fragment => fragment.ElementId).ToArray(),
        }).ToArray();

        var json = JsonSerializer.Serialize(paragraphData).Replace("</", "<\\/");
        var html = new StringBuilder(128 * 1024);
        html.AppendLine("<!doctype html>");
        html.AppendLine("<html><head><meta charset=\"utf-8\">");
        html.Append("<style>");
        html.Append("@page{size:")
            .Append(Pt(pageWidth)).Append(' ').Append(Pt(pageHeight))
            .Append(";margin:0}");
        html.Append("html,body{margin:0;padding:0;background:#fff;-webkit-print-color-adjust:exact;print-color-adjust:exact}");
        html.Append(".page{position:relative;width:").Append(Pt(pageWidth))
            .Append(";height:").Append(Pt(pageHeight))
            .Append(";overflow:hidden;break-after:page;page-break-after:always;background:#fff}");
        html.Append(".page:last-child{break-after:auto;page-break-after:auto}");
        html.Append(".bg{position:absolute;left:0;top:0;width:100%;height:100%;object-fit:fill}");
        html.Append(".fragment{position:absolute;box-sizing:border-box;margin:0;padding:0;color:#000;overflow:hidden;");
        html.Append("font-family:\"Times New Roman\",\"Noto Serif SC\",\"Songti SC\",SimSun,serif;");
        html.Append("font-weight:400;line-height:1.18;font-kerning:normal;text-rendering:optimizeLegibility;font-synthesis:none;white-space:pre-wrap;word-break:normal;overflow-wrap:anywhere;text-align:left}");
        html.Append(".fragment.bold{font-weight:700}");
        html.Append("</style></head><body>");

        var allFragments = paragraphs.SelectMany(paragraph => paragraph.Fragments).ToArray();
        foreach (var page in pages)
        {
            var bgPath = Path.Combine(paperDirectory, page.BackgroundAsset);
            if (!File.Exists(bgPath))
                throw new FileNotFoundException($"缺少 clean page：{bgPath}", bgPath);

            html.Append("<section class=\"page\" data-page=\"").Append(page.PageIndex).Append("\">");
            html.Append("<img class=\"bg\" src=\"")
                .Append(WebUtility.HtmlEncode(new Uri(Path.GetFullPath(bgPath)).AbsoluteUri))
                .Append("\">");

            foreach (var fragment in allFragments.Where(fragment => fragment.PageIndex == page.PageIndex))
            {
                var effectiveHeight = fragment.Height + fragment.FontSize * 0.50;
                html.Append("<div id=\"").Append(WebUtility.HtmlEncode(fragment.ElementId))
                    .Append("\" class=\"fragment").Append(fragment.Bold ? " bold" : "")
                    .Append("\" style=\"left:").Append(Pt(fragment.X))
                    .Append(";top:").Append(Pt(Math.Max(0, fragment.Y - fragment.FontSize * 0.04)))
                    .Append(";width:").Append(Pt(Math.Max(1, fragment.Width)))
                    .Append(";height:").Append(Pt(Math.Max(fragment.FontSize * 1.15, effectiveHeight)))
                    .Append(";font-size:").Append(Pt(Math.Max(6.0, fragment.FontSize)))
                    .Append(";\"></div>");
            }

            html.Append("</section>");
        }

        html.Append("<script>");
        html.Append("const DATA=").Append(json).Append(';');
        html.Append(@"
function normalizeStart(s){return s.replace(/^s+/u,'');}
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
  return Math.max(1,c);
}
function fits(el){
  return el.scrollHeight<=el.clientHeight+1&&el.scrollWidth<=el.clientWidth+1;
}
function put(el,text){
  if(!text)return 0;
  let lo=1,hi=text.length,best=0;
  while(lo<=hi){
    const mid=Math.floor((lo+hi)/2);
    el.textContent=text.slice(0,mid);
    if(fits(el)){best=mid;lo=mid+1}else{hi=mid-1}
  }
  if(best<=0){el.textContent='';return 0;}
  let cut=safeCut(text,best);
  if(cut<Math.floor(best*0.65))cut=best;
  el.textContent=text.slice(0,cut).trimEnd();
  while(!fits(el)&&cut>1){
    cut--;
    el.textContent=text.slice(0,cut).trimEnd();
  }
  return cut;
}
let overflows=0;
for(const p of DATA){
  let rest=normalizeStart(p.text||'');
  for(const id of p.fragments){
    if(!rest)break;
    const el=document.getElementById(id);
    if(!el)continue;
    const used=put(el,rest);
    if(used<=0)continue;
    rest=normalizeStart(rest.slice(used));
  }
  if(rest)overflows++;
}
document.body.dataset.overflowCount=String(overflows);
document.body.dataset.layoutReady='1';
");
        html.Append("</script></body></html>");
        return html.ToString();
    }

    private static List<FragmentSpec> ParseFragments(
        JsonElement paragraph,
        string paragraphId,
        double fallbackFontSize,
        bool bold)
    {
        var result = new List<FragmentSpec>();
        if (paragraph.TryGetProperty("layout_fragments", out var fragments) &&
            fragments.ValueKind == JsonValueKind.Array)
        {
            var index = 0;
            foreach (var fragment in fragments.EnumerateArray())
            {
                if (!fragment.TryGetProperty("bbox", out var bbox)) continue;
                result.Add(new FragmentSpec(
                    $"frag_{SanitizeId(paragraphId)}_{index++}",
                    GetInt(fragment, "page_index", 0),
                    GetDouble(bbox, "x", 0),
                    GetDouble(bbox, "y", 0),
                    GetDouble(bbox, "width", 0),
                    GetDouble(bbox, "height", 0),
                    GetDouble(fragment, "font_size", fallbackFontSize),
                    bold));
            }
        }

        if (result.Count > 0) return result;

        if (paragraph.TryGetProperty("preferred_bbox", out var preferred) &&
            paragraph.TryGetProperty("page_span", out var pageSpan) &&
            pageSpan.ValueKind == JsonValueKind.Array &&
            pageSpan.GetArrayLength() == 1)
        {
            result.Add(new FragmentSpec(
                $"frag_{SanitizeId(paragraphId)}_0",
                pageSpan[0].GetInt32(),
                GetDouble(preferred, "x", 0),
                GetDouble(preferred, "y", 0),
                GetDouble(preferred, "width", 0),
                GetDouble(preferred, "height", 0),
                fallbackFontSize,
                bold));
        }

        return result;
    }

    private static PdfPage ParsePage(JsonElement page) =>
        new(
            GetInt(page, "page_index", 0),
            GetDouble(page, "width", 612),
            GetDouble(page, "height", 792),
            GetString(page, "background_asset"));

    private static string BuildOutputPath(string sourcePath, string paperDirectory)
    {
        if (!string.IsNullOrWhiteSpace(sourcePath))
        {
            try
            {
                var sourceDirectory = Path.GetDirectoryName(sourcePath);
                var sourceStem = Path.GetFileNameWithoutExtension(sourcePath);
                if (!string.IsNullOrWhiteSpace(sourceDirectory) &&
                    Directory.Exists(sourceDirectory) &&
                    !string.IsNullOrWhiteSpace(sourceStem))
                {
                    return Path.Combine(sourceDirectory, sourceStem + "_zh-CN.pdf");
                }
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
            ?? throw new InvalidOperationException("无法启动浏览器 PDF Composer。");
        using var timeoutCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeoutCts.CancelAfter(TimeSpan.FromSeconds(30));
        var stdoutTask = process.StandardOutput.ReadToEndAsync();
        var stderrTask = process.StandardError.ReadToEndAsync();
        try
        {
            await process.WaitForExitAsync(timeoutCts.Token);
        }
        catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
        {
            try { process.Kill(entireProcessTree: true); } catch { }
            throw new TimeoutException("浏览器 PDF Composer 超过 30 秒未完成，已自动终止。");
        }
        cancellationToken.ThrowIfCancellationRequested();
        return (process.ExitCode, await stdoutTask, await stderrTask);
    }

    private static int ParseOverflowCount(string dom)
    {
        if (string.IsNullOrWhiteSpace(dom)) return -1;
        var match = Regex.Match(dom, "data-overflow-count=[\"'](?<n>\\d+)[\"']", RegexOptions.IgnoreCase);
        return match.Success && int.TryParse(match.Groups["n"].Value, out var count) ? count : -1;
    }

    private static string NormalizeTranslatedText(string text) =>
        Regex.Replace(text.Replace("\r\n", "\n").Replace('\r', '\n'), "[ \t]+", " ").Trim();

    private static string SanitizeId(string value)
    {
        var builder = new StringBuilder(value.Length);
        foreach (var ch in value)
            builder.Append(char.IsLetterOrDigit(ch) || ch is '_' or '-' ? ch : '_');
        return builder.ToString();
    }

    private static string Pt(double value) =>
        value.ToString("0.###", CultureInfo.InvariantCulture) + "pt";

    private static string GetString(JsonElement element, string property) =>
        element.TryGetProperty(property, out var value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? string.Empty
            : string.Empty;

    private static int GetInt(JsonElement element, string property, int fallback) =>
        element.TryGetProperty(property, out var value) && value.TryGetInt32(out var result)
            ? result
            : fallback;

    private static double GetDouble(JsonElement element, string property, double fallback) =>
        element.TryGetProperty(property, out var value) && value.TryGetDouble(out var result)
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
