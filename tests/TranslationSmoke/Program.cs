using System.Text.Json.Nodes;
using Palimpsest_App.Translation;

var root = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "..", "test_output", "translation_smoke"));
if (Directory.Exists(root)) Directory.Delete(root, true);
Directory.CreateDirectory(root);
var ir = Path.Combine(root, "paper_ir.json");

await File.WriteAllTextAsync(ir, """
{
  "paragraphs": [
    { "paragraph_id": "P1", "semantic_type": "BODY", "source_text": "Hello world.", "normalized_text": "Hello world.", "translated_text": "", "translation_state": "PENDING" },
    { "paragraph_id": "P2", "semantic_type": "FIGURE_CAPTION", "source_text": "Figure 1. Keep me.", "normalized_text": "Figure 1. Keep me.", "translated_text": "", "translation_state": "PENDING" },
    { "paragraph_id": "P3", "semantic_type": "BODY", "source_text": "Second paragraph.", "normalized_text": "Second paragraph.", "translated_text": "", "translation_state": "PENDING" },
    { "paragraph_id": "P4", "semantic_type": "APPENDIX_HEADING", "source_text": "Appendix A. Proofs", "normalized_text": "Appendix A. Proofs", "translated_text": "", "translation_state": "PENDING" },
    { "paragraph_id": "P5", "semantic_type": "APPENDIX_BODY", "source_text": "Appendix paragraph.", "normalized_text": "Appendix paragraph.", "translated_text": "", "translation_state": "PENDING" }
  ]
}
""");

var firstProvider = new FakeProvider();
var first = await new PaperTranslationService(firstProvider).TranslateAsync(ir);
if (first.Total != 4 || first.CacheHits != 0 || firstProvider.TranslatedItemCount != 4)
    throw new Exception("first translation pass failed");

var doc = JsonNode.Parse(await File.ReadAllTextAsync(ir))!.AsObject();
var paragraphs = doc["paragraphs"]!.AsArray();
if (paragraphs[0]!["translated_text"]!.GetValue<string>() != "译:Hello world.")
    throw new Exception("BODY P1 was not translated");
if (!string.IsNullOrEmpty(paragraphs[1]!["translated_text"]!.GetValue<string>()))
    throw new Exception("non-BODY paragraph was translated");
if (paragraphs[2]!["translation_state"]!.GetValue<string>() != "COMPLETED")
    throw new Exception("BODY P3 state was not persisted");
if (paragraphs[3]!["translated_text"]!.GetValue<string>() != "译:Appendix A. Proofs" ||
    paragraphs[4]!["translated_text"]!.GetValue<string>() != "译:Appendix paragraph.")
    throw new Exception("appendix content was not translated");

var secondProvider = new FakeProvider();
var second = await new PaperTranslationService(secondProvider).TranslateAsync(ir);
if (second.CacheHits != 4 || secondProvider.TranslatedItemCount != 0)
    throw new Exception("translation cache did not prevent repeat provider calls");

var markdown = await File.ReadAllTextAsync(Path.Combine(root, "translated_body.md"));
if (!markdown.Contains("译:Hello world.") || !markdown.Contains("译:Second paragraph.") ||
    !markdown.Contains("译:Appendix A. Proofs") || !markdown.Contains("译:Appendix paragraph."))
    throw new Exception("translated_body.md was not generated correctly");

var credentialStore = new CredentialStore();
const string dummyCredential = "palimpsest-smoke-secret-not-a-real-key";
await credentialStore.SaveAsync("smoke-test", dummyCredential);
var loadedCredential = await credentialStore.LoadAsync("smoke-test");
if (loadedCredential != dummyCredential)
    throw new Exception("DPAPI credential roundtrip failed");
await credentialStore.DeleteAsync("smoke-test");

Console.WriteLine("TRANSLATION_SMOKE_OK body=2 appendix=2 cache_hits=4 locked_translated=0 dpapi=ok");

sealed class FakeProvider : ITranslationProvider
{
    public string Id => "fake";
    public string DisplayName => "Fake";
    public int TranslatedItemCount { get; private set; }

    public Task<IReadOnlyList<string>> TranslateBatchAsync(IReadOnlyList<string> texts, CancellationToken cancellationToken = default)
    {
        TranslatedItemCount += texts.Count;
        return Task.FromResult<IReadOnlyList<string>>(texts.Select(text => "译:" + text).ToArray());
    }
}

