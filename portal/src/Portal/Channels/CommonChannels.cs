// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Microsoft.Extensions.Options;
using Portal.Push;

namespace Portal.Channels;

public sealed record CommonChannel(string Snapshot, string PublicKey, JsonElement Document)
{
    public string Signature => "verified";
    public string Trust => "not-evaluated";
    public string Review => "not-evaluated";
    public string RuntimeTesting => "not-evaluated";
    public string Payload => "not-checked";
    public string Freshness => "unknown";
}

/// A verified observation of the immutable common contract, without granting client roles.
public sealed partial class CommonChannels(IOptions<PortalOptions> options, CommonEngine engine)
{
    [GeneratedRegex("^[0-9a-f]{64}\\n\\z")]
    private static partial Regex Reference();

    [GeneratedRegex("^[0-9a-f]{64}\\z")]
    private static partial Regex Digest();

    public bool HasReference(string channel) => ChannelPaths.IsChannelName(channel)
        && File.Exists(Path.Combine(options.Value.ChannelsDir, channel, "current"));

    async Task<byte[]> Read(string path, int bound, CancellationToken ct)
    {
        // Refuse links inside the operator-owned data tree. Ancestors of that configured
        // tree may legitimately be OS aliases (for example the macOS temporary directory).
        var root = Path.TrimEndingDirectorySeparator(Path.GetFullPath(options.Value.DataDir));
        for (var item = new FileInfo(Path.GetFullPath(path)) as FileSystemInfo;
             item is not null; item = Path.GetDirectoryName(item.FullName) is { } parent ? new DirectoryInfo(parent) : null)
        {
            if ((item.Attributes & FileAttributes.ReparsePoint) != 0)
                throw new InvalidDataException("linked channel member");
            if (item.FullName == root) break;
        }
        await using var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        if (file.Length > bound) throw new InvalidDataException("channel metadata exceeds bound");
        using var result = new MemoryStream();
        var buffer = new byte[8192];
        int n;
        while ((n = await file.ReadAsync(buffer, ct)) != 0)
        {
            if (result.Length + n > bound) throw new InvalidDataException("channel metadata exceeds bound");
            result.Write(buffer, 0, n);
        }
        return result.ToArray();
    }

    public async Task<CommonChannel?> Open(string channel, CancellationToken ct = default)
    {
        if (!ChannelPaths.IsChannelName(channel)) return null;
        var directory = Path.Combine(options.Value.ChannelsDir, channel);
        var head = Path.Combine(directory, "current");
        if (!File.Exists(head)) return null;
        var before = await Read(head, 65, ct);
        var reference = Encoding.ASCII.GetString(before);
        if (!Reference().IsMatch(reference)) throw new InvalidDataException("invalid common current reference");
        var digest = reference[..64];
        var observed = await ReadSnapshot(directory, digest, ct);
        var after = await Read(head, 65, ct);
        if (!before.AsSpan().SequenceEqual(after))
            throw new InvalidDataException("common channel changed during observation");
        return observed;
    }

    /// Exact-build links keep their signed snapshot when the channel advances.
    /// An historical observation makes no claim about the current channel head.
    public async Task<CommonChannel?> OpenSnapshot(string channel, string digest, CancellationToken ct = default)
    {
        if (!ChannelPaths.IsChannelName(channel) || !Digest().IsMatch(digest))
            return null;
        var directory = Path.Combine(options.Value.ChannelsDir, channel);
        return Directory.Exists(Path.Combine(directory, "snapshots", digest))
            ? await ReadSnapshot(directory, digest, ct) : null;
    }

    async Task<CommonChannel> ReadSnapshot(string directory, string digest, CancellationToken ct)
    {
        var snapshot = Path.Combine(directory, "snapshots", digest);
        var document = await Read(Path.Combine(snapshot, "channel.json"), 2 * 1024 * 1024, ct);
        var signature = await Read(Path.Combine(snapshot, "channel.signature.json"), 16384, ct);
        if (Convert.ToHexStringLower(SHA256.HashData(document)) != digest)
            throw new InvalidDataException("common snapshot digest mismatch");
        var temporary = Directory.CreateTempSubdirectory("apkg-portal-verify-");
        try
        {
            var doc = Path.Combine(temporary.FullName, "channel.json");
            var sig = Path.Combine(temporary.FullName, "channel.signature.json");
            await File.WriteAllBytesAsync(doc, document, ct);
            await File.WriteAllBytesAsync(sig, signature, ct);
            var verified = await engine.Verify("channel", doc, sig, ct);
            if (verified.GetProperty("sha256").GetString() != digest ||
                verified.GetProperty("domain").GetString() != "channel")
                throw new InvalidDataException("common verifier identity mismatch");
            using var json = JsonDocument.Parse(document);
            return new(digest, verified.GetProperty("public_key").GetString()!, json.RootElement.Clone());
        }
        finally { temporary.Delete(recursive: true); }
    }
}
