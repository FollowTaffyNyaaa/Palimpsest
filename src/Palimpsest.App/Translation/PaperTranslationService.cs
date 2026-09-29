using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace Palimpsest_App.Translation;

public sealed record TranslationProgressInfo(
    int Total,
    int Completed,
    int Cached,
    string Message)
{
    public double Percent => Total <= 0 ? 0 : Completed * 100.0 / Total;
}

public sealed record TranslationRunResult(
    int Total,
    int Translated,
    int CacheHits,
    string PaperIrPath,
    string MarkdownPath);

public sealed class PaperTranslationService
{
    private const int MaxBatchItems = 20;
    private const int MaxBatchCharacters = 18_000;

    private readonly ITranslationProvider _provider;

    public PaperTranslationService(ITranslationProvider provider)
    {
        _provider = provider;
    }

    public async Task<TranslationRunResult> TranslateAsync(
        string paperIrPath,
        IProgress<TranslationProgressInfo>? progress = null,
        CancellationToken cancellationToken = default)
    {
        if (!File.Exists(paperIrPath))
            throw new FileNotFoundException("没有找到 paper_ir.json，请先分析论文。", paperIrPath);

        var root = JsonNode.Parse(await File.ReadAllTextAsync(paperIrPath, cancellationToken))?.AsObject()
            ?? throw new InvalidOperationException("paper_ir.json 不是有效的 JSON。" );
        var paragraphs = root["paragraphs"]?.AsArray()
            ?? throw new InvalidOperationException("paper_ir.json 中没有 paragraphs。" );

        var outputDirectory = Path.GetDirectoryName(paperIrPath) ?? ".";
        var cache = await TranslationCache.LoadAsync(
            Path.Combine(outputDirectory, "translation_cache.json"),
            cancellationToken);

        var backup = Path.Combine(outputDirectory, "paper_ir.source.json");
        if (!File.Exists(backup)) File.Copy(paperIrPath, backup);

        var translatableParagraphs = paragraphs
            .OfType<JsonObject>()
            .Where(p => IsTranslatableSemanticType(p["semantic_type"]?.GetValue<string>()))
            .Where(p => !string.IsNullOrWhiteSpace(SourceText(p)))
            .ToList();

        var total = translatableParagraphs.Count;
        var completed = 0;
        var cacheHits = 0;
        var pending = new List<(JsonObject Paragraph, string Text, string CacheKey)>();

        foreach (var paragraph in translatableParagraphs)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var text = SourceText(paragraph);
            var key = TranslationCache.MakeKey(text, _provider.Id, "zh-CN", "v1");
            var existingTranslation = paragraph["translated_text"]?.GetValue<string>();
            var existingProvider = paragraph["translation_provider"]?.GetValue<string>();
            var existingTarget = paragraph["translation_target"]?.GetValue<string>();
            if (!string.IsNullOrWhiteSpace(existingTranslation) &&
                string.Equals(existingProvider, _provider.Id, StringComparison.OrdinalIgnoreCase) &&
                string.Equals(existingTarget, "zh-CN", StringComparison.OrdinalIgnoreCase))
            {
                paragraph["translation_state"] = "COMPLETED";
                await cache.PutAndSaveAsync(key, existingTranslation.Trim(), cancellationToken);
                completed++;
                cacheHits++;
                continue;
            }
            if (cache.TryGet(key, out var cached))
            {
                paragraph["translated_text"] = cached;
                paragraph["translation_state"] = "COMPLETED";
                paragraph["translation_provider"] = _provider.Id;
                paragraph["translation_target"] = "zh-CN";
                completed++;
                cacheHits++;
            }
            else
            {
                pending.Add((paragraph, text, key));
            }
        }

        await SavePaperIrAsync(root, paperIrPath, cancellationToken);
        progress?.Report(new TranslationProgressInfo(total, completed, cacheHits, $"已命中缓存 {cacheHits} 段。"));

        foreach (var batch in BuildBatches(pending))
        {
            cancellationToken.ThrowIfCancellationRequested();
            progress?.Report(new TranslationProgressInfo(total, completed, cacheHits,
                $"正在用 {_provider.DisplayName} 翻译第 {completed + 1}–{completed + batch.Count} 段…"));

            IReadOnlyList<string> translated;
            try
            {
                translated = await _provider.TranslateBatchAsync(
                    batch.Select(item => item.Text).ToArray(),
                    cancellationToken);
            }
            catch
            {
                foreach (var item in batch) item.Paragraph["translation_state"] = "FAILED";
                await SavePaperIrAsync(root, paperIrPath, cancellationToken);
                throw;
            }

            for (var index = 0; index < batch.Count; index++)
            {
                var item = batch[index];
                var translatedText = translated[index].Trim();
                var semanticType = item.Paragraph["semantic_type"]?.GetValue<string>();
                if (semanticType is not null &&
                    (semanticType.Equals("HEADING", StringComparison.OrdinalIgnoreCase) ||
                     semanticType.Equals("APPENDIX_HEADING", StringComparison.OrdinalIgnoreCase)))
                {
                    translatedText = PreserveHeadingPrefix(item.Text, translatedText);
                }
                item.Paragraph["translated_text"] = translatedText;
                item.Paragraph["translation_state"] = "COMPLETED";
                item.Paragraph["translation_provider"] = _provider.Id;
                item.Paragraph["translation_target"] = "zh-CN";
                await cache.PutAndSaveAsync(item.CacheKey, translatedText, cancellationToken);
                completed++;
                progress?.Report(new TranslationProgressInfo(total, completed, cacheHits,
                    $"已完成 {completed}/{total} 段。"));
            }

            await SavePaperIrAsync(root, paperIrPath, cancellationToken);
            await WriteMarkdownAsync(translatableParagraphs, Path.Combine(outputDirectory, "translated_body.md"), cancellationToken);
        }

        var markdownPath = Path.Combine(outputDirectory, "translated_body.md");
        await WriteMarkdownAsync(translatableParagraphs, markdownPath, cancellationToken);
        await SavePaperIrAsync(root, paperIrPath, cancellationToken);

        return new TranslationRunResult(total, total - cacheHits, cacheHits, paperIrPath, markdownPath);
    }

    private static bool IsTranslatableSemanticType(string? semanticType)
    {
        return semanticType is not null && (
            semanticType.Equals("ABSTRACT", StringComparison.OrdinalIgnoreCase) ||
            semanticType.Equals("HEADING", StringComparison.OrdinalIgnoreCase) ||
            semanticType.Equals("BODY", StringComparison.OrdinalIgnoreCase) ||
            semanticType.Equals("APPENDIX_BODY", StringComparison.OrdinalIgnoreCase) ||
            semanticType.Equals("APPENDIX_HEADING", StringComparison.OrdinalIgnoreCase));
    }

    private static readonly Regex HeadingPrefixRegex = new(
        @"^\s*((?:(?:[IVXLCDM]+|\d+(?:\.\d+)*|[A-Z])(?:[\.)])?)\s+)",
        RegexOptions.Compiled | RegexOptions.CultureInvariant | RegexOptions.IgnoreCase);

    private static string PreserveHeadingPrefix(string source, string translated)
    {
        var sourceMatch = HeadingPrefixRegex.Match(source);
        if (!sourceMatch.Success) return translated.Trim();

        var prefix = sourceMatch.Groups[1].Value;
        var translatedMatch = HeadingPrefixRegex.Match(translated);
        var body = translatedMatch.Success
            ? translated[translatedMatch.Length..].TrimStart()
            : translated.Trim();

        return string.IsNullOrWhiteSpace(body) ? prefix.TrimEnd() : prefix + body;
    }

    private static string SourceText(JsonObject paragraph)
    {
        var normalized = paragraph["normalized_text"]?.GetValue<string>();
        if (!string.IsNullOrWhiteSpace(normalized)) return normalized.Trim();
        return paragraph["source_text"]?.GetValue<string>()?.Trim() ?? string.Empty;
    }

    private static IEnumerable<List<(JsonObject Paragraph, string Text, string CacheKey)>> BuildBatches(
        List<(JsonObject Paragraph, string Text, string CacheKey)> pending)
    {
        var batch = new List<(JsonObject Paragraph, string Text, string CacheKey)>();
        var chars = 0;
        foreach (var item in pending)
        {
            if (batch.Count > 0 && (batch.Count >= MaxBatchItems || chars + item.Text.Length > MaxBatchCharacters))
            {
                yield return batch;
                batch = new List<(JsonObject Paragraph, string Text, string CacheKey)>();
                chars = 0;
            }
            batch.Add(item);
            chars += item.Text.Length;
        }
        if (batch.Count > 0) yield return batch;
    }

    private static async Task SavePaperIrAsync(JsonObject root, string path, CancellationToken cancellationToken)
    {
        var temp = path + ".tmp";
        await File.WriteAllTextAsync(temp, root.ToJsonString(new JsonSerializerOptions { WriteIndented = true }), cancellationToken);
        File.Move(temp, path, true);
    }

    private static async Task WriteMarkdownAsync(
        IReadOnlyList<JsonObject> paragraphs,
        string path,
        CancellationToken cancellationToken)
    {
        var builder = new StringBuilder();
        builder.AppendLine("# Palimpsest translated content").AppendLine();
        foreach (var paragraph in paragraphs)
        {
            var id = paragraph["paragraph_id"]?.GetValue<string>() ?? "";
            var translated = paragraph["translated_text"]?.GetValue<string>();
            if (string.IsNullOrWhiteSpace(translated)) continue;
            builder.Append("<!-- ").Append(id).AppendLine(" -->");
            builder.AppendLine(translated.Trim()).AppendLine();
        }
        await File.WriteAllTextAsync(path, builder.ToString(), new UTF8Encoding(false), cancellationToken);
    }

    private sealed class TranslationCache
    {
        private readonly string _path;
        private readonly Dictionary<string, string> _items;

        private TranslationCache(string path, Dictionary<string, string> items)
        {
            _path = path;
            _items = items;
        }

        public static async Task<TranslationCache> LoadAsync(string path, CancellationToken cancellationToken)
        {
            if (!File.Exists(path))
                return new TranslationCache(path, new Dictionary<string, string>(StringComparer.Ordinal));
            try
            {
                await using var stream = File.OpenRead(path);
                var items = await JsonSerializer.DeserializeAsync<Dictionary<string, string>>(stream, cancellationToken: cancellationToken)
                    ?? new Dictionary<string, string>(StringComparer.Ordinal);
                return new TranslationCache(path, items);
            }
            catch (JsonException)
            {
                return new TranslationCache(path, new Dictionary<string, string>(StringComparer.Ordinal));
            }
        }

        public bool TryGet(string key, out string translated) => _items.TryGetValue(key, out translated!);

        public async Task PutAndSaveAsync(string key, string translated, CancellationToken cancellationToken)
        {
            _items[key] = translated;
            var temp = _path + ".tmp";
            await File.WriteAllTextAsync(temp, JsonSerializer.Serialize(_items, new JsonSerializerOptions { WriteIndented = true }), cancellationToken);
            File.Move(temp, _path, true);
        }

        public static string MakeKey(string source, string provider, string target, string settingsVersion)
        {
            var bytes = Encoding.UTF8.GetBytes(source + "\n" + provider + "\n" + target + "\n" + settingsVersion);
            return Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
        }
    }
}

