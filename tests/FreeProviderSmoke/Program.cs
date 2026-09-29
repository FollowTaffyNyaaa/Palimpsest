using Palimpsest_App.Translation;

var provider = new BingFreeTranslationProvider();
var source = new[]
{
    "We evaluate the proposed method on three public datasets and report mean Average Precision.",
    "For CLIP-based features, the objective L = L_sem + 0.5 L_hash preserves cross-modal similarity."
};

var result = await provider.TranslateBatchAsync(source);
if (result.Count != source.Length || result.Any(string.IsNullOrWhiteSpace))
    throw new Exception("Bing provider returned an invalid batch.");

Console.WriteLine("BING_FREE_V2_OK count=" + result.Count);
for (var i = 0; i < result.Count; i++)
    Console.WriteLine($"[{i}] {result[i]}");

var auto = new AutoFreeTranslationProvider();
var autoResult = await auto.TranslateBatchAsync(new[] { "This sentence verifies automatic free translation fallback." });
if (autoResult.Count != 1 || string.IsNullOrWhiteSpace(autoResult[0]))
    throw new Exception("Auto free provider returned an invalid result.");

Console.WriteLine("AUTO_FREE_V3_OK " + autoResult[0]);
