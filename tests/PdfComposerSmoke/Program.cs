using Palimpsest_App.Translation;

if (args.Length != 1)
{
    Console.Error.WriteLine("usage: PdfComposerSmoke <paper_ir.json>");
    return 2;
}

var result = await ReflowChinesePdfComposer.ComposeAsync(args[0]);
Console.WriteLine($"PDF_REFLOW_V2_OK path={result.OutputPath} pages={result.PageCount} paragraphs={result.ParagraphCount} overflow={result.OverflowParagraphs} qa={result.QaPath}");
return 0;
