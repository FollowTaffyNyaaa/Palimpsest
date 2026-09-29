using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Text.Json;
using Palimpsest_App.Translation;

await TestOpenAiAsync();
await TestAnthropicAsync();
Console.WriteLine("LLM_PROVIDER_SMOKE_OK openai=ok anthropic=ok batch=ok headers=ok");

static async Task TestOpenAiAsync()
{
    await using var server = new MockHttpServer(request =>
    {
        if (request.Path != "/v1/chat/completions") throw new Exception("OpenAI endpoint path mismatch: " + request.Path);
        if (!string.Equals(request.Headers.GetValueOrDefault("authorization"), "Bearer openai-secret", StringComparison.Ordinal))
            throw new Exception("OpenAI Authorization header mismatch");
        using var json = JsonDocument.Parse(request.Body);
        if (json.RootElement.GetProperty("model").GetString() != "test-openai")
            throw new Exception("OpenAI model mismatch");
        var messages = json.RootElement.GetProperty("messages");
        if (messages.GetArrayLength() != 2) throw new Exception("OpenAI messages mismatch");

        return """
        {"choices":[{"message":{"role":"assistant","content":"{\"translations\":[\"你好世界。\",\"第二段。\"]}"}}]}
        """;
    });

    var provider = new CompatibleLlmTranslationProvider(
        new LlmApiProfile(LlmCompatibilityProtocol.OpenAI, server.BaseUrl + "/v1", "test-openai"),
        "openai-secret");

    var result = await provider.TranslateBatchAsync(["Hello world.", "Second paragraph."]);
    if (result.Count != 2 || result[0] != "你好世界。" || result[1] != "第二段。")
        throw new Exception("OpenAI translation parsing failed");
}

static async Task TestAnthropicAsync()
{
    await using var server = new MockHttpServer(request =>
    {
        if (request.Path != "/v1/messages") throw new Exception("Anthropic endpoint path mismatch: " + request.Path);
        if (!string.Equals(request.Headers.GetValueOrDefault("x-api-key"), "anthropic-secret", StringComparison.Ordinal))
            throw new Exception("Anthropic x-api-key mismatch");
        if (!string.Equals(request.Headers.GetValueOrDefault("anthropic-version"), "2023-06-01", StringComparison.Ordinal))
            throw new Exception("Anthropic version header mismatch");
        using var json = JsonDocument.Parse(request.Body);
        if (json.RootElement.GetProperty("model").GetString() != "test-anthropic")
            throw new Exception("Anthropic model mismatch");
        if (string.IsNullOrWhiteSpace(json.RootElement.GetProperty("system").GetString()))
            throw new Exception("Anthropic system prompt missing");

        return """
        {"content":[{"type":"text","text":"{\"translations\":[\"附录标题\",\"附录正文\"]}"}]}
        """;
    });

    var provider = new CompatibleLlmTranslationProvider(
        new LlmApiProfile(LlmCompatibilityProtocol.Anthropic, server.BaseUrl + "/v1", "test-anthropic"),
        "anthropic-secret");

    var result = await provider.TranslateBatchAsync(["Appendix heading", "Appendix body"]);
    if (result.Count != 2 || result[0] != "附录标题" || result[1] != "附录正文")
        throw new Exception("Anthropic translation parsing failed");
}

sealed record HttpRequestData(string Path, Dictionary<string, string> Headers, string Body);

sealed class MockHttpServer : IAsyncDisposable
{
    private readonly TcpListener _listener;
    private readonly Func<HttpRequestData, string> _handler;
    private readonly Task _serverTask;

    public MockHttpServer(Func<HttpRequestData, string> handler)
    {
        _handler = handler;
        _listener = new TcpListener(IPAddress.Loopback, 0);
        _listener.Start();
        var port = ((IPEndPoint)_listener.LocalEndpoint).Port;
        BaseUrl = $"http://127.0.0.1:{port}";
        _serverTask = ServeOneAsync();
    }

    public string BaseUrl { get; }

    private async Task ServeOneAsync()
    {
        using var client = await _listener.AcceptTcpClientAsync();
        await using var stream = client.GetStream();
        using var reader = new StreamReader(stream, Encoding.UTF8, false, 8192, leaveOpen: true);

        var requestLine = await reader.ReadLineAsync() ?? throw new Exception("empty request");
        var parts = requestLine.Split(' ');
        var path = parts.Length >= 2 ? parts[1] : "";
        var headers = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        string? line;
        while (!string.IsNullOrEmpty(line = await reader.ReadLineAsync()))
        {
            var colon = line.IndexOf(':');
            if (colon > 0) headers[line[..colon].Trim()] = line[(colon + 1)..].Trim();
        }

        string body;
        if (headers.TryGetValue("Content-Length", out var rawLength) && int.TryParse(rawLength, out var length) && length > 0)
        {
            var buffer = new char[length];
            var read = 0;
            while (read < length)
            {
                var count = await reader.ReadAsync(buffer.AsMemory(read, length - read));
                if (count == 0) break;
                read += count;
            }
            body = new string(buffer, 0, read);
        }
        else if (headers.TryGetValue("Transfer-Encoding", out var transferEncoding) &&
                 transferEncoding.Contains("chunked", StringComparison.OrdinalIgnoreCase))
        {
            var builder = new StringBuilder();
            while (true)
            {
                var sizeLine = await reader.ReadLineAsync() ?? throw new Exception("missing chunk size");
                var semicolon = sizeLine.IndexOf(';');
                if (semicolon >= 0) sizeLine = sizeLine[..semicolon];
                var chunkSize = Convert.ToInt32(sizeLine.Trim(), 16);
                if (chunkSize == 0)
                {
                    while (!string.IsNullOrEmpty(await reader.ReadLineAsync())) { }
                    break;
                }

                var chunk = new char[chunkSize];
                var chunkRead = 0;
                while (chunkRead < chunkSize)
                {
                    var count = await reader.ReadAsync(chunk.AsMemory(chunkRead, chunkSize - chunkRead));
                    if (count == 0) break;
                    chunkRead += count;
                }
                builder.Append(chunk, 0, chunkRead);
                await reader.ReadLineAsync();
            }
            body = builder.ToString();
        }
        else
        {
            body = string.Empty;
        }

        var responseBody = _handler(new HttpRequestData(path, headers, body));
        var bytes = Encoding.UTF8.GetBytes(responseBody);
        var headerBytes = Encoding.ASCII.GetBytes(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " + bytes.Length + "\r\nConnection: close\r\n\r\n");
        await stream.WriteAsync(headerBytes);
        await stream.WriteAsync(bytes);
        await stream.FlushAsync();
    }

    public async ValueTask DisposeAsync()
    {
        _listener.Stop();
        await _serverTask;
    }
}
