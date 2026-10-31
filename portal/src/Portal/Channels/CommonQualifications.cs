// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.Extensions.Options;
using Portal.Push;
namespace Portal.Channels;

public sealed class CommonQualifications(IOptions<PortalOptions> options,CommonEngine engine,CommonChannels channels,CommonPublicationFiles files)
{
    public async Task<JsonElement> Evaluate(CommonCatalogueView view,CommonBuild build,CancellationToken ct=default)
    {
        var settings=options.Value;
        if(settings.QualificationPolicyPath.Length==0)return Empty("not-evaluated");
        var scratch=Directory.CreateTempSubdirectory("apkg-portal-qualification-");
        using var stop=CancellationTokenSource.CreateLinkedTokenSource(ct);stop.CancelAfter(settings.CheckTimeout);
        try
        {
            var token=stop.Token;
            var observed=await channels.OpenSnapshot(view.Channel,view.Snapshot,token)??throw new InvalidDataException("Snapshot missing");
            var policyPath=Path.GetFullPath(settings.QualificationPolicyPath);
            var policy=await files.ReadBounded(policyPath,2*1024*1024,Path.GetDirectoryName(policyPath)!,token);
            var evaluation=JsonNode.Parse(policy)??throw new InvalidDataException("Invalid qualification policy");
            // The verified transport grant is private to this query; role grants stay unchanged.
            evaluation["body"]!["channels"]=JsonSerializer.SerializeToNode(new[]{new{id=view.ChannelId,keys=new[]{new{algorithm="ed25519",public_key=observed.PublicKey}}}});
            var seed=Path.Combine(scratch.FullName,"policy.json");
            await File.WriteAllBytesAsync(seed,CommonJson.Encode(evaluation),token);
            var directory=Path.Combine(settings.ChannelsDir,view.Channel,"snapshots",view.Snapshot);
            var captured=Path.Combine(scratch.FullName,"channel","snapshots",view.Snapshot);Directory.CreateDirectory(captured);
            var bytes=await files.ReadBounded(Path.Combine(directory,"channel.json"),2*1024*1024,settings.DataDir,token);
            var signature=await files.ReadBounded(Path.Combine(directory,"channel.signature.json"),16384,settings.DataDir,token);
            if(CommonJson.Digest(bytes)!=view.Snapshot)throw new InvalidDataException("Snapshot changed");
            await File.WriteAllBytesAsync(Path.Combine(captured,"channel.json"),bytes,token);
            await File.WriteAllBytesAsync(Path.Combine(captured,"channel.signature.json"),signature,token);
            var endpoint=Path.Combine(scratch.FullName,"channel");await File.WriteAllTextAsync(Path.Combine(endpoint,"current"),view.Snapshot+"\n",token);
            var root=Path.Combine(scratch.FullName,"root");
            await engine.RunText(settings.ApkgPath,["init",root,build.Target,seed,"--yes"],token);
            var result=await engine.Run(settings.ApkgPath,["channel-updates",root,endpoint,view.ChannelId],token,outputLimit:16*1024*1024);
            var row=result.GetProperty("entries").EnumerateArray().Single(entry=>entry.GetProperty("selection").GetString()!.EndsWith('/'+build.Digest,StringComparison.Ordinal));
            var claims=row.GetProperty("attestations").EnumerateArray().Select(observation=>
            {
                var body=observation.GetProperty("record").GetProperty("body");
                return new{record_sha256=observation.GetProperty("record_sha256"),kind=body.GetProperty("kind"),result=body.GetProperty("result"),
                    issuer=body.GetProperty("issuer"),sequence=body.GetProperty("sequence"),issuer_trust=observation.GetProperty("issuer_trust"),
                    independent=observation.GetProperty("independent"),history=observation.GetProperty("history"),
                    target_matches=observation.GetProperty("target_matches"),freshness=observation.GetProperty("freshness")};
            }).ToArray();
            return JsonSerializer.SerializeToElement(new{status="evaluated",policy_sha256=CommonJson.Digest(policy),observations=claims,
                current_approval="not-asserted",client_trust="not-granted",freshness="unknown"});
        }
        catch(Exception error) when(error is IOException or InvalidDataException or System.ComponentModel.Win32Exception or JsonException or OperationCanceledException or NullReferenceException or InvalidOperationException or KeyNotFoundException)
        {if(ct.IsCancellationRequested)throw;return Empty("unavailable");}
        finally{scratch.Delete(recursive:true);}
    }
    static JsonElement Empty(string status)=>JsonSerializer.SerializeToElement(new{status,observations=Array.Empty<object>(),current_approval="not-asserted",freshness="unknown"});
}
