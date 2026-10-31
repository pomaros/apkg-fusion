// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;
using Portal.Channels;

namespace Portal.Push;

[JsonUnmappedMemberHandling(JsonUnmappedMemberHandling.Disallow)]
public sealed record CommonCommitRequest
{
    [JsonRequired] public string? Snapshot { get; init; }
    [JsonRequired] public string? ExpectedCurrent { get; init; }
    public Dictionary<string, string>? Links { get; init; }
}

public sealed partial class PushService
{
    static Record UncertainCommonCommit(string reason) => new Record().Add("result", "uncertain")
        .Add("reason", reason).Add("summary", "current may already name this snapshot; inspect it before retrying")
        .Add("next", "retry the same signed snapshot and expectedCurrent values");
    [GeneratedRegex("^[0-9a-f]{64}\\z")]
    private static partial Regex CommonDigest();

    public async Task<(Record Answer, int Status)> CommitCommon(Publisher who, string channel,
                                                               CommonCommitRequest request, CancellationToken ct,
                                                               bool allowLoopbackLinks = false)
    {
        if (request.Snapshot is null || !CommonDigest().IsMatch(request.Snapshot) ||
            (request.ExpectedCurrent is not null && !CommonDigest().IsMatch(request.ExpectedCurrent)))
            return (Record.Refused(20, "common commit requires exact lowercase snapshot and expectedCurrent digests", "rebuild the snapshot"), 400);
        var keyGate = KeyLocks.GetOrAdd(who.StagingId, _ => new SemaphoreSlim(1, 1));
        await keyGate.WaitAsync(ct);
        try
        {
            var gate = LockFor(channel);
            await gate.WaitAsync(ct);
            try
            {
                var staging = Staging(who, channel);
                Directory.CreateDirectory(staging);
                JsonElement result;
                var scratch = Directory.CreateTempSubdirectory("apkg-publication-");
                try
                {
                    using var stop=CancellationTokenSource.CreateLinkedTokenSource(ct);
                    stop.CancelAfter(o.CheckTimeout);
                    result=await new CommonPublication(o,common,files).Commit(who,channel,staging,request,scratch.FullName,allowLoopbackLinks,stop.Token);
                }
                catch(PublicationRefusal refusal)
                {
                    return (Record.Refused(refusal.Code=="owner"?14:refusal.Code=="conflict"?15:12,refusal.Message,
                        "inspect current and rebuild the signed snapshot if it changed"),refusal.Code=="owner"?403:refusal.Code=="conflict"?409:422);
                }
                catch (OperationCanceledException) when (!ct.IsCancellationRequested)
                { return (UncertainCommonCommit("common commit timed out"), 504); }
                catch (System.ComponentModel.Win32Exception)
                { return (UncertainCommonCommit("common commit verifier unavailable"),503); }
                catch (Exception error) when (error is IOException or InvalidDataException or JsonException or HttpRequestException or OverflowException)
                {
                    log.LogWarning(error,"Common publication could not finish for channel {Channel}",channel);
                    try
                    {
                        if(await files.Current(Live(channel),o.DataDir,CancellationToken.None)!=request.Snapshot)
                            return (Record.Refused(12,"common publication verification failed","check the signed snapshot and archives"),422);
                    }
                    catch(Exception readError) when(readError is IOException or InvalidDataException){}
                    return (UncertainCommonCommit("inspect current and retry the same signed snapshot"),503);
                }
                finally { scratch.Delete(recursive: true); }
                // Retain unrelated uploads and remove only exact files now durably published.
                foreach (var item in ReadPlan(staging).Values)
                {
                    var source = Path.Combine(staging, item.Path);
                    var destination = Path.Combine(Live(channel), item.Path);
                    if (File.Exists(source) && File.Exists(destination) &&
                        ChannelPaths.Immutable(item.Path) && await Sha(destination, ct) == item.Sha)
                        File.Delete(source);
                }
                ForgetPlanned(staging, Live(channel));
                return (new Record().Add("result", result.GetProperty("result").GetString()!)
                    .Add("snapshot", request.Snapshot).Add("sequence", result.GetProperty("sequence").GetString()!)
                    .Add("published", result.GetProperty("added").GetInt32())
                    .Add("trust", "not-granted").Add("review", "not-granted")
                    .Add("summary", "signed common snapshot retained; publication grants no client trust or review"), 200);
            }
            finally { gate.Release(); }
        }
        finally { keyGate.Release(); }
    }
}
