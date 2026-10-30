// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Portal.Channels;

public sealed record CommonBuild(string Digest, JsonElement Descriptor, JsonElement AuthorSignature,
                                  JsonElement Archive, JsonElement Attestations)
{
    [JsonIgnore] public JsonElement Body => Descriptor.GetProperty("body");
    public string Authority => Body.GetProperty("authority").GetString()!;
    public string Name => Body.GetProperty("name").GetString()!;
    public string Version => Body.GetProperty("version").GetString()!;
    // Keep all 64 bits as text through JavaScript/JSON consumers.
    public string Revision => Body.GetProperty("revision").GetString()!;
    public string Target => Body.GetProperty("target").GetString()!;
    public string Kind => Body.GetProperty("kind").GetString()!;
    public string? Summary => Presentation("summary");
    public string? Category => Presentation("category");
    public string? License => Presentation("license");
    public string? Repository => Presentation("repository");
    [JsonIgnore] public IEnumerable<JsonElement> Files => Body.GetProperty("files").EnumerateArray();
    string? Presentation(string field) => Body.TryGetProperty("presentation", out var p)
        && p.TryGetProperty(field, out var value) ? value.GetString() : null;
}

public sealed record CommonCatalogueView(string Channel, string Snapshot, string ChannelId,
    string ChannelPublicKey, string Sequence, IReadOnlyList<CommonBuild> Builds)
{
    public string Signature => "verified";
    public string Trust => "not-evaluated";
    public string Review => "not-evaluated";
    public string RuntimeTesting => "not-evaluated";
    public string Payload => "not-checked";
    public string Freshness => "unknown";

    public IEnumerable<CommonBuild> Search(string query = "", string authority = "", string target = "") =>
        Builds.Where(b => (authority.Length == 0 || b.Authority == authority)
            && (target.Length == 0 || b.Target == target)
            && (query.Length == 0 || new[] { b.Name, b.Summary, b.Category, b.Authority }
                .Any(s => s?.Contains(query, StringComparison.OrdinalIgnoreCase) == true)
                || b.Files.Any(f => f.GetProperty("path").GetString()!
                    .Contains(query, StringComparison.OrdinalIgnoreCase))))
            .OrderBy(b => b.Name, StringComparer.Ordinal).ThenBy(b => b.Authority, StringComparer.Ordinal)
            .ThenBy(b => b.Target, StringComparer.Ordinal).ThenBy(b => b.Digest, StringComparer.Ordinal);

    public string BuildUrl(CommonBuild build) =>
        $"/packages/apkg/builds/{Channel}/{build.Digest}?snapshot={Snapshot}";
}

public sealed record CommonCatalogueResult(int Status, string? Error, CommonCatalogueView? View);

/// Presentation over common-engine-verified bytes, with no second schema/crypto engine,
/// version ordering or interpretation of reviewer/tester authorization in the portal.
public sealed class CommonCatalogue(CommonChannels channels, Catalogue legacy)
{
    public IEnumerable<string> ChannelNames() => legacy.ChannelNames()
        .Where(name => legacy.SeesUnlisted() || !legacy.IsUnlisted(name))
        .Where(channels.HasReference);

    public async Task<CommonCatalogueResult> Read(string channel, string? snapshot = null,
                                                  CancellationToken ct = default)
    {
        try
        {
            var observed = snapshot is null ? await channels.Open(channel, ct)
                : await channels.OpenSnapshot(channel, snapshot, ct);
            if (observed is null) return new(404, "No signed APKG snapshot found.", null);
            var body = observed.Document.GetProperty("body");
            var builds = body.GetProperty("entries").EnumerateArray().Select(entry =>
            {
                var descriptor = entry.GetProperty("descriptor");
                var digest = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(descriptor.GetRawText())));
                return new CommonBuild(digest, descriptor, entry.GetProperty("signature"), entry.GetProperty("archive"),
                    entry.TryGetProperty("attestations", out var claims) ? claims : JsonSerializer.SerializeToElement(Array.Empty<object>()));
            }).ToArray();
            return new(200, null, new(channel, observed.Snapshot, body.GetProperty("id").GetString()!,
                observed.PublicKey, body.GetProperty("sequence").GetString()!, builds));
        }
        catch (InvalidDataException) { return new(422, "The signed APKG catalogue could not be verified.", null); }
        catch (OperationCanceledException) when (!ct.IsCancellationRequested)
        { return new(504, "APKG catalogue verification timed out. Please try again.", null); }
        catch (Exception error) when (error is IOException or System.ComponentModel.Win32Exception or JsonException)
        { return new(503, "APKG catalogue verification is temporarily unavailable.", null); }
    }
}
