using System.Collections.Generic;
using System.Threading;
using System.Threading.Tasks;

namespace Palimpsest_App.Translation;

public interface ITranslationProvider
{
    string Id { get; }
    string DisplayName { get; }

    Task<IReadOnlyList<string>> TranslateBatchAsync(
        IReadOnlyList<string> texts,
        CancellationToken cancellationToken = default);
}

