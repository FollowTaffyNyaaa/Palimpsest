using System;
using System.Collections.Generic;
using System.Linq;
using System.Net;
using System.Net.Http;
using System.Net.Http.Json;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using GTranslate.Translators;

namespace Palimpsest_App.Translation;

internal static class FreeTranslationTextChunker
{
    public static IReadOnlyList<string> Split(string text, int maxChars)
    {
        if (text.Length <= maxChars) return [text];

        var chunks = new List<string>();
        var remaining = text.Trim();
        while (remaining.Length > maxChars)
        {
            var cut = FindCut(remaining, maxChars);
            chunks.Add(remaining[..cut].Trim());
            remaining = remaining[cut..].TrimStart();
        }
        if (remaining.Length > 0) chunks.Add(remaining);
        return chunks;
    }

    private static int FindCut(string text, int maxChars)
    {
        var searchStart = Math.Min(maxChars, text.Length - 1);
        for (var i = searchStart; i >= Math.Max(1, maxChars / 2); i--)
        {
            if (text[i] is '.' or '!' or '?' or ';' or ':' or '\n')
                return i + 1;
        }
        for (var i = searchStart; i >= Math.Max(1, maxChars / 2); i--)
        {
            if (char.IsWhiteSpace(text[i])) return i;
        }
        return maxChars;
    }
}

public sealed class GoogleFreeTranslationProvider : ITranslationProvider
{
    private readonly GoogleTranslator _translator = new();

    public string Id => "google-free";
    public string DisplayName => "Google 翻译（免费）";

    public async Task<IReadOnlyList<string>> TranslateBatchAsync(
        IReadOnlyList<string> texts,
        CancellationToken cancellationToken = default)
    {
        var output = new List<string>(texts.Count);
        foreach (var text in texts)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var translated = new StringBuilder();
            foreach (var chunk in FreeTranslationTextChunker.Split(text, 3500))
            {
                cancellationToken.ThrowIfCancellationRequested();
                var result = await _translator.TranslateAsync(chunk, "zh-CN", "en");
                translated.Append(result.Translation);
            }
            output.Add(translated.ToString());
            await Task.Delay(80, cancellationToken);
        }
        return output;
    }
}

public sealed class BingFreeTranslationProvider : ITranslationProvider
{
    private sealed record BingSession(string Ig, string Key, string Token, DateTimeOffset ExpiresAt);

    private static readonly CookieContainer Cookies = new();
    private static readonly HttpClient Http = new(new HttpClientHandler
    {
        CookieContainer = Cookies,
        UseCookies = true,
        AllowAutoRedirect = true,
        AutomaticDecompression = DecompressionMethods.All,
    })
    {
        Timeout = TimeSpan.FromSeconds(16),
    };

    private static readonly SemaphoreSlim SessionGate = new(1, 1);
    private static readonly Regex IgRegex = new(
        @"window\._G\s*=\s*\{.*?\bIG:\s*""(?<ig>[^""]+)""",
        RegexOptions.IgnoreCase | RegexOptions.Singleline | RegexOptions.Compiled);
    private static readonly Regex AbuseRegex = new(
        @"params_AbusePreventionHelper\s*=\s*\[\s*(?<key>\d+)\s*,\s*""(?<token>[^""]+)""\s*,\s*(?<ttl>\d+)",
        RegexOptions.IgnoreCase | RegexOptions.Compiled);

    private static BingSession? _session;

    public string Id => "bing-free-v2";
    public string DisplayName => "Microsoft/Bing 翻译（免费）";

    public async Task<IReadOnlyList<string>> TranslateBatchAsync(
        IReadOnlyList<string> texts,
        CancellationToken cancellationToken = default)
    {
        if (texts.Count == 0) return Array.Empty<string>();

        var output = new string[texts.Count];
        using var concurrency = new SemaphoreSlim(3, 3);

        var tasks = texts.Select(async (text, index) =>
        {
            await concurrency.WaitAsync(cancellationToken);
            try
            {
                output[index] = await TranslateTextAsync(text, cancellationToken);
            }
            finally
            {
                concurrency.Release();
            }
        }).ToArray();

        await Task.WhenAll(tasks);
        return output;
    }

    private static async Task<string> TranslateTextAsync(
        string text,
        CancellationToken cancellationToken)
    {
        var translated = new StringBuilder();

        foreach (var chunk in FreeTranslationTextChunker.Split(text, 900))
        {
            cancellationToken.ThrowIfCancellationRequested();
            translated.Append(await TranslateChunkAsync(chunk, cancellationToken));
            await Task.Delay(70, cancellationToken);
        }

        return translated.ToString();
    }

    private static async Task<string> TranslateChunkAsync(
        string text,
        CancellationToken cancellationToken)
    {
        Exception? lastFailure = null;

        for (var attempt = 0; attempt < 3; attempt++)
        {
            cancellationToken.ThrowIfCancellationRequested();

            var forceRefresh = attempt > 0 && lastFailure is BingSessionExpiredException;
            var session = await GetSessionAsync(forceRefresh, cancellationToken);

            using var request = BuildTranslationRequest(session, text);
            using var response = await Http.SendAsync(request, cancellationToken);
            var raw = await response.Content.ReadAsStringAsync(cancellationToken);

            if (response.StatusCode is HttpStatusCode.Unauthorized or HttpStatusCode.Forbidden)
            {
                InvalidateSession();
                lastFailure = new BingSessionExpiredException(
                    $"Bing 会话失效 ({(int)response.StatusCode})。");
                continue;
            }

            if ((int)response.StatusCode == 429)
            {
                lastFailure = new InvalidOperationException("Bing 免费翻译当前被限流 (429)。");
                if (attempt == 2) throw lastFailure;

                await Task.Delay(
                    TimeSpan.FromMilliseconds(700 * Math.Pow(2, attempt)),
                    cancellationToken);
                continue;
            }

            if (!response.IsSuccessStatusCode)
            {
                lastFailure = new HttpRequestException(
                    $"Bing 返回 {(int)response.StatusCode}：{Compact(raw)}");

                if ((int)response.StatusCode >= 500 && attempt < 2)
                {
                    await Task.Delay(
                        TimeSpan.FromMilliseconds(500 * Math.Pow(2, attempt)),
                        cancellationToken);
                    continue;
                }

                throw new InvalidOperationException(
                    $"Bing 免费翻译请求失败 ({(int)response.StatusCode})：{Compact(raw)}");
            }

            return ParseTranslation(raw);
        }

        throw new InvalidOperationException(
            "Bing 免费翻译连续重试失败。" +
            (lastFailure is null ? string.Empty : " " + lastFailure.Message),
            lastFailure);
    }

    private static HttpRequestMessage BuildTranslationRequest(BingSession session, string text)
    {
        var endpoint =
            $"https://www.bing.com/ttranslatev3?isVertical=1&IG={Uri.EscapeDataString(session.Ig)}&IID=translator.5028.1";

        var request = new HttpRequestMessage(HttpMethod.Post, endpoint);
        request.Headers.UserAgent.ParseAdd(
            "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " +
            "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36");
        request.Headers.Referrer = new Uri("https://www.bing.com/translator");
        request.Headers.Accept.ParseAdd("application/json, text/plain, */*");
        request.Content = new FormUrlEncodedContent(new Dictionary<string, string>
        {
            ["fromLang"] = "en",
            ["to"] = "zh-Hans",
            ["text"] = text,
            ["token"] = session.Token,
            ["key"] = session.Key,
            ["tryFetchingGenderDebiasedTranslations"] = "true",
        });
        return request;
    }

    private static async Task<BingSession> GetSessionAsync(
        bool forceRefresh,
        CancellationToken cancellationToken)
    {
        var current = _session;
        if (!forceRefresh &&
            current is not null &&
            current.ExpiresAt > DateTimeOffset.UtcNow.AddMinutes(2))
            return current;

        await SessionGate.WaitAsync(cancellationToken);
        try
        {
            current = _session;
            if (!forceRefresh &&
                current is not null &&
                current.ExpiresAt > DateTimeOffset.UtcNow.AddMinutes(2))
                return current;

            using var request = new HttpRequestMessage(
                HttpMethod.Get,
                "https://www.bing.com/translator");
            request.Headers.UserAgent.ParseAdd(
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 " +
                "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36");
            request.Headers.Accept.ParseAdd(
                "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8");

            using var response = await Http.SendAsync(request, cancellationToken);
            var html = await response.Content.ReadAsStringAsync(cancellationToken);

            if (!response.IsSuccessStatusCode)
                throw new InvalidOperationException(
                    $"无法初始化 Bing 免费翻译会话 ({(int)response.StatusCode})。");

            var igMatch = IgRegex.Match(html);
            var abuseMatch = AbuseRegex.Match(html);
            if (!igMatch.Success || !abuseMatch.Success)
                throw new InvalidOperationException(
                    "Bing Translator 页面结构已变化，无法读取免费翻译会话参数。");

            var ttlMs = long.TryParse(abuseMatch.Groups["ttl"].Value, out var parsedTtl)
                ? parsedTtl
                : 3_600_000L;
            var safeTtl = TimeSpan.FromMilliseconds(
                Math.Clamp(ttlMs - 300_000L, 600_000L, 3_300_000L));

            current = new BingSession(
                igMatch.Groups["ig"].Value,
                abuseMatch.Groups["key"].Value,
                abuseMatch.Groups["token"].Value,
                DateTimeOffset.UtcNow.Add(safeTtl));
            _session = current;
            return current;
        }
        finally
        {
            SessionGate.Release();
        }
    }

    private static string ParseTranslation(string raw)
    {
        try
        {
            using var json = JsonDocument.Parse(raw);
            var root = json.RootElement;

            if (root.ValueKind != JsonValueKind.Array || root.GetArrayLength() == 0)
                throw new InvalidOperationException("Bing 返回的根节点不是有效数组。");

            var first = root[0];
            if (!first.TryGetProperty("translations", out var translations) ||
                translations.ValueKind != JsonValueKind.Array ||
                translations.GetArrayLength() == 0)
                throw new InvalidOperationException("Bing 返回中没有 translations。");

            var translated = translations[0].TryGetProperty("text", out var text)
                ? text.GetString()
                : null;
            if (string.IsNullOrWhiteSpace(translated))
                throw new InvalidOperationException("Bing 返回了空译文。");

            return translated;
        }
        catch (JsonException ex)
        {
            throw new InvalidOperationException(
                "Bing 返回了无法解析的 JSON：" + Compact(raw),
                ex);
        }
    }

    private static void InvalidateSession() => _session = null;

    private static string Compact(string text)
    {
        text = text.Replace("\r", " ").Replace("\n", " ").Trim();
        return text.Length <= 500 ? text : text[..500];
    }

    private sealed class BingSessionExpiredException : Exception
    {
        public BingSessionExpiredException(string message) : base(message) { }
    }
}

public sealed class TencentTranSmartFreeTranslationProvider : ITranslationProvider
{
    private static readonly HttpClient Http = new()
    {
        Timeout = TimeSpan.FromSeconds(35),
    };

    private static readonly Uri Endpoint = new("https://transmart.qq.com/api/imt");
    private static readonly string ClientKey = Convert.ToBase64String(
        Encoding.UTF8.GetBytes("transmart_crx_Palimpsest/0.3 Windows"))
        .TrimEnd('=')
        .Replace('+', '-')
        .Replace('/', '_')[..Math.Min(64, Convert.ToBase64String(Encoding.UTF8.GetBytes("transmart_crx_Palimpsest/0.3 Windows")).Length)];

    public string Id => "transmart-free";
    public string DisplayName => "腾讯 TranSmart（免费）";

    public async Task<IReadOnlyList<string>> TranslateBatchAsync(
        IReadOnlyList<string> texts,
        CancellationToken cancellationToken = default)
    {
        var result = new List<string>(texts.Count);
        for (var offset = 0; offset < texts.Count; offset += 5)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var batch = texts.Skip(offset).Take(5).ToArray();
            var translated = await TranslateSubBatchAsync(batch, cancellationToken);
            result.AddRange(translated);
            await Task.Delay(100, cancellationToken);
        }
        return result;
    }

    private static async Task<IReadOnlyList<string>> TranslateSubBatchAsync(
        IReadOnlyList<string> texts,
        CancellationToken cancellationToken)
    {
        var payload = new
        {
            header = new
            {
                fn = "auto_translation",
                client_key = ClientKey,
            },
            source = new
            {
                text_list = texts,
                lang = "en",
                orig_url = "palimpsest://local-paper",
            },
            target = new
            {
                lang = "zh",
            },
            type = "plain",
        };

        for (var attempt = 0; attempt < 4; attempt++)
        {
            using var request = new HttpRequestMessage(HttpMethod.Post, Endpoint);
            request.Headers.UserAgent.ParseAdd("Palimpsest/0.3");
            request.Content = JsonContent.Create(payload);

            using var response = await Http.SendAsync(request, cancellationToken);
            var raw = await response.Content.ReadAsStringAsync(cancellationToken);

            if (response.IsSuccessStatusCode)
            {
                using var json = JsonDocument.Parse(raw);
                var root = json.RootElement;
                var retCode = root.TryGetProperty("header", out var header) &&
                              header.TryGetProperty("ret_code", out var ret)
                    ? ret.GetString()
                    : null;
                if (!string.Equals(retCode, "succ", StringComparison.OrdinalIgnoreCase))
                    throw new InvalidOperationException("腾讯 TranSmart 返回错误：" + Compact(raw));

                if (!root.TryGetProperty("auto_translation", out var translations) ||
                    translations.ValueKind != JsonValueKind.Array)
                    throw new InvalidOperationException("腾讯 TranSmart 返回了无法识别的响应。");

                var items = translations.EnumerateArray()
                    .Select(item => item.GetString() ?? string.Empty)
                    .ToArray();
                if (items.Length != texts.Count)
                    throw new InvalidOperationException($"腾讯 TranSmart 返回数量异常：请求 {texts.Count} 段，返回 {items.Length} 段。");
                return items;
            }

            if ((int)response.StatusCode is not (429 or >= 500 and <= 599) || attempt == 3)
                throw new InvalidOperationException($"腾讯 TranSmart 请求失败 ({(int)response.StatusCode})：" + Compact(raw));

            await Task.Delay(TimeSpan.FromMilliseconds(400 * Math.Pow(2, attempt)), cancellationToken);
        }

        throw new InvalidOperationException("腾讯 TranSmart 请求重试失败。");
    }

    private static string Compact(string text)
    {
        text = text.Replace("\r", " ").Replace("\n", " ").Trim();
        return text.Length <= 500 ? text : text[..500];
    }
}

public sealed class AutoFreeTranslationProvider : ITranslationProvider
{
    private sealed record Candidate(
        string Name,
        Func<ITranslationProvider> Factory,
        TimeSpan Timeout);

    private readonly Candidate[] _candidates =
    [
        new("Microsoft/Bing", static () => new BingFreeTranslationProvider(), TimeSpan.FromSeconds(55)),
        new("Google", static () => new GoogleFreeTranslationProvider(), TimeSpan.FromSeconds(30)),
        new("腾讯 TranSmart", static () => new TencentTranSmartFreeTranslationProvider(), TimeSpan.FromSeconds(18)),
    ];

    private int _preferredIndex = -1;

    public string Id => "free-auto-v3";
    public string DisplayName => "免费翻译（自动）";

    public async Task<IReadOnlyList<string>> TranslateBatchAsync(
        IReadOnlyList<string> texts,
        CancellationToken cancellationToken = default)
    {
        var failures = new List<string>();

        if (_preferredIndex >= 0 && _preferredIndex < _candidates.Length)
        {
            var preferred = _candidates[_preferredIndex];
            var preferredResult = await TryCandidateAsync(preferred, texts, cancellationToken);
            if (preferredResult.Success)
                return preferredResult.Translations!;

            failures.Add($"{preferred.Name}: {preferredResult.Error}");
            _preferredIndex = -1;
        }

        for (var index = 0; index < _candidates.Length; index++)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var candidate = _candidates[index];

            var result = await TryCandidateAsync(candidate, texts, cancellationToken);
            if (result.Success)
            {
                _preferredIndex = index;
                return result.Translations!;
            }

            failures.Add($"{candidate.Name}: {result.Error}");
        }

        throw new InvalidOperationException(
            "所有免费翻译服务当前都不可用。\n" + string.Join("\n", failures));
    }

    private static async Task<(bool Success, IReadOnlyList<string>? Translations, string Error)> TryCandidateAsync(
        Candidate candidate,
        IReadOnlyList<string> texts,
        CancellationToken cancellationToken)
    {
        ITranslationProvider? provider = null;
        using var providerCts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        providerCts.CancelAfter(candidate.Timeout);

        try
        {
            provider = candidate.Factory();
            var translationTask = provider.TranslateBatchAsync(texts, providerCts.Token);
            var translated = await translationTask.WaitAsync(
                candidate.Timeout + TimeSpan.FromSeconds(2),
                cancellationToken);
            return (true, translated, string.Empty);
        }
        catch (OperationCanceledException) when (cancellationToken.IsCancellationRequested)
        {
            throw;
        }
        catch (OperationCanceledException)
        {
            return (false, null, $"超过 {candidate.Timeout.TotalSeconds:0} 秒未响应，已自动切换下一个免费引擎");
        }
        catch (TimeoutException)
        {
            providerCts.Cancel();
            return (false, null, $"超过 {candidate.Timeout.TotalSeconds:0} 秒未响应，已自动切换下一个免费引擎");
        }
        catch (Exception ex)
        {
            return (false, null, ex.Message);
        }
        finally
        {
            providerCts.Cancel();
            if (provider is IDisposable disposable) disposable.Dispose();
        }
    }
}
