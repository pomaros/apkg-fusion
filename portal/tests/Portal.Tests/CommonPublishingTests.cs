// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
using System.Net;
using System.Net.Http.Json;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.Extensions.DependencyInjection;
using Portal.Accounts;

namespace Portal.Tests;

public sealed class CommonPublishingTests
{
    static string Public(CommonPushTests.Site site) => JsonDocument.Parse(CommonChannelTests.Fixture.Run(
        site.Source.Engine,"key-public",Path.Combine(site.Source.Data,"channel-key"))).RootElement.GetProperty("public_key").GetString()!;
    static string RunPush(CommonPushTests.Site site,string endpoint,string? links=null,bool stageOnly=false)
    {
        var arguments=new List<string> { Path.Combine(site.Source.Repository,"tools","apkg-portal-push"),
            site.Source.Channel,endpoint,"--engine",site.Source.Engine,"--sign",Path.Combine(site.Source.Data,"channel-key"),"--part-bytes","1024" };
        if(links is not null)arguments.AddRange(["--links",links]);
        if(stageOnly)arguments.Add("--stage-only");
        return CommonChannelTests.Fixture.Run("python3",arguments.ToArray());
    }

    [Fact]
    public async Task Registered_publisher_uses_common_signed_auth_and_resumable_client_without_a_pkg_backend()
    {
        using var site=new CommonPushTests.Site();site.UseKestrel(0);
        using var client=site.CreateClient();
        var registry=site.Services.GetRequiredService<Registry>();
        Assert.Null(registry.Register(7,"author","Test author",Public(site),"common"));
        Assert.True(registry.Change(7,record=>record with { Files=true }));
        var first=site.Snapshot(1);File.WriteAllText(Path.Combine(site.Source.Channel,"current"),first+"\n");
        var endpoint=new Uri(client.BaseAddress!,"/common").AbsoluteUri;
        using var pushed=JsonDocument.Parse(RunPush(site,endpoint));
        Assert.Equal("published",pushed.RootElement.GetProperty("result").GetString());
        Assert.Equal(first+"\n",File.ReadAllText(Path.Combine(site.Live,"current")));
        using var retry=JsonDocument.Parse(RunPush(site,endpoint));
        Assert.Equal("unchanged",retry.RootElement.GetProperty("result").GetString());
        Assert.Equal(0,retry.RootElement.GetProperty("files_sent").GetInt32());
        Assert.Equal("github:7",JsonDocument.Parse(File.ReadAllText(Path.Combine(site.Live,".portal-owner.json"))).RootElement.GetProperty("publisher").GetString());
        Assert.Equal(HttpStatusCode.OK,(await client.GetAsync("/channels/common")).StatusCode);
        Assert.DoesNotContain("/channels/common",await client.GetStringAsync("/")); // self-service channels start unlisted
    }

    [Fact]
    public async Task Link_only_registration_can_publish_advance_retry_and_install_external_archives_without_storing_them()
    {
        using var site=new CommonPushTests.Site(rich:true) { Loopback=true };site.UseKestrel(0);site.Source.UseKestrel(0);
        using var client=site.CreateClient();using var external=site.Source.CreateClient();
        var registry=site.Services.GetRequiredService<Registry>();
        Assert.Null(registry.Register(7,"author","Test author",Public(site),"common"));
        Assert.False(registry.ByAccount(7)!.Files);
        var first=site.Snapshot(1);var second=site.Snapshot(2);
        var endpoint=new Uri(client.BaseAddress!,"/common").AbsoluteUri;
        var linksFile=Path.Combine(site.Source.Data,"links.json");
        foreach(var snapshot in new[]{first,second})
        {
            File.WriteAllText(Path.Combine(site.Source.Channel,"current"),snapshot+"\n");
            var links=site.Files(snapshot).Keys.Where(path=>path.StartsWith("archives/",StringComparison.Ordinal))
                .ToDictionary(path=>path[9..73],path=>new Uri(external.BaseAddress!,"/common/"+path).AbsoluteUri);
            File.WriteAllText(linksFile,JsonSerializer.Serialize(links));
            using var pushed=JsonDocument.Parse(RunPush(site,endpoint,linksFile));
            Assert.Equal("published",pushed.RootElement.GetProperty("result").GetString());
            using var retry=JsonDocument.Parse(RunPush(site,endpoint,linksFile));
            Assert.Equal("unchanged",retry.RootElement.GetProperty("result").GetString());
        }
        Assert.Empty(Directory.GetFiles(Path.Combine(site.Live,"archives"),"*.pkg"));
        Assert.Equal(2,Directory.GetFiles(Path.Combine(site.Live,"archives"),"*.url").Length);
        using var catalogue=JsonDocument.Parse(await client.GetStringAsync("/api/channels/common/packages?authority="+new string('1',32)));
        var entry=catalogue.RootElement.GetProperty("builds")[0].GetProperty("build");
        var temporary=Directory.CreateTempSubdirectory("portal-linked-cycle-");
        try
        {
            var script=Path.Combine(site.Source.Repository,"portal","tests","common_http_lifecycle.py");
            var engine=site.Source.Engine;
            var output=CommonChannelTests.Fixture.Run("python3",script,"install",engine,temporary.FullName,
                site.Source.Data,endpoint,second,entry.GetProperty("digest").GetString()!);
            Assert.Contains("actual PkgManager worker",output);
            site.Dispose();
            output=CommonChannelTests.Fixture.Run("python3",script,"remove",engine,temporary.FullName);
            Assert.Contains("Offline removal passed",output);
        }
        finally { temporary.Delete(recursive:true); }
    }

    static async Task<HttpRequestMessage> Sign(CommonPushTests.Site site,HttpClient client,string path,byte[] body,string session,long sequence)
    {
        var digest=Convert.ToHexStringLower(SHA256.HashData(body));
        var text=$"pkg-push-1\nPOST\n{path}\n{session}\n{sequence}\n{digest}\n-\n";
        var file=Path.Combine(site.Source.Data,"request-"+Guid.NewGuid().ToString("N"));File.WriteAllText(file,text);
        using var signature=JsonDocument.Parse(CommonChannelTests.Fixture.Run(site.Source.Engine,"push-sign",Path.Combine(site.Source.Data,"channel-key"),file));
        var request=new HttpRequestMessage(HttpMethod.Post,path){Content=new ByteArrayContent(body)};
        request.Headers.TryAddWithoutValidation("Authorization",$"Pkg-Signature key={Public(site)},session={session},seq={sequence},sha256={digest},sig={signature.RootElement.GetProperty("signature").GetString()}");
        await Task.CompletedTask;return request;
    }

    [Fact]
    public async Task Revoking_binary_storage_after_staging_is_enforced_again_at_signed_commit()
    {
        using var site=new CommonPushTests.Site();site.UseKestrel(0);using var client=site.CreateClient();
        var registry=site.Services.GetRequiredService<Registry>();
        Assert.Null(registry.Register(7,"author","Test author",Public(site),"common"));
        registry.Change(7,record=>record with { Files=true });
        var first=site.Snapshot(1);File.WriteAllText(Path.Combine(site.Source.Channel,"current"),first+"\n");
        var endpoint=new Uri(client.BaseAddress!,"/common").AbsoluteUri;
        using var staged=JsonDocument.Parse(RunPush(site,endpoint,stageOnly:true));
        Assert.Equal("staged",staged.RootElement.GetProperty("result").GetString());
        Assert.False(File.Exists(Path.Combine(site.Live,"current")));
        registry.Change(7,record=>record with { Files=false });
        var opened=await client.PostAsync("/common/_push/session",new StringContent("key: "+Public(site)+"\n"));
        var session=(await opened.Content.ReadAsStringAsync()).Split('\n').Single(line=>line.StartsWith("session: ",StringComparison.Ordinal))[9..];
        var body=JsonSerializer.SerializeToUtf8Bytes(new { snapshot=first,expectedCurrent=(string?)null });
        using var request=await Sign(site,client,"/common/_push/common-commit",body,session,1);
        var refused=await client.SendAsync(request);
        Assert.Equal(HttpStatusCode.Forbidden,refused.StatusCode);
        Assert.Contains("external link",await refused.Content.ReadAsStringAsync());
        Assert.False(File.Exists(Path.Combine(site.Live,"current")));
        Assert.False(File.Exists(Path.Combine(site.Live,".portal-owner.json")));
    }

    [Fact]
    public async Task Signed_requests_reject_replay_body_tampering_and_out_of_scope_channels()
    {
        using var site=new CommonPushTests.Site();site.UseKestrel(0);using var client=site.CreateClient();
        Assert.Null(site.Services.GetRequiredService<Registry>().Register(7,"author","Test author",Public(site),"common"));
        var opened=await client.PostAsync("/common/_push/session",new StringContent("key: "+Public(site)+"\n"));
        var session=(await opened.Content.ReadAsStringAsync()).Split('\n').Single(line=>line.StartsWith("session: ",StringComparison.Ordinal))[9..];
        using var good=await Sign(site,client,"/common/_push/plan",[],session,1);
        Assert.Equal(HttpStatusCode.OK,(await client.SendAsync(good)).StatusCode);
        using var replay=await Sign(site,client,"/common/_push/plan",[],session,1);
        Assert.Equal(HttpStatusCode.Unauthorized,(await client.SendAsync(replay)).StatusCode);
        using var changed=await Sign(site,client,"/common/_push/plan",[],session,2);
        changed.Content=new ByteArrayContent([1]);
        Assert.Equal(HttpStatusCode.Unauthorized,(await client.SendAsync(changed)).StatusCode);
        using var foreign=await Sign(site,client,"/other/_push/plan",[],session,3);
        Assert.Equal(HttpStatusCode.Forbidden,(await client.SendAsync(foreign)).StatusCode);
        Assert.False(File.Exists(Path.Combine(site.Live,"current")));
    }
}
