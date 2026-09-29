using System;
using System.Collections.Generic;
using System.Linq;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;

namespace Palimpsest_App.Translation;

public enum LlmCompatibilityProtocol
{
    OpenAI,
    Anthropic,
}

public sealed record LlmApiProfile(
    LlmCompatibilityProtocol Protocol,
    string BaseUrl,
    string Model);

public sealed class CompatibleLlmTranslationProvider : ITranslationProvider
{
    private static readonly HttpClient Http = new()
    {
        Timeout = TimeSpan.FromMinutes(3),
    };

    private const string TranslationInstruction =
        "You are a professional academic translator. Translate English academic prose into natural, precise Simplified Chinese. " +
        "Preserve citation markers, URLs, DOI strings, dataset/model names, acronyms, inline mathematical expressions, symbols, equation references, " +
        "and placeholder tokens exactly. Do not add explanations, summaries, headings, or commentary.";

    private readonly LlmApiProfile _profile;
    private readonly string _apiKey;
    private readonly Uri _endpoint;

    public CompatibleLlmTranslationProvider(LlmApiProfile profile, string? apiKey)
    {
        if (string.IsNullOrWhiteSpace(profile.BaseUrl))
            throw new ArgumentException("大模型 API URL 不能为空。", nameof(profile));
        if (string.IsNullOrWhiteSpace(profile.Model))
            throw new ArgumentException("模型名称不能为空。", nameof(profile));

        _profile = profile with
        {
            BaseUrl = profile.BaseUrl.Trim(),
            Model = profile.Model.Trim(),
        };
        _apiKey = apiKey?.Trim() ?? string.Empty;
        _endpoint = BuildEndpoint(_profile.Protocol, _profile.BaseUrl);
    }

    public string Id
    {
        get
        {
            var source = $"{_profile.Protocol}|{_profile.BaseUrl}|{_profile.Model}";
            var hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(source))).ToLowerInvariant();
            return $"llm-{_profile.Protocol.ToString().ToLowerInvariant()}-{hash[..16]}";
        }
    }

    public string DisplayName =>
        $"{(_profile.Protocol == LlmCompatibilityProtocol.OpenAI ? "OpenAI 兼容" : "Anthropic 兼容")} · {_profile.Model}";

    public async Task<IReadOnlyList<string>> TranslateBatchAsync(
        IReadOnlyList<string> texts,
        CancellationToken cancellationToken = default)
    {
        if (texts.Count == 0) return Array.Empty<string>();

        try
        {
            var content = await SendBatchRequestAsync(texts, cancellationToken);
            return ParseBatchTranslations(content, texts.Count);
        }
        catch (LlmResponseFormatException) when (texts.Count > 1)
        {
            var fallback = new List<string>(texts.Count);
            foreach (var text in texts)
            {
                cancellationToken.ThrowIfCancellationRequested();
                fallback.Add(await TranslateOneAsync(text, cancellationToken));
            }
            return fallback;
        }
    }

    private async Task<string> TranslateOneAsync(string text, CancellationToken cancellationToken)
    {
        var userPrompt =
            "Translate the following English academic passage into Simplified Chinese. " +
            "Return only the translated text, with no quotation marks or commentary.\n\n" + text;

        var content = await SendRequestAsync(userPrompt, batchMode: false, cancellationToken);
        var trimmed = StripCodeFence(content).Trim();
        if (string.IsNullOrWhiteSpace(trimmed))
            throw new InvalidOperationException("大模型返回了空译文。");
        return trimmed;
    }

    private Task<string> SendBatchRequestAsync(IReadOnlyList<string> texts, CancellationToken cancellationToken)
    {
        var items = texts.Select((text, index) => new { id = index, text }).ToArray();
        var sourceJson = JsonSerializer.Serialize(items);
        var userPrompt =
            "Translate every item in the JSON array below into Simplified Chinese. Keep the same order and item count. " +
            "Return ONLY valid JSON in exactly this shape: {\"translations\":[\"translation 0\",\"translation 1\"]}. " +
            "Do not wrap the JSON in Markdown.\n\nSOURCE_JSON:\n" + sourceJson;
        return SendRequestAsync(userPrompt, batchMode: true, cancellationToken);
    }

    private async Task<string> SendRequestAsync(
        string userPrompt,
        bool batchMode,
        CancellationToken cancellationToken)
    {
        using var request = _profile.Protocol == LlmCompatibilityProtocol.OpenAI
            ? BuildOpenAiRequest(userPrompt)
            : BuildAnthropicRequest(userPrompt, batchMode);

        using var response = await Http.SendAsync(request, cancellationToken);
        var raw = await response.Content.ReadAsStringAsync(cancellationToken);
        if (!response.IsSuccessStatusCode)
        {
            throw new InvalidOperationException(
                $"{DisplayName} 请求失败 ({(int)response.StatusCode})：{Compact(raw)}");
        }

        try
        {
            using var json = JsonDocument.Parse(raw);
            return _profile.Protocol == LlmCompatibilityProtocol.OpenAI
                ? ExtractOpenAiContent(json.RootElement)
                : ExtractAnthropicContent(json.RootElement);
        }
        catch (JsonException ex)
        {
            throw new InvalidOperationException($"{DisplayName} 返回了无法解析的 JSON：{Compact(raw)}", ex);
        }
    }

    private HttpRequestMessage BuildOpenAiRequest(string userPrompt)
    {
        var request = new HttpRequestMessage(HttpMethod.Post, _endpoint);
        if (!string.IsNullOrWhiteSpace(_apiKey))
            request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", _apiKey);
        request.Headers.UserAgent.ParseAdd("Palimpsest/0.3");
        request.Content = JsonContent.Create(new
        {
            model = _profile.Model,
            messages = new object[]
            {
                new { role = "system", content = TranslationInstruction },
                new { role = "user", content = userPrompt },
            },
            stream = false,
        });
        return request;
    }

    private HttpRequestMessage BuildAnthropicRequest(string userPrompt, bool batchMode)
    {
        var request = new HttpRequestMessage(HttpMethod.Post, _endpoint);
        if (!string.IsNullOrWhiteSpace(_apiKey))
            request.Headers.TryAddWithoutValidation("x-api-key", _apiKey);
        request.Headers.TryAddWithoutValidation("anthropic-version", "2023-06-01");
        request.Headers.UserAgent.ParseAdd("Palimpsest/0.3");
        request.Content = JsonContent.Create(new
        {
            model = _profile.Model,
            max_tokens = batchMode ? 8192 : 4096,
            system = TranslationInstruction,
            messages = new[]
            {
                new { role = "user", content = userPrompt },
            },
        });
        return request;
    }

    private static string ExtractOpenAiContent(JsonElement root)
    {
        if (!root.TryGetProperty("choices", out var choices) ||
            choices.ValueKind != JsonValueKind.Array ||
            choices.GetArrayLength() == 0)
            throw new InvalidOperationException("OpenAI 兼容接口响应中没有 choices。");

        var message = choices[0].GetProperty("message");
        if (!message.TryGetProperty("content", out var content))
            throw new InvalidOperationException("OpenAI 兼容接口响应中没有 message.content。");

        if (content.ValueKind == JsonValueKind.String)
            return content.GetString() ?? string.Empty;

        if (content.ValueKind == JsonValueKind.Array)
        {
            var builder = new StringBuilder();
            foreach (var part in content.EnumerateArray())
            {
                if (part.ValueKind == JsonValueKind.Object &&
                    part.TryGetProperty("text", out var text) &&
                    text.ValueKind == JsonValueKind.String)
                    builder.Append(text.GetString());
            }
            return builder.ToString();
        }

        throw new InvalidOperationException("OpenAI 兼容接口返回了不支持的 content 格式。");
    }

    private static string ExtractAnthropicContent(JsonElement root)
    {
        if (!root.TryGetProperty("content", out var content) || content.ValueKind != JsonValueKind.Array)
            throw new InvalidOperationException("Anthropic 兼容接口响应中没有 content 数组。");

        var builder = new StringBuilder();
        foreach (var block in content.EnumerateArray())
        {
            if (block.ValueKind != JsonValueKind.Object) continue;
            if (block.TryGetProperty("type", out var type) &&
                type.ValueKind == JsonValueKind.String &&
                type.GetString() != "text")
                continue;
            if (block.TryGetProperty("text", out var text) && text.ValueKind == JsonValueKind.String)
                builder.Append(text.GetString());
        }

        if (builder.Length == 0)
            throw new InvalidOperationException("Anthropic 兼容接口没有返回文本内容。");
        return builder.ToString();
    }

    private static IReadOnlyList<string> ParseBatchTranslations(string content, int expectedCount)
    {
        var cleaned = StripCodeFence(content).Trim();
        try
        {
            using var json = JsonDocument.Parse(cleaned);
            JsonElement translations;
            if (json.RootElement.ValueKind == JsonValueKind.Object &&
                json.RootElement.TryGetProperty("translations", out var property))
            {
                translations = property;
            }
            else if (json.RootElement.ValueKind == JsonValueKind.Array)
            {
                translations = json.RootElement;
            }
            else
            {
                throw new LlmResponseFormatException("大模型没有返回 translations 数组。");
            }

            if (translations.ValueKind != JsonValueKind.Array)
                throw new LlmResponseFormatException("translations 不是数组。");

            var result = translations.EnumerateArray()
                .Select(item => item.ValueKind == JsonValueKind.String ? item.GetString() ?? string.Empty : string.Empty)
                .ToArray();

            if (result.Length != expectedCount || result.Any(string.IsNullOrWhiteSpace))
                throw new LlmResponseFormatException(
                    $"批量译文数量不匹配：请求 {expectedCount} 段，返回 {result.Length} 段。");
            return result;
        }
        catch (JsonException ex)
        {
            throw new LlmResponseFormatException("大模型没有返回有效的批量翻译 JSON。", ex);
        }
    }

    private static Uri BuildEndpoint(LlmCompatibilityProtocol protocol, string baseUrl)
    {
        var value = baseUrl.Trim().TrimEnd('/');
        var suffix = protocol == LlmCompatibilityProtocol.OpenAI ? "/chat/completions" : "/messages";

        if (value.EndsWith(suffix, StringComparison.OrdinalIgnoreCase))
            return new Uri(value, UriKind.Absolute);

        if (!Uri.TryCreate(value, UriKind.Absolute, out var baseUri) ||
            (baseUri.Scheme != Uri.UriSchemeHttp && baseUri.Scheme != Uri.UriSchemeHttps))
            throw new ArgumentException("API URL 必须是有效的 http/https 地址。", nameof(baseUrl));

        return new Uri(value + suffix, UriKind.Absolute);
    }

    private static string StripCodeFence(string text)
    {
        var fence = new string((char)96, 3);
        var trimmed = text.Trim();
        if (!trimmed.StartsWith(fence, StringComparison.Ordinal)) return trimmed;

        var firstLineEnd = trimmed.IndexOf('\n');
        if (firstLineEnd >= 0) trimmed = trimmed[(firstLineEnd + 1)..];
        var lastFence = trimmed.LastIndexOf(fence, StringComparison.Ordinal);
        if (lastFence >= 0) trimmed = trimmed[..lastFence];
        return trimmed.Trim();
    }

    private static string Compact(string value)
    {
        value = value.Replace("\r", " ").Replace("\n", " ").Trim();
        return value.Length <= 900 ? value : value[..900] + "…";
    }

    private sealed class LlmResponseFormatException : Exception
    {
        public LlmResponseFormatException(string message) : base(message) { }
        public LlmResponseFormatException(string message, Exception inner) : base(message, inner) { }
    }
}
