// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Text.Json;
using System.Security.Cryptography;
using Microsoft.Extensions.Options;

namespace Portal.Channels;

/// A maintainer's observation about exact published bytes. Author metadata
/// cannot create these records. Runtime reports and editorial review are
/// independent, and a report's target includes the ABI and runtime tested.
public sealed record QualificationRecord(string Kind, string Result, string Target,
    string By, DateTimeOffset When, string Summary, string Evidence);

public sealed record Qualification(string Channel, string Name, string Version,
    string Architecture, string Manifest, List<QualificationRecord> History)
{
    public string TargetIdentity => Qualifications.IsQualifiedTarget(Architecture) ? "qualified" : Architecture == "generic" ? "generic" : "cpu-only";
    public string Review => History.LastOrDefault(r => r.Kind == "review")?.Result ?? "unreviewed";
    public IEnumerable<QualificationRecord> RuntimeTests => History.Where(r => r.Kind == "runtime")
        .GroupBy(r => r.Target, StringComparer.Ordinal).Select(g => g.Last());
}

public sealed record QualificationRequest(string? Result, string? Summary, string? Evidence, string? Target = null);

/// Records live outside publisher-writable channels. A replacement file retains
/// the whole history and is flushed before its atomic rename. One portal process
/// owns a state directory, as with the account registry.
public sealed class Qualifications(IOptions<PortalOptions> options)
{
    readonly object gate = new();
    readonly string state = options.Value.StateDir;
    readonly string channels = options.Value.ChannelsDir;
    static readonly JsonSerializerOptions Json = new(JsonSerializerDefaults.Web) { WriteIndented = true };

    static bool Digest(string digest) => digest.Length == 64 && digest.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f');
    string PathFor(string channel, string digest) => Path.Combine(state, channel, "qualifications", digest + ".json");

    public Qualification Read(string channel, IndexLine line)
    {
        if (!ChannelPaths.IsChannelName(channel) || !Digest(line.Digest)) throw new ArgumentException("invalid qualification identity");
        lock (gate)
        {
            var path = PathFor(channel, line.Digest);
            if (!File.Exists(path)) return new(channel, line.Name, line.Version, line.Arch, line.Digest, []);
            var q = JsonSerializer.Deserialize<Qualification>(File.ReadAllText(path), Json);
            if (q is null || q.Channel != channel || q.Name != line.Name || q.Version != line.Version
                || q.Architecture != line.Arch || q.Manifest != line.Digest || q.History is null)
                throw new InvalidDataException("qualification identity differs from the published manifest");
            return q;
        }
    }

    // Keep the wire syntax aligned with include/pkg_target.h. This validates
    // an observation's identity; executing a test is the reviewer's responsibility.
    public static bool IsQualifiedTarget(string? value)
    {
        var p = value?.Split('-');
        return p is { Length: 4 } && p[0] is ("x86_64" or "i386" or "aarch64" or "arm" or "m68k" or "ppc")
            && p[1] == "aros" && p[2] is ("v0" or "v1" or "v11")
            && p[3] is ("native" or "hosted" or "raspi") && (p[3] != "raspi" || p[0] is ("arm" or "aarch64"));
    }

    public Qualification Add(string channel, IndexLine line, string kind, string reviewer, QualificationRequest request)
    {
        if (kind is not ("review" or "runtime")) throw new ArgumentException("unknown qualification kind");
        if (kind == "review" && request.Result is not ("reviewed" or "rejected" or "revoked"))
            throw new ArgumentException("review result must be reviewed, rejected or revoked");
        if (kind == "runtime" && request.Result is not ("passed" or "failed"))
            throw new ArgumentException("runtime result must be passed or failed");
        static bool Text(string? value, int max) => !string.IsNullOrWhiteSpace(value) && value.Length <= max && !value.Any(char.IsControl);
        if (!Text(reviewer, 128) || !Text(request.Summary, 1000)) throw new ArgumentException("a summary of at most 1000 characters is required");
        if (!Text(request.Evidence, 2048) || !Uri.TryCreate(request.Evidence, UriKind.Absolute, out var evidence)
            || evidence.Scheme != "https" || evidence.UserInfo.Length != 0)
            throw new ArgumentException("evidence must be an https URL without credentials");
        if (kind == "runtime" && (!IsQualifiedTarget(request.Target)
            || line.Arch != "generic" && (IsQualifiedTarget(line.Arch)
                ? line.Arch != request.Target : line.Arch != request.Target!.Split('-')[0])))
            throw new ArgumentException("runtime target must be CPU-aros-ABI-runtime and match the package target; a CPU-only package establishes only CPU identity");
        if (kind == "review" && !string.IsNullOrEmpty(request.Target))
            throw new ArgumentException("a review covers its manifest; target belongs to a runtime report");
        lock (gate)
        {
            var q = Read(channel, line);
            var bytes = File.ReadAllBytes(Path.Combine(channels, channel, "objects", line.Digest + ".manifest"));
            if (Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant() != line.Digest)
                throw new ArgumentException("published manifest bytes differ from their digest");
            var manifest = Manifest.Parse(System.Text.Encoding.UTF8.GetString(bytes));
            if (manifest.Name != line.Name || manifest.Version != line.Version || manifest.Architecture != line.Arch)
                throw new ArgumentException("published manifest identity differs from the index");
            if (q.History.Count >= 4096) throw new InvalidOperationException("qualification history limit reached");
            q.History.Add(new(kind, request.Result!, kind == "runtime" ? request.Target! : "",
                reviewer, DateTimeOffset.UtcNow, request.Summary!, request.Evidence!));
            var path = PathFor(channel, line.Digest);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            var temp = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
            try
            {
                using (var stream = new FileStream(temp, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    JsonSerializer.Serialize(stream, q, Json);
                    stream.Flush(true);
                }
                File.Move(temp, path, true);
            }
            finally { if (File.Exists(temp)) File.Delete(temp); }
            return q;
        }
    }
}
