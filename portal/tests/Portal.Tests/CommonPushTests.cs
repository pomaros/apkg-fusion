// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.AspNetCore.Mvc.Testing;
using Microsoft.Extensions.DependencyInjection;
using Portal.Push;

namespace Portal.Tests;

public sealed class CommonPushTests
{
    internal sealed class Site : WebApplicationFactory<Program>
    {
        public readonly CommonChannelTests.Fixture Source;
        public bool Loopback;
        public readonly string Data = Directory.CreateTempSubdirectory("portal-common-push-").FullName;
        public readonly string Key, OtherKey, LinkKey;
        readonly string configuration;
        public readonly Dictionary<string,string> Settings = [];
        public string Live => Path.Combine(Data, "channels", "common");
        public Site(bool rich = false)
        {
            Source = new(rich);
            var a = PublisherKeys.Create("first", "common", files: true);
            var b = PublisherKeys.Create("second", "common", files: true);
            var c = PublisherKeys.Create("link", "common");
            (Key, OtherKey, LinkKey) = (a.Key, b.Key, c.Key);
            configuration = string.Join(';', a.Config, b.Config, c.Config);
        }
        protected override void ConfigureWebHost(Microsoft.AspNetCore.Hosting.IWebHostBuilder builder)
        {
            builder.UseSetting("Portal:DataDir", Data);
            builder.UseSetting("Portal:ApkgPath", Source.Engine);
            builder.UseSetting("Portal:Keys", configuration);
            builder.UseSetting("Portal:AllowLoopbackHttpPush", Loopback.ToString());
            foreach(var setting in Settings)builder.UseSetting(setting.Key,setting.Value);
        }
        public HttpClient Client(string? key = null)
        {
            var client = CreateClient(new WebApplicationFactoryClientOptions { BaseAddress = new Uri("https://localhost") });
            client.DefaultRequestHeaders.Authorization = new("Bearer", key ?? Key);
            return client;
        }
        public string Snapshot(int sequence) => Directory.GetDirectories(Path.Combine(Source.Channel, "snapshots"))
            .Single(path => JsonDocument.Parse(File.ReadAllBytes(Path.Combine(path, "channel.json"))).RootElement
                .GetProperty("body").GetProperty("sequence").GetString() == sequence.ToString()).Split(Path.DirectorySeparatorChar).Last();
        public Dictionary<string, byte[]> Files(string snapshot)
        {
            var result = new Dictionary<string, byte[]>();
            foreach (var name in new[] { "channel.json", "channel.signature.json" })
            {
                var relative = $"snapshots/{snapshot}/{name}";
                result[relative] = File.ReadAllBytes(Path.Combine(Source.Channel, relative));
            }
            using var doc = JsonDocument.Parse(result[$"snapshots/{snapshot}/channel.json"]);
            foreach (var entry in doc.RootElement.GetProperty("body").GetProperty("entries").EnumerateArray())
            {
                var relative = "archives/" + entry.GetProperty("archive").GetProperty("sha256").GetString() + ".pkg";
                result[relative] = File.ReadAllBytes(Path.Combine(Source.Channel, relative));
            }
            return result;
        }
        public string SignSnapshot(JsonNode document, string? key = null)
        {
            var path = Path.Combine(Source.Data, "proposed.json");
            File.WriteAllText(path, document.ToJsonString());
            CommonChannelTests.Fixture.Run("python3", "-c",
                "import json,sys; p=sys.argv[1]; v=json.load(open(p)); open(p,'w').write(json.dumps(v,sort_keys=True,ensure_ascii=False,separators=(',',':')))", path);
            var bytes = File.ReadAllBytes(path);
            var digest = Convert.ToHexStringLower(SHA256.HashData(bytes));
            var folder = Path.Combine(Source.Channel, "snapshots", digest);
            Directory.CreateDirectory(folder);
            File.WriteAllBytes(Path.Combine(folder, "channel.json"), bytes);
            File.WriteAllText(Path.Combine(folder, "channel.signature.json"),
                CommonChannelTests.Fixture.Run(Source.Engine, "sign-object", key ?? Path.Combine(Source.Data, "channel-key"), "channel", path).Trim());
            return digest;
        }
        public async Task Upload(HttpClient client, Dictionary<string, byte[]> files, bool parts = false)
        {
            var plan = string.Concat(files.Select(f => $"{f.Key} {Convert.ToHexStringLower(SHA256.HashData(f.Value))} {f.Value.Length}\n"));
            var planned = await client.PostAsync("/common/_push/plan", new StringContent(plan));
            var text = await planned.Content.ReadAsStringAsync();
            Assert.Equal(HttpStatusCode.OK, planned.StatusCode);
            Assert.DoesNotContain("refused:", text);
            foreach (var file in files.Where(file => text.Split('\n').Contains("need: " + file.Key)))
            {
                var split = parts ? file.Value.Length / 2 : file.Value.Length;
                var offset = 0;
                while (offset < file.Value.Length)
                {
                    var count = Math.Min(Math.Max(split, 1), file.Value.Length - offset);
                    using var content = new ByteArrayContent(file.Value, offset, count);
                    if (parts) content.Headers.ContentRange = new ContentRangeHeaderValue(offset, offset + count - 1, file.Value.Length);
                    var sent = await client.PutAsync("/common/_push/files/" + file.Key, content);
                    Assert.True(sent.IsSuccessStatusCode, await sent.Content.ReadAsStringAsync());
                    offset += count;
                }
            }
        }
        public Task<HttpResponseMessage> Commit(HttpClient client, string snapshot, string? expected = null) =>
            client.PostAsJsonAsync("/common/_push/common-commit", new { snapshot, expectedCurrent = expected });
        protected override void Dispose(bool disposing)
        {
            base.Dispose(disposing);
            if (disposing)
            {
                Source.Dispose();
                if (Directory.Exists(Data)) Directory.Delete(Data, true);
            }
        }
    }

    [Fact]
    public async Task Authenticated_resumable_publication_is_atomic_append_only_and_idempotent()
    {
        using var site = new Site();
        using var client = site.Client();
        var first = site.Snapshot(1);
        await site.Upload(client, site.Files(first), parts: true);
        Assert.False(File.Exists(Path.Combine(site.Live, "current")));
        Assert.Equal(HttpStatusCode.OK, (await site.Commit(client, first)).StatusCode);
        Assert.Equal(first + "\n", File.ReadAllText(Path.Combine(site.Live, "current")));
        var second = site.Snapshot(2);
        await site.Upload(client, site.Files(second));
        Assert.Equal(HttpStatusCode.Conflict, (await site.Commit(client, second)).StatusCode);
        Assert.Equal(first + "\n", File.ReadAllText(Path.Combine(site.Live, "current")));
        var published = await site.Commit(client, second, first);
        Assert.Equal(HttpStatusCode.OK, published.StatusCode);
        Assert.Contains("review: not-granted", await published.Content.ReadAsStringAsync());
        var retry = await site.Commit(client, second, first);
        Assert.Equal(HttpStatusCode.OK, retry.StatusCode);
        Assert.Contains("result: unchanged", await retry.Content.ReadAsStringAsync());
        Assert.Equal(HttpStatusCode.Conflict, (await site.Commit(client, first, second)).StatusCode);
        Assert.Equal(second + "\n", File.ReadAllText(Path.Combine(site.Live, "current")));
        using var observation = JsonDocument.Parse(await client.GetStringAsync("/api/channels/common"));
        Assert.Equal(2, observation.RootElement.GetProperty("document").GetProperty("body").GetProperty("entries").GetArrayLength());
        foreach (var file in site.Files(second))
            Assert.Equal(file.Value, await client.GetByteArrayAsync("/common/" + file.Key));
        Assert.Equal(HttpStatusCode.NotFound, (await client.GetAsync("/common/.portal-owner.json")).StatusCode);
    }

    [Theory]
    [InlineData("corrupt")]
    [InlineData("missing")]
    [InlineData("wrong-signed-package")]
    [InlineData("content")]
    public async Task Archive_refusals_leave_no_visible_head_or_owner(string failure)
    {
        using var site = new Site();
        using var client = site.Client();
        var snapshot = site.Snapshot(1);
        var files = site.Files(snapshot);
        var archive = files.Keys.Single(k => k.StartsWith("archives/", StringComparison.Ordinal));
        if (failure == "missing") files.Remove(archive);
        // Correct outer signature and archive hash are insufficient: embedded package identity must match.
        if (failure is "wrong-signed-package" or "content")
        {
            var other = failure == "content" ? files[archive].ToArray() : File.ReadAllBytes(Path.Combine(site.Source.Data, "archive2.pkg"));
            if (failure == "content") other[^1] ^= 1;
            var digest = Convert.ToHexStringLower(SHA256.HashData(other));
            var channel = JsonNode.Parse(files[$"snapshots/{snapshot}/channel.json"])!;
            var reference = channel["body"]!["entries"]![0]!["archive"]!;
            reference["sha256"] = digest;
            reference["size"] = other.Length.ToString();
            snapshot = site.SignSnapshot(channel);
            files = new()
            {
                [$"snapshots/{snapshot}/channel.json"] = File.ReadAllBytes(Path.Combine(site.Source.Channel, "snapshots", snapshot, "channel.json")),
                [$"snapshots/{snapshot}/channel.signature.json"] = File.ReadAllBytes(Path.Combine(site.Source.Channel, "snapshots", snapshot, "channel.signature.json")),
                [$"archives/{digest}.pkg"] = other,
            };
        }
        await site.Upload(client, files);
        if(failure=="corrupt")
        {
            var staged=Directory.GetFiles(Path.Combine(site.Data,"staging"),"*.pkg",SearchOption.AllDirectories).Single();
            var bytes=File.ReadAllBytes(staged);bytes[^1]^=1;File.WriteAllBytes(staged,bytes);
        }
        var response = await site.Commit(client, snapshot);
        Assert.Equal(HttpStatusCode.UnprocessableEntity, response.StatusCode);
        Assert.False(File.Exists(Path.Combine(site.Live, "current")));
        Assert.False(File.Exists(Path.Combine(site.Live, ".portal-owner.json")));
    }

    [Fact]
    public async Task Publisher_rights_and_registered_channel_owner_cannot_be_bypassed()
    {
        using var site = new Site();
        using var client = site.Client();
        var first = site.Snapshot(1);
        await site.Upload(client, site.Files(first));
        Assert.Equal(HttpStatusCode.OK, (await site.Commit(client, first)).StatusCode);
        using var other = site.Client(site.OtherKey);
        Assert.Equal(HttpStatusCode.Forbidden, (await site.Commit(other, first)).StatusCode);
        var service = site.Services.GetRequiredService<PushService>();
        var request = new CommonCommitRequest { Snapshot = first, ExpectedCurrent = null };
        var renamed = new Publisher("Renamed display", new HashSet<string> { "common" }, true, "configured:first");
        Assert.Equal(200, (await service.CommitCommon(renamed, "common", request, CancellationToken.None)).Status);
        var sameDisplayOtherAccount = new Publisher("first", new HashSet<string> { "common" }, true, "github:8");
        Assert.Equal(403, (await service.CommitCommon(sameDisplayOtherAccount, "common", request, CancellationToken.None)).Status);
        using var anonymous = site.Client("unknown");
        Assert.Equal(HttpStatusCode.Unauthorized, (await site.Commit(anonymous, first)).StatusCode);
        using var link = site.Client(site.LinkKey);
        var archive = site.Files(first).Single(f => f.Key.StartsWith("archives/", StringComparison.Ordinal));
        var plan = await link.PostAsync("/common/_push/plan", new StringContent($"{archive.Key} {Convert.ToHexStringLower(SHA256.HashData(archive.Value))} {archive.Value.Length}\n"));
        Assert.Contains("refused:", await plan.Content.ReadAsStringAsync());
        Assert.Equal(first + "\n", File.ReadAllText(Path.Combine(site.Live, "current")));
    }

    [Fact]
    public async Task Unsupported_commit_route_cannot_publish_metadata_or_current()
    {
        using var site = new Site();
        using var client = site.Client();
        var first = site.Snapshot(1);
        await site.Upload(client, site.Files(first));
        await client.PostAsync("/common/_push/commit", new StringContent(""));
        Assert.False(File.Exists(Path.Combine(site.Live, "current")));
        Assert.False(Directory.Exists(Path.Combine(site.Live, "snapshots")));
        var plan = await client.PostAsync("/common/_push/plan", new StringContent($"current {new string('a', 64)} 65\n"));
        Assert.Contains("refused:", await plan.Content.ReadAsStringAsync());
    }

    [Theory]
    [InlineData("remove-build")]
    [InlineData("sequence")]
    [InlineData("channel-identity")]
    [InlineData("channel-key")]
    public async Task Validly_signed_history_rewrites_are_refused(string failure)
    {
        using var site = new Site();
        using var client = site.Client();
        var first = site.Snapshot(1);
        await site.Upload(client, site.Files(first));
        Assert.Equal(HttpStatusCode.OK, (await site.Commit(client, first)).StatusCode);
        var document = JsonNode.Parse(File.ReadAllBytes(Path.Combine(site.Source.Channel, "snapshots", first, "channel.json")))!;
        document["body"]!["sequence"] = failure == "sequence" ? "3" : "2";
        if (failure == "remove-build") document["body"]!["entries"] = new JsonArray();
        if (failure == "channel-identity") document["body"]!["id"] = new string('e', 32);
        string? key = null;
        if (failure == "channel-key")
        {
            key = Path.Combine(site.Source.Data, "other-channel-key");
            CommonChannelTests.Fixture.Run(site.Source.Engine, "keygen", key);
        }
        var proposed = site.SignSnapshot(document, key);
        await site.Upload(client, site.Files(proposed));
        var refused = await site.Commit(client, proposed, first);
        Assert.Equal(failure is "channel-identity" or "channel-key" ? HttpStatusCode.Forbidden : HttpStatusCode.Conflict, refused.StatusCode);
        Assert.Equal(first + "\n", File.ReadAllText(Path.Combine(site.Live, "current")));
    }

    [Theory]
    [InlineData("{}")]
    [InlineData("null")]
    [InlineData("{\"snapshot\":\"bad\",\"expectedCurrent\":null}")]
    [InlineData("{\"snapshot\":null,\"expectedCurrent\":null,\"review\":true}")]
    [InlineData("{\"snapshot\":null,\"snapshot\":null,\"expectedCurrent\":null}")]
    [InlineData("{\"Snapshot\":null,\"expectedCurrent\":null}")]
    public async Task Malformed_common_requests_refuse_without_publication(string body)
    {
        using var site = new Site();
        using var client = site.Client();
        var response = await client.PostAsync("/common/_push/common-commit", new StringContent(body, Encoding.UTF8, "application/json"));
        Assert.Equal(HttpStatusCode.BadRequest, response.StatusCode);
        Assert.False(File.Exists(Path.Combine(site.Live, "current")));
    }
}
