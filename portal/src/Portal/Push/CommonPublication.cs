// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
using System.Globalization;
using System.Text;
using System.Text.Json;
namespace Portal.Push;

public sealed class PublicationRefusal(string code,string message):Exception(message)
{ public string Code { get; }=code; }

/// Account ownership and immutable publication; package semantics stay in apkg.
public sealed class CommonPublication(PortalOptions options,CommonEngine engine,CommonPublicationFiles files)
{
    const int MetadataMax=2*1024*1024;
    readonly string root=options.DataDir;
    static void Refuse(string code,string reason)=>throw new PublicationRefusal(code,reason);
    string Find(string endpoint,string staging,string relative)
    {
        foreach(var folder in new[]{staging,endpoint})
        {
            var path=Path.Combine(folder,relative);
            try {File.GetAttributes(path);files.NoLinks(path,root);return path;}
            catch(FileNotFoundException){}catch(DirectoryNotFoundException){}
        }
        throw new PublicationRefusal("invalid","A referenced immutable member was not uploaded");
    }
    sealed record Snapshot(JsonElement Document,string Key,byte[] Bytes,byte[] Signature);
    async Task<Snapshot> ReadSnapshot(string endpoint,string staging,string digest,string scratch,CancellationToken ct)
    {
        var relative="snapshots/"+digest+'/';
        var bytes=await files.ReadBounded(Find(endpoint,staging,relative+"channel.json"),MetadataMax,root,ct);
        var signature=await files.ReadBounded(Find(endpoint,staging,relative+"channel.signature.json"),16384,root,ct);
        if(CommonJson.Digest(bytes)!=digest)Refuse("invalid","Snapshot digest differs from its name");
        var doc=Path.Combine(scratch,Guid.NewGuid().ToString("N")+".json");var sig=doc+".sig";
        await File.WriteAllBytesAsync(doc,bytes,ct);await File.WriteAllBytesAsync(sig,signature,ct);
        var verified=await engine.Verify("channel",doc,sig,ct);
        if(verified.GetProperty("sha256").GetString()!=digest || verified.GetProperty("domain").GetString()!="channel")Refuse("invalid","Verifier returned a different snapshot identity");
        using var parsed=JsonDocument.Parse(bytes);
        return new(parsed.RootElement.Clone(),verified.GetProperty("public_key").GetString()!,bytes,signature);
    }
    static JsonElement[] Entries(Snapshot snapshot)=>snapshot.Document.GetProperty("body").GetProperty("entries").EnumerateArray().ToArray();
    static string Identity(JsonElement entry)=>CommonJson.Identity(entry.GetProperty("descriptor"));
    static JsonElement Body(JsonElement entry)=>entry.GetProperty("descriptor").GetProperty("body");
    static JsonElement[] Added(JsonElement[] previous,JsonElement[] proposed)
    {
        var old=previous.ToDictionary(Identity,StringComparer.Ordinal);var fresh=proposed.ToDictionary(Identity,StringComparer.Ordinal);
        foreach(var (digest,entry) in old)
        {
            if(!fresh.TryGetValue(digest,out var candidate) || new[]{"archive","descriptor","signature"}.Any(key=>!JsonElement.DeepEquals(entry.GetProperty(key),candidate.GetProperty(key))))
                Refuse("conflict","Published builds cannot be removed or replaced");
            var claims=candidate.TryGetProperty("attestations",out var latest)?latest.EnumerateArray().ToArray():[];
            if(entry.TryGetProperty("attestations",out var prior) && prior.EnumerateArray().Any(claim=>!claims.Any(next=>JsonElement.DeepEquals(claim,next))))
                Refuse("conflict","Published qualification evidence cannot be removed");
        }
        var keys=new Dictionary<(string,string),string>();
        foreach(var entry in previous.Concat(proposed))
        {
            var body=Body(entry);var author=(body.GetProperty("authority").GetString()!,body.GetProperty("name").GetString()!);
            var key=entry.GetProperty("signature").GetProperty("public_key").GetString()!;
            if(keys.TryGetValue(author,out var first) && first!=key)Refuse("conflict","Author key changes require an explicit key transition");
            keys[author]=key;
        }
        return fresh.Where(pair=>!old.ContainsKey(pair.Key)).Select(pair=>pair.Value).ToArray();
    }
    static (string Digest,long Size) Archive(JsonElement entry)
    {
        var archive=entry.GetProperty("archive");
        if(!long.TryParse(archive.GetProperty("size").GetString(),NumberStyles.None,CultureInfo.InvariantCulture,out var size) || size<0 || size>uint.MaxValue)
            throw new PublicationRefusal("invalid","Archive size exceeds PKG/1 envelope bound");
        return (archive.GetProperty("sha256").GetString()!,size);
    }
    async Task ValidateArchive(JsonElement entry,string source,string sourceRoot,string scratch,CancellationToken ct)
    {
        if(await files.Hash(source,sourceRoot,ct)!=Archive(entry))Refuse("invalid","Archive bytes differ from signed reference");
        var body=Body(entry);
        var policy=new {format="apkg-json-1",requires=Array.Empty<string>(),schema="1",type="policy",body=new {authorities=new[]{new{
            id=body.GetProperty("authority").GetString(),keys=new[]{new{algorithm="ed25519",public_key=entry.GetProperty("signature").GetProperty("public_key").GetString()}}}}}};
        var folder=Path.Combine(scratch,Guid.NewGuid().ToString("N"));Directory.CreateDirectory(folder);
        var seed=Path.Combine(folder,"policy.json");var context=Path.Combine(folder,"root");
        await File.WriteAllBytesAsync(seed,CommonJson.Encode(policy),ct);
        await engine.RunText(options.ApkgPath,["init",context,body.GetProperty("target").GetString()!,seed,"--yes"],ct);
        var inspected=await engine.Run(options.ApkgPath,["archive-check",context,source],ct,outputLimit:16*1024*1024);
        if(!JsonElement.DeepEquals(inspected.GetProperty("descriptor"),entry.GetProperty("descriptor")) || !JsonElement.DeepEquals(inspected.GetProperty("signature"),entry.GetProperty("signature")))
            Refuse("invalid","Archive contains a different signed package");
    }
    public async Task<JsonElement> Commit(Publisher publisher,string channel,string staging,CommonCommitRequest request,string scratch,bool allowLoopback,CancellationToken ct)
    {
        if(!Portal.Channels.ChannelPaths.IsChannelName(channel) || publisher.Identity.Length is 0 or >256 || publisher.Identity.Any(char.IsControl))
            Refuse("invalid","Invalid publisher or channel identity");
        var endpoint=Path.Combine(options.ChannelsDir,channel);
        files.NoLinks(staging,root);
        files.DirectoryReady(endpoint,root);
        using var writer=await files.Lock(endpoint,root,ct);
        var before=await files.Current(endpoint,root,ct);
        if(before!=request.Snapshot && before!=request.ExpectedCurrent)Refuse("conflict","Current changed; rebuild the signed snapshot before retrying");
        var proposed=await ReadSnapshot(endpoint,staging,request.Snapshot!,scratch,ct);
        var channelId=proposed.Document.GetProperty("body").GetProperty("id").GetString();
        var owner=new {format="apkg-portal-owner-1",publisher=publisher.Identity,channel_id=channelId,public_key=proposed.Key};
        var ownerFile=Path.Combine(endpoint,".portal-owner.json");
        if(File.Exists(ownerFile))
        {
            var captured=await files.ReadBounded(ownerFile,4096,root,ct);
            using var existing=JsonDocument.Parse(captured);
            if(!JsonElement.DeepEquals(existing.RootElement,JsonSerializer.SerializeToElement(owner)))Refuse("owner","Publisher or channel differs from its registered owner");
        }
        else if(before is not null)Refuse("conflict","An unregistered channel cannot be adopted by publication");
        JsonElement[] previous=[];
        var sequence=ulong.Parse(proposed.Document.GetProperty("body").GetProperty("sequence").GetString()!,CultureInfo.InvariantCulture);
        if(before is not null)
        {
            var current=await ReadSnapshot(endpoint,endpoint,before,scratch,ct);
            if(current.Key!=proposed.Key || current.Document.GetProperty("body").GetProperty("id").GetString()!=channelId)Refuse("conflict","Channel identity or signing key changed");
            var prior=ulong.Parse(current.Document.GetProperty("body").GetProperty("sequence").GetString()!,CultureInfo.InvariantCulture);
            if(before!=request.Snapshot && (prior==ulong.MaxValue || sequence!=prior+1))Refuse("conflict","Channel sequence must advance exactly once");
            previous=Entries(current);
        }
        else if(sequence!=1)Refuse("conflict","A new channel must begin at sequence one");
        var entries=Entries(proposed);var added=Added(previous,entries);
        var links=request.Links??[];
        if(links.Count>32)Refuse("invalid","At most 32 archive links may be submitted per commit");
        var groups=entries.Select(Archive).GroupBy(value=>value.Digest).ToArray();
        if(groups.Any(group=>group.Select(value=>value.Size).Distinct().Count()!=1))Refuse("invalid","Conflicting archive size references");
        var references=groups.ToDictionary(group=>group.Key,group=>group.First(),StringComparer.Ordinal);
        if(links.Any(pair=>!references.ContainsKey(pair.Key)||pair.Value is null))Refuse("invalid","Links must name an exact archive in the signed snapshot");
        var incoming=added.Select(Archive).Distinct().ToArray();
        long total=0;
        foreach(var archive in incoming.Where(a=>links.ContainsKey(a.Digest)))
        {
            if(archive.Size>options.MaxCommonLinkBytes || archive.Size>options.MaxCommonValidationBytes-total)Refuse("invalid","Linked archive validation exceeds portal byte limits");
            total+=archive.Size;
        }
        if((!publisher.Files || !options.Policy.BinariesAllowed) && incoming.Any(a=>!links.ContainsKey(a.Digest)))Refuse("owner","This publisher must provide an external link for every new archive");
        var sources=new Dictionary<string,(string Path,string Root)>();
        var downloader=new CommonArchiveLinks();
        foreach(var archive in incoming)
        {
            if(links.TryGetValue(archive.Digest,out var url))
            {
                var path=Path.Combine(scratch,archive.Digest+".pkg");
                await downloader.Download(url,path,archive,options.Policy.Hosts,allowLoopback,ct);sources[archive.Digest]=(path,scratch);
            }
            else sources[archive.Digest]=(Find(endpoint,staging,"archives/"+archive.Digest+".pkg"),root);
        }
        foreach(var entry in added)
        {var source=sources[Archive(entry).Digest];await ValidateArchive(entry,source.Path,source.Root,scratch,ct);}
        files.DirectoryReady(Path.Combine(endpoint,"archives"),root);
        files.DirectoryReady(Path.Combine(endpoint,"snapshots"),root);
        foreach(var archive in incoming)
        {
            var destination=Path.Combine(endpoint,"archives",archive.Digest+".pkg");
            if(links.TryGetValue(archive.Digest,out var url))await files.ImmutableBytes(destination+".url",Encoding.UTF8.GetBytes(url),root,ct);
            else {var source=sources[archive.Digest];await files.ImmutableArchive(destination,source.Path,root,source.Root,archive,ct);}
        }
        var snapshot=Path.Combine(endpoint,"snapshots",request.Snapshot!);files.DirectoryReady(snapshot,root);
        await files.ImmutableBytes(Path.Combine(snapshot,"channel.json"),proposed.Bytes,root,ct);
        await files.ImmutableBytes(Path.Combine(snapshot,"channel.signature.json"),proposed.Signature,root,ct);
        await files.ImmutableBytes(ownerFile,CommonJson.Encode(owner),root,ct);
        await files.ReplaceCurrent(endpoint,before,request.Snapshot!,root,ct);
        return JsonSerializer.SerializeToElement(new{result=before==request.Snapshot?"unchanged":"published",sequence=sequence.ToString(CultureInfo.InvariantCulture),added=added.Length});
    }
}
