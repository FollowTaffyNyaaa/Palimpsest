using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Palimpsest_App.Translation;
using Windows.ApplicationModel.DataTransfer;
using Windows.Storage;
using Windows.Storage.Pickers;

namespace Palimpsest_App;

public sealed partial class MainPage : Page
{
    private string? _pdfPath;
    private string? _outputPath;
    private string? _translatedPdfPath;
    private readonly CredentialStore _credentialStore = new();
    private CancellationTokenSource? _translationCts;

    public MainPage()
    {
        InitializeComponent();
        Loaded += MainPage_Loaded;
    }

    private void MainPage_Loaded(object sender, RoutedEventArgs e)
    {
        UpdateProviderUi();
    }

    private async void OpenPdfButton_Click(object sender, RoutedEventArgs e)
    {
        var picker = new FileOpenPicker();
        picker.FileTypeFilter.Add(".pdf");
        picker.SuggestedStartLocation = PickerLocationId.DocumentsLibrary;

        if (App.MainWindow is not null)
        {
            var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(App.MainWindow);
            WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
        }

        var file = await picker.PickSingleFileAsync();
        if (file is not null) SetPdf(file.Path);
    }

    private void DropZone_DragOver(object sender, DragEventArgs e)
    {
        if (!e.DataView.Contains(StandardDataFormats.StorageItems)) return;
        e.AcceptedOperation = DataPackageOperation.Copy;
        e.DragUIOverride.Caption = "用 Palimpsest 分析这篇论文";
        e.DragUIOverride.IsCaptionVisible = true;
    }

    private async void DropZone_Drop(object sender, DragEventArgs e)
    {
        if (!e.DataView.Contains(StandardDataFormats.StorageItems)) return;
        var items = await e.DataView.GetStorageItemsAsync();
        var file = items.OfType<StorageFile>().FirstOrDefault(f => string.Equals(f.FileType, ".pdf", StringComparison.OrdinalIgnoreCase));
        if (file is not null) SetPdf(file.Path);
        else StatusText.Text = "拖入的项目里没有 PDF 文件。";
    }

    private void SetPdf(string path)
    {
        _pdfPath = path;
        var directory = Path.GetDirectoryName(path) ?? Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments);
        var stem = Path.GetFileNameWithoutExtension(path);
        _outputPath = Path.Combine(directory, stem + "_palimpsest_debug");
        _translatedPdfPath = Path.Combine(directory, stem + "_zh-CN.pdf");
        PdfPathText.Text = path;
        StatusText.Text = "已选择论文。点击“分析论文结构”生成 PaperIR 和调试页面。";
        AnalyzeButton.IsEnabled = true;
        OpenOutputButton.IsEnabled = Directory.Exists(_outputPath);
        TranslateButton.IsEnabled = File.Exists(Path.Combine(_outputPath, "paper_ir.json"));
        OpenTranslationButton.IsEnabled = _translatedPdfPath is not null && File.Exists(_translatedPdfPath);
        TranslationProgress.Visibility = Visibility.Collapsed;
        TranslationStatusText.Text = TranslateButton.IsEnabled ? "检测到已有 PaperIR，可直接翻译。" : "请先分析论文。";
    }

    private async void AnalyzeButton_Click(object sender, RoutedEventArgs e)
    {
        if (_pdfPath is null || _outputPath is null) return;
        AnalyzeButton.IsEnabled = false;
        InspectorProgress.Visibility = Visibility.Visible;
        StatusText.Text = "正在解析 PDF、恢复阅读顺序并生成 Clean Paper Template…";

        try
        {
            var result = await RunInspectorAsync(_pdfPath, _outputPath);
            StatusText.Text = result.ExitCode == 0
                ? (string.IsNullOrWhiteSpace(result.StdOut) ? "分析完成。" : result.StdOut.Trim())
                : "分析失败：" + (string.IsNullOrWhiteSpace(result.StdErr) ? result.StdOut : result.StdErr).Trim();
            OpenOutputButton.IsEnabled = result.ExitCode == 0 && Directory.Exists(_outputPath);
            TranslateButton.IsEnabled = result.ExitCode == 0 && File.Exists(Path.Combine(_outputPath, "paper_ir.json"));
            OpenTranslationButton.IsEnabled = _translatedPdfPath is not null && File.Exists(_translatedPdfPath);
            if (result.ExitCode == 0) TranslationStatusText.Text = "正文与附录已分区，可以开始翻译。";
        }
        catch (Exception ex)
        {
            StatusText.Text = "分析失败：" + ex.Message;
        }
        finally
        {
            InspectorProgress.Visibility = Visibility.Collapsed;
            AnalyzeButton.IsEnabled = true;
        }
    }

    private static async Task<(int ExitCode, string StdOut, string StdErr)> RunInspectorAsync(string pdfPath, string outputPath)
    {
        var inspector = Path.Combine(AppContext.BaseDirectory, "Native", "paper_inspector.exe");
        if (!File.Exists(inspector))
            throw new FileNotFoundException("Paper Inspector 未随 App 构建。请先运行 windows\\scripts\\build.ps1。", inspector);

        Directory.CreateDirectory(outputPath);
        var start = new ProcessStartInfo
        {
            FileName = inspector,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
        };
        start.ArgumentList.Add(pdfPath);
        start.ArgumentList.Add(outputPath);

        using var process = Process.Start(start) ?? throw new InvalidOperationException("无法启动 Paper Inspector。");
        var stdoutTask = process.StandardOutput.ReadToEndAsync();
        var stderrTask = process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        return (process.ExitCode, await stdoutTask, await stderrTask);
    }

    private void OpenOutputButton_Click(object sender, RoutedEventArgs e)
    {
        if (_outputPath is null || !Directory.Exists(_outputPath)) return;
        var start = new ProcessStartInfo("explorer.exe") { UseShellExecute = true };
        start.ArgumentList.Add(_outputPath);
        Process.Start(start);
    }

    private string SelectedProviderId =>
        (ProviderCombo.SelectedItem as ComboBoxItem)?.Tag?.ToString() ?? "free-auto";

    private bool IsLlmProvider =>
        SelectedProviderId is "llm-openai" or "llm-anthropic";

    private LlmCompatibilityProtocol SelectedLlmProtocol =>
        SelectedProviderId == "llm-anthropic"
            ? LlmCompatibilityProtocol.Anthropic
            : LlmCompatibilityProtocol.OpenAI;

    private string SelectedLlmProfileStoreKey =>
        SelectedLlmProtocol == LlmCompatibilityProtocol.Anthropic
            ? "llm.anthropic.profile"
            : "llm.openai.profile";

    private string SelectedLlmApiKeyStoreKey =>
        SelectedLlmProtocol == LlmCompatibilityProtocol.Anthropic
            ? "llm.anthropic.key"
            : "llm.openai.key";

    private async void ProviderCombo_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        UpdateProviderUi();
        if (IsLlmProvider)
            await LoadLlmProfileIntoUiAsync();
    }

    private void UpdateProviderUi()
    {
        if (LlmApiPanel is null || ProviderCombo is null) return;
        LlmApiPanel.Visibility = IsLlmProvider ? Visibility.Visible : Visibility.Collapsed;
        if (!IsLlmProvider) return;

        LlmProtocolText.Text = SelectedLlmProtocol == LlmCompatibilityProtocol.OpenAI
            ? "OpenAI 兼容 API"
            : "Anthropic 兼容 API";
        LlmBaseUrlBox.PlaceholderText = SelectedLlmProtocol == LlmCompatibilityProtocol.OpenAI
            ? "例如 https://api.openai.com/v1"
            : "例如 https://api.anthropic.com/v1";
    }

    private async Task LoadLlmProfileIntoUiAsync()
    {
        if (!IsLlmProvider || LlmBaseUrlBox is null || LlmModelBox is null || LlmApiKeyBox is null) return;

        var raw = await _credentialStore.LoadAsync(SelectedLlmProfileStoreKey);
        LlmApiProfile? profile = null;
        if (!string.IsNullOrWhiteSpace(raw))
        {
            try
            {
                profile = JsonSerializer.Deserialize<LlmApiProfile>(raw);
            }
            catch (JsonException)
            {
                profile = null;
            }
        }

        profile ??= new LlmApiProfile(
            SelectedLlmProtocol,
            SelectedLlmProtocol == LlmCompatibilityProtocol.OpenAI
                ? "https://api.openai.com/v1"
                : "https://api.anthropic.com/v1",
            string.Empty);

        LlmBaseUrlBox.Text = profile.BaseUrl;
        LlmModelBox.Text = profile.Model;
        LlmApiKeyBox.Password = string.Empty;

        var savedKey = await _credentialStore.LoadAsync(SelectedLlmApiKeyStoreKey);
        LlmCredentialStateText.Text = string.IsNullOrWhiteSpace(savedKey)
            ? "配置未保存 Key"
            : "配置与 Key 已安全保存";
    }

    private async void SaveLlmProfileButton_Click(object sender, RoutedEventArgs e)
    {
        if (!IsLlmProvider) return;
        try
        {
            var profile = ReadLlmProfileFromUi();
            await _credentialStore.SaveAsync(SelectedLlmProfileStoreKey, JsonSerializer.Serialize(profile));

            var key = LlmApiKeyBox.Password.Trim();
            if (!string.IsNullOrWhiteSpace(key))
            {
                await _credentialStore.SaveAsync(SelectedLlmApiKeyStoreKey, key);
                LlmApiKeyBox.Password = string.Empty;
            }

            var savedKey = await _credentialStore.LoadAsync(SelectedLlmApiKeyStoreKey);
            LlmCredentialStateText.Text = string.IsNullOrWhiteSpace(savedKey)
                ? "配置已保存 · 无 Key"
                : "配置与 Key 已安全保存";
            TranslationStatusText.Text = "大模型接口配置已保存到本机。";
        }
        catch (Exception ex)
        {
            TranslationStatusText.Text = "保存大模型接口失败：" + ex.Message;
        }
    }

    private async void ClearLlmApiKeyButton_Click(object sender, RoutedEventArgs e)
    {
        if (!IsLlmProvider) return;
        await _credentialStore.DeleteAsync(SelectedLlmApiKeyStoreKey);
        LlmApiKeyBox.Password = string.Empty;
        LlmCredentialStateText.Text = "Key 已清除";
        TranslationStatusText.Text = "已清除当前协议保存的 API Key。";
    }

    private LlmApiProfile ReadLlmProfileFromUi()
    {
        var baseUrl = LlmBaseUrlBox.Text.Trim();
        var model = LlmModelBox.Text.Trim();
        if (string.IsNullOrWhiteSpace(baseUrl))
            throw new InvalidOperationException("请填写 Base URL。");
        if (!Uri.TryCreate(baseUrl, UriKind.Absolute, out var uri) ||
            (uri.Scheme != Uri.UriSchemeHttp && uri.Scheme != Uri.UriSchemeHttps))
            throw new InvalidOperationException("Base URL 必须是有效的 http/https 地址。");
        if (string.IsNullOrWhiteSpace(model))
            throw new InvalidOperationException("请填写 Model。");

        return new LlmApiProfile(SelectedLlmProtocol, baseUrl, model);
    }

    private async Task<ITranslationProvider> CreateLlmProviderAsync()
    {
        var profile = ReadLlmProfileFromUi();
        await _credentialStore.SaveAsync(SelectedLlmProfileStoreKey, JsonSerializer.Serialize(profile));

        var key = LlmApiKeyBox.Password.Trim();
        if (!string.IsNullOrWhiteSpace(key))
        {
            await _credentialStore.SaveAsync(SelectedLlmApiKeyStoreKey, key);
            LlmApiKeyBox.Password = string.Empty;
        }
        else
        {
            key = await _credentialStore.LoadAsync(SelectedLlmApiKeyStoreKey) ?? string.Empty;
        }

        LlmCredentialStateText.Text = string.IsNullOrWhiteSpace(key)
            ? "配置已保存 · 无 Key"
            : "配置与 Key 已安全保存";

        return new CompatibleLlmTranslationProvider(profile, key);
    }

    private async void TranslateButton_Click(object sender, RoutedEventArgs e)
    {
        if (_outputPath is null) return;
        var paperIrPath = Path.Combine(_outputPath, "paper_ir.json");
        if (!File.Exists(paperIrPath))
        {
            TranslationStatusText.Text = "没有找到 paper_ir.json，请先分析论文。";
            return;
        }

        var providerId = SelectedProviderId;
        ITranslationProvider provider;

        switch (providerId)
        {
            case "free-auto":
                provider = new AutoFreeTranslationProvider();
                break;
            case "bing-free":
                provider = new BingFreeTranslationProvider();
                break;
            case "transmart-free":
                provider = new TencentTranSmartFreeTranslationProvider();
                break;
            case "google-free":
                provider = new GoogleFreeTranslationProvider();
                break;
            case "llm-openai":
            case "llm-anthropic":
                try
                {
                    provider = await CreateLlmProviderAsync();
                }
                catch (Exception ex)
                {
                    TranslationStatusText.Text = "大模型接口配置错误：" + ex.Message;
                    return;
                }
                break;
            default:
                TranslationStatusText.Text = "未知翻译引擎。";
                return;
        }

        TranslateButton.IsEnabled = false;
        AnalyzeButton.IsEnabled = false;
        ProviderCombo.IsEnabled = false;
        TranslationProgress.Value = 0;
        TranslationProgress.Visibility = Visibility.Visible;
        TranslationStatusText.Text = $"正在使用 {provider.DisplayName} 翻译正文与附录…";
        _translationCts = new CancellationTokenSource();

        var progress = new Progress<TranslationProgressInfo>(info =>
        {
            TranslationProgress.Value = info.Percent;
            TranslationStatusText.Text = info.Message;
        });

        try
        {
            var service = new PaperTranslationService(provider);
            var result = await service.TranslateAsync(paperIrPath, progress, _translationCts.Token);
            TranslationStatusText.Text = "翻译完成，正在排版并生成中文 PDF…";
            var pdfResult = await ReflowChinesePdfComposer.ComposeAsync(paperIrPath, _translationCts.Token);
            _translatedPdfPath = pdfResult.OutputPath;
            TranslationProgress.Value = 100;
            TranslationStatusText.Text = pdfResult.OverflowParagraphs == 0
                ? $"完成：{result.Total} 个正文/附录段落，缓存命中 {result.CacheHits} 段；Layout Engine V2 已重新分页并生成中文 PDF：{pdfResult.OutputPath}"
                : $"Layout Engine V2 已生成中文 PDF，但仍有 {pdfResult.OverflowParagraphs} 个块触发 QA；请查看 {pdfResult.QaPath}。";
            OpenTranslationButton.IsEnabled = File.Exists(pdfResult.OutputPath);
            OpenOutputButton.IsEnabled = true;
        }
        catch (OperationCanceledException)
        {
            TranslationStatusText.Text = "翻译已取消；已完成段落仍保留在缓存中。";
        }
        catch (Exception ex)
        {
            TranslationStatusText.Text = "翻译失败：" + ex.Message;
        }
        finally
        {
            _translationCts?.Dispose();
            _translationCts = null;
            TranslationProgress.Visibility = Visibility.Collapsed;
            TranslateButton.IsEnabled = true;
            AnalyzeButton.IsEnabled = true;
            ProviderCombo.IsEnabled = true;
            if (provider is IDisposable disposable) disposable.Dispose();
        }
    }

    private void OpenTranslationButton_Click(object sender, RoutedEventArgs e)
    {
        if (string.IsNullOrWhiteSpace(_translatedPdfPath) || !File.Exists(_translatedPdfPath)) return;
        Process.Start(new ProcessStartInfo(_translatedPdfPath) { UseShellExecute = true });
    }
}
