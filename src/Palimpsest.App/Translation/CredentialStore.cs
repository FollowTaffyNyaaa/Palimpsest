using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading.Tasks;

namespace Palimpsest_App.Translation;

public sealed class CredentialStore
{
    private static readonly byte[] Entropy = Encoding.UTF8.GetBytes("Palimpsest.Translation.Credentials.v1");
    private readonly string _path;

    public CredentialStore()
    {
        var directory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "Palimpsest");
        Directory.CreateDirectory(directory);
        _path = Path.Combine(directory, "credentials.json");
    }

    public async Task SaveAsync(string providerId, string apiKey)
    {
        var values = await ReadAllAsync();
        var protectedBytes = ProtectedData.Protect(
            Encoding.UTF8.GetBytes(apiKey.Trim()),
            Entropy,
            DataProtectionScope.CurrentUser);
        values[providerId] = Convert.ToBase64String(protectedBytes);
        await WriteAllAsync(values);
    }

    public async Task<string?> LoadAsync(string providerId)
    {
        var values = await ReadAllAsync();
        if (!values.TryGetValue(providerId, out var encoded) || string.IsNullOrWhiteSpace(encoded))
            return null;

        try
        {
            var protectedBytes = Convert.FromBase64String(encoded);
            var plain = ProtectedData.Unprotect(protectedBytes, Entropy, DataProtectionScope.CurrentUser);
            return Encoding.UTF8.GetString(plain);
        }
        catch (CryptographicException)
        {
            return null;
        }
    }

    public async Task DeleteAsync(string providerId)
    {
        var values = await ReadAllAsync();
        if (!values.Remove(providerId)) return;
        await WriteAllAsync(values);
    }

    private async Task<Dictionary<string, string>> ReadAllAsync()
    {
        if (!File.Exists(_path)) return new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        try
        {
            await using var stream = File.OpenRead(_path);
            return await JsonSerializer.DeserializeAsync<Dictionary<string, string>>(stream)
                ?? new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        }
        catch (JsonException)
        {
            return new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        }
    }

    private async Task WriteAllAsync(Dictionary<string, string> values)
    {
        var temp = _path + ".tmp";
        await using (var stream = File.Create(temp))
        {
            await JsonSerializer.SerializeAsync(stream, values, new JsonSerializerOptions { WriteIndented = true });
        }
        File.Move(temp, _path, true);
    }
}

