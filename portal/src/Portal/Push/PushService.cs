// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.Security.Cryptography;
using Microsoft.Extensions.Options;
using Portal.Channels;

namespace Portal.Push;

/// Authenticated resumable staging and common-engine-verified immutable publication.
public sealed partial class PushService(IOptions<PortalOptions> options, ILogger<PushService> log, CommonEngine common, CommonPublicationFiles files)
{
    readonly PortalOptions o = options.Value;
    static readonly ConcurrentDictionary<string, SemaphoreSlim> Locks = new();
    static readonly ConcurrentDictionary<string, SemaphoreSlim> KeyLocks = new();

    /// One writer per channel at a time: pushes and admin changes alike.
    public static SemaphoreSlim LockFor(string channel) => Locks.GetOrAdd(channel, _ => new SemaphoreSlim(1, 1));

    sealed record PlanLine(string Path, string Sha, long Size);

    string Live(string channel) => Path.Combine(o.ChannelsDir, channel);
    string Staging(Publisher who, string channel) => Path.Combine(o.StagingDir, "accounts", who.StagingId, channel);
    static string PlanFile(string staging) => Path.Combine(staging, ".plan");
    static string StatsFile(string staging) => Path.Combine(staging, ".stats");

    // ---- plan ---------------------------------------------------------------

    public async Task<Record> Plan(Publisher who, string channel, string body)
    {
        RemoveExpiredStaging();
        var keyGate = KeyLocks.GetOrAdd(who.StagingId, _ => new SemaphoreSlim(1, 1));
        await keyGate.WaitAsync();
        try { return await PlanLocked(who, channel, body); }
        finally { keyGate.Release(); }
    }

    async Task<Record> PlanLocked(Publisher who, string channel, string body)
    {
        var staging = Staging(who, channel);
        Directory.CreateDirectory(staging);
        var plan = ReadPlan(staging);
        var r = new Record().Add("result", "planned");
        int need = 0, have = 0, refused = 0, line = 0;
        long needBytes = 0, haveBytes = 0;
        foreach (var raw in body.Split('\n'))
        {
            line++;
            var text = raw.Trim();
            if (text.Length == 0) continue;
            // <relative path> <sha256> <size>; the path is everything before the last two fields.
            int b = text.LastIndexOf(' '), a = b > 0 ? text.LastIndexOf(' ', b - 1) : -1;
            if (a <= 0 || !long.TryParse(text[(b + 1)..], NumberStyles.None, CultureInfo.InvariantCulture, out var size)
                || !IsHex64(text[(a + 1)..b]))
            {
                r.Add("refused", $"line {line} 20 not '<path> <sha256> <size>'");
                refused++;
                continue;
            }
            var p = new PlanLine(text[..a], text[(a + 1)..b].ToLowerInvariant(), size);
            var why = Refusal(p.Path) ?? LinkOnly(who, p.Path, o.Policy);
            if (why is not null) { r.Add("refused", $"{p.Path} 20 {why}"); refused++; continue; }
            if (ChannelPaths.Classify(p.Path) == ChannelPaths.Kind.Snapshot &&
                p.Size > (p.Path.EndsWith(".signature.json", StringComparison.Ordinal) ? 16384 : 2 * 1024 * 1024))
            { r.Add("refused", $"{p.Path} 20 common metadata exceeds its contract bound"); refused++; continue; }
            if (ChannelPaths.PromisedDigest(p.Path) is { } promised && promised != p.Sha)
            {
                r.Add("refused", $"{p.Path} 12 its name promises {promised} but the plan gives {p.Sha}");
                refused++;
                continue;
            }
            var live = Path.Combine(Live(channel), p.Path);
            if (Published(live))
            {
                var liveSha = await LiveSha(channel, p.Path);
                if (liveSha == p.Sha) { have++; haveBytes += p.Size; continue; }
                if (ChannelPaths.Immutable(p.Path))
                {
                    r.Add("refused", $"{p.Path} 15 already published with other content, and a published file never changes");
                    refused++;
                    continue;
                }
            }
            var staged = Path.Combine(staging, p.Path);
            if (File.Exists(staged) && plan.TryGetValue(p.Path, out var old) && old.Sha == p.Sha)
            {
                have++; haveBytes += p.Size;
                plan[p.Path] = p;
                continue;
            }
            if (File.Exists(staged)) File.Delete(staged);
            plan[p.Path] = p;
            r.Add("need", p.Path);
            need++; needBytes += p.Size;
        }
        WritePlan(staging, plan);
        File.WriteAllText(StatsFile(staging), $"started {DateTime.UtcNow:O}\nskipped {have} {haveBytes}\n");
        r.Add("files-needed", need).Add("bytes-needed", needBytes)
         .Add("files-skipped", have).Add("bytes-skipped", haveBytes);
        if (refused > 0) r.Add("files-refused", refused);
        r.Add("summary", (need == 0 ? "the portal already has every file" : $"{need} file{S(need)} to send ({Record.Size(needBytes)})")
            + (have > 0 && need > 0 ? $"; {have} already on the portal, {Record.Size(haveBytes)} not sent again" : "")
            + (refused > 0 ? $"; {refused} refused, each with its reason above" : ""));
        return r;
    }

    // ---- files --------------------------------------------------------------

    public async Task<(Record Answer, int Status)> PutFile(Publisher who, string channel, string path,
                                                           string? contentRange, long? contentLength, Stream body,
                                                           CancellationToken ct)
    {
        // Planning, uploading and committing share the publisher's staging and quota.
        var keyGate = KeyLocks.GetOrAdd(who.StagingId, _ => new SemaphoreSlim(1, 1));
        await keyGate.WaitAsync(ct);
        try { return await PutFileLocked(who, channel, path, contentRange, contentLength, body, ct); }
        finally { keyGate.Release(); }
    }

    async Task<(Record Answer, int Status)> PutFileLocked(Publisher who, string channel, string path,
                                                        string? contentRange, long? contentLength, Stream body,
                                                        CancellationToken ct)
    {
        if (Refusal(path) is { } why) return (Record.Refused(20, $"{path}: {why}", "check the path"), 400);
        if (LinkOnly(who, path, o.Policy) is { } linkOnly)
            return (Record.Refused(20, $"{path}: {linkOnly}", "provide signed snapshot metadata and external archive links"), 403);
        var staging = Staging(who, channel);
        var plan = ReadPlan(staging);
        if (!plan.TryGetValue(path, out var want))
            return (Record.Refused(20, $"{path} is not in this push's plan", "send the plan first, then the files it lists"), 400);
        var live = Path.Combine(Live(channel), path);
        if (Published(live) && ChannelPaths.Immutable(path))
        {
            if (await LiveSha(channel, path) == want.Sha)
                return (new Record().Add("result", "unchanged").Add("path", path)
                    .Add("summary", $"{path} is already published with these bytes"), 200);
            return (Record.Refused(15, $"{path} is already published with other content, and a published file never changes",
                "publish a new version instead"), 409);
        }

        long start = 0, end = want.Size - 1;
        if (contentRange is not null)
        {
            if (!TryRange(contentRange, out start, out end, out var total) || total != want.Size)
                return (Record.Refused(20, $"Content-Range '{contentRange}' does not fit a file of {want.Size} bytes",
                    "send 'bytes <first>-<last>/<size>'"), 400);
        }
        else if (want.Size > o.MaxPartBytes)
        {
            return (Record.Refused(20, $"{path} is {Record.Size(want.Size)}, more than one request carries",
                $"send it in parts of at most {o.MaxPartBytes} bytes with Content-Range"), 400);
        }

        var final = Path.Combine(staging, path);
        var part = final + ".part";
        var cap = who.Files ? o.MaxStagingBytes : o.MaxLinkOnlyStagingBytes;
        var keyRoot = Path.Combine(o.StagingDir, "accounts", who.StagingId);
        var held = Directory.Exists(keyRoot)
            ? new DirectoryInfo(keyRoot).EnumerateFiles("*", SearchOption.AllDirectories).Sum(f => f.Length) : 0;
        if (held + (end - start + 1) > cap)
            return (Record.Refused(20, $"this push would hold {Record.Size(held + end - start + 1)} in staging, more than the {Record.Size(cap)} a key may hold",
                "commit or let the staged files expire, then push again"), 413);
        Directory.CreateDirectory(Path.GetDirectoryName(final)!);
        long received = File.Exists(part) ? new FileInfo(part).Length : 0;
        if (File.Exists(final))
            return (new Record().Add("result", "received").Add("path", path).Add("received", want.Size)
                .Add("summary", $"{path} was already received"), 200);
        if (start != received)
            // A resumed upload: the client continues from what the portal holds.
            return (new Record().Add("result", "partial").Add("path", path).Add("received", received)
                .Add("summary", $"the portal holds {received} of {want.Size} bytes; continue from byte {received}"), 200);

        long wrote;
        await using (var f = new FileStream(part, FileMode.Append, FileAccess.Write, FileShare.None, 1 << 16, true))
        {
            var before = f.Length;
            await body.CopyToAsync(f, ct);
            wrote = f.Length - before;
            if (wrote != end - start + 1)
            {
                f.SetLength(before);
                return (Record.Refused(20, $"the part carried {wrote} bytes where its range says {end - start + 1}",
                    $"send the part from byte {before} again"), 400);
            }
        }
        AddStats(staging, 1, wrote, partOnly: end + 1 < want.Size);
        received = end + 1;
        if (received < want.Size)
            return (new Record().Add("result", "partial").Add("path", path).Add("received", received)
                .Add("summary", $"{Record.Size(received)} of {Record.Size(want.Size)} received"), 200);

        var sha = await Sha(part, ct);
        if (sha != want.Sha)
        {
            File.Delete(part);
            return (Record.Refused(12, $"{path} arrived with SHA-256 {sha}, not the {want.Sha} its plan gave",
                "send the file again from byte 0"), 200);
        }
        File.Move(part, final, overwrite: true);
        return (new Record().Add("result", "received").Add("path", path).Add("received", received)
            .Add("summary", $"{path} received and checked, {Record.Size(want.Size)}"), 200);
    }

    static string? LinkOnly(Publisher who, string path, PortalPolicy policy) =>
        ChannelPaths.Classify(path) == ChannelPaths.Kind.Archive && (!who.Files || !policy.BinariesAllowed)
            ? "this publisher uploads metadata only; provide an external archive link at commit" : null;
    static string? Refusal(string path) => ChannelPaths.Classify(path) switch
    {
        ChannelPaths.Kind.Current => "current changes only through an authenticated common commit",
        ChannelPaths.Kind.None => "only common snapshots and digest-named PKG/1 archives are accepted",
        _ => null,
    };
    static bool Published(string path) => File.Exists(path) || File.Exists(path + ".url");
    static void ForgetPlanned(string staging, string live)
    {
        // Drop only files that went live; retain pending uploads, including unstarted ones.
        var plan = ReadPlan(staging);
        foreach (var k in plan.Keys.ToList())
            if (Published(Path.Combine(live, k)) && !File.Exists(Path.Combine(staging, k))
                && !File.Exists(Path.Combine(staging, k + ".part"))) plan.Remove(k);
        WritePlan(staging, plan);
    }

    async Task<string> LiveSha(string channel, string rel)
    {
        if (ChannelPaths.PromisedDigest(rel) is { } promised) return promised;
        var path = Path.Combine(Live(channel), rel);
        return await Sha(path, default);
    }

    static async Task<string> Sha(string path, CancellationToken ct)
    {
        await using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, true);
        return Convert.ToHexString(await SHA256.HashDataAsync(f, ct)).ToLowerInvariant();
    }

    static Dictionary<string, PlanLine> ReadPlan(string staging)
    {
        var d = new Dictionary<string, PlanLine>(StringComparer.Ordinal);
        var p = PlanFile(staging);
        if (!File.Exists(p)) return d;
        foreach (var l in File.ReadLines(p))
        {
            var f = l.Split('\t');
            if (f.Length == 3) d[f[0]] = new PlanLine(f[0], f[1], long.Parse(f[2], CultureInfo.InvariantCulture));
        }
        return d;
    }

    static void WritePlan(string staging, Dictionary<string, PlanLine> plan)
    {
        Directory.CreateDirectory(staging);
        File.WriteAllLines(PlanFile(staging), plan.Values.Select(p => $"{p.Path}\t{p.Sha}\t{p.Size}"));
    }

    sealed record Stats(DateTime? Started, int Files, long Bytes, int SkippedFiles, long SkippedBytes);

    static Stats ReadStats(string staging)
    {
        DateTime? started = null; int files = 0, sf = 0; long bytes = 0, sb = 0;
        if (File.Exists(StatsFile(staging)))
            foreach (var l in File.ReadLines(StatsFile(staging)))
            {
                var f = l.Split(' ');
                if (f[0] == "started" && DateTime.TryParse(f[1], null, DateTimeStyles.RoundtripKind, out var t)) started = t;
                else if (f[0] == "skipped") { sf = int.Parse(f[1]); sb = long.Parse(f[2]); }
                else if (f[0] == "sent") { files += int.Parse(f[1]); bytes += long.Parse(f[2]); }
            }
        return new Stats(started, files, bytes, sf, sb);
    }

    static void AddStats(string staging, int files, long bytes, bool partOnly) =>
        File.AppendAllText(StatsFile(staging), $"sent {(partOnly ? 0 : files)} {bytes}\n");

    void RemoveExpiredStaging()
    {
        var accounts = Path.Combine(o.StagingDir, "accounts");
        if (!Directory.Exists(accounts)) return;
        foreach (var who in Directory.EnumerateDirectories(accounts).Where(p => IsHex64(Path.GetFileName(p))))
        {
            var keyGate = KeyLocks.GetOrAdd(Path.GetFileName(who), _ => new SemaphoreSlim(1, 1));
            if (!keyGate.Wait(0)) continue;
            try
            {
                foreach (var ch in Directory.EnumerateDirectories(who))
                {
                    // The last file written, so an upload in progress is never taken away.
                    var when = new DirectoryInfo(ch).EnumerateFiles("*", SearchOption.AllDirectories)
                        .Select(f => f.LastWriteTimeUtc).DefaultIfEmpty(Directory.GetLastWriteTimeUtc(ch)).Max();
                    if (DateTime.UtcNow - when > o.StagingLifetime)
                    {
                        try { Directory.Delete(ch, true); log.LogInformation("removed expired staging {Dir}", ch); }
                        catch (IOException) { }
                    }
                }
            }
            finally { keyGate.Release(); }
        }
    }

    static bool TryRange(string header, out long start, out long end, out long total)
    {
        start = end = total = -1;
        // bytes <first>-<last>/<size>
        if (!header.StartsWith("bytes ", StringComparison.Ordinal)) return false;
        var s = header[6..].Split('/', 2);
        if (s.Length != 2) return false;
        var r = s[0].Split('-', 2);
        return r.Length == 2
            && long.TryParse(r[0], NumberStyles.None, CultureInfo.InvariantCulture, out start)
            && long.TryParse(r[1], NumberStyles.None, CultureInfo.InvariantCulture, out end)
            && long.TryParse(s[1], NumberStyles.None, CultureInfo.InvariantCulture, out total)
            && start <= end && end < total;
    }

    static bool IsHex64(string s) => s.Length == 64 && s.All(Uri.IsHexDigit);
    static string S(int n) => n == 1 ? "" : "s";
}
