using Palimpsest_App.Translation;

if (args.Length != 1)
{
    Console.Error.WriteLine("usage: ReflowEndToEndSmoke <paper_ir.json>");
    return 2;
}

var paperIr = Path.GetFullPath(args[0]);
var provider = new AutoFreeTranslationProvider();
var service = new PaperTranslationService(provider);

var progress = new Progress<TranslationProgressInfo>(p =>
{
    Console.WriteLine($"TRANSLATE {p.Completed}/{p.Total} cached={p.Cached} {p.Message}");
});

var translation = await service.TranslateAsync(paperIr, progress);
Console.WriteLine($"TRANSLATION_DONE total={translation.Total} translated={translation.Translated} cached={translation.CacheHits}");

var pdf = await ReflowChinesePdfComposer.ComposeAsync(paperIr);
Console.WriteLine($"REFLOW_DONE path={pdf.OutputPath} pages={pdf.PageCount} paragraphs={pdf.ParagraphCount} overflow={pdf.OverflowParagraphs} qa={pdf.QaPath}");
return 0;
