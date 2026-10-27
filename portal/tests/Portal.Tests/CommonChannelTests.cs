// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Diagnostics;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Microsoft.AspNetCore.Mvc.Testing;
using Portal.Channels;

namespace Portal.Tests;

public sealed class CommonChannelTests
{
    sealed class Fixture : WebApplicationFactory<Program>
    {
        public readonly string Data = Directory.CreateTempSubdirectory("portal-common-").FullName;
        public readonly string Repository;
        public string Engine;
        public string Channel => Path.Combine(Data, "channels", "common");
        public string Head => File.ReadAllText(Path.Combine(Channel, "current")).TrimEnd('\n');
        public string Snapshot => Path.Combine(Channel, "snapshots", Head);

        public Fixture()
        {
            var dir = new DirectoryInfo(AppContext.BaseDirectory);
            while (dir is not null && !File.Exists(Path.Combine(dir.FullName, "tools", "apkg-channel"))) dir = dir.Parent;
            Repository = dir?.FullName ?? throw new InvalidOperationException("repository not found");
            Engine = Path.Combine(Repository, "build", "apkg-core");
            // Required dependency: fail rather than silently skipping the common acceptance tests.
            Assert.True(File.Exists(Engine), "build common verifier with make build/apkg-core");
            Run("python3", Path.Combine(Repository, "portal", "tests", "common_fixture.py"), Repository, Data, Engine);
        }
        public static string Run(string executable, params string[] args)
        {
            var start = new ProcessStartInfo(executable) { RedirectStandardOutput = true, RedirectStandardError = true };
            foreach (var arg in args) start.ArgumentList.Add(arg);
            using var process = Process.Start(start)!;
            var output = process.StandardOutput.ReadToEndAsync();
            var error = process.StandardError.ReadToEndAsync();
            if (!process.WaitForExit(90000))
            {
                process.Kill(entireProcessTree: true);
                process.WaitForExit();
                Assert.Fail("fixture process timed out");
            }
            Assert.True(process.ExitCode == 0, error.GetAwaiter().GetResult());
            return output.GetAwaiter().GetResult();
        }
        protected override void ConfigureWebHost(Microsoft.AspNetCore.Hosting.IWebHostBuilder builder)
        {
            builder.UseSetting("Portal:DataDir", Data);
            builder.UseSetting("Portal:ApkgPath", Engine);
            builder.UseSetting("Portal:PkgPath", Path.Combine(Data, "legacy-must-not-run"));
        }
        public void Rewrite(JsonNode document)
        {
            var bytes = Encoding.UTF8.GetBytes(document.ToJsonString(new() { WriteIndented = false }));
            // Common contracts require canonical property order: use the same canonical encoder as publishing.
            var path = Path.Combine(Data, "rewrite.json");
            File.WriteAllBytes(path, bytes);
            Run("python3", "-c", "import json,sys; p=sys.argv[1]; v=json.load(open(p)); open(p,'w').write(json.dumps(v,ensure_ascii=False,sort_keys=True,separators=(',',':')))", path);
            bytes = File.ReadAllBytes(path);
            var digest = Convert.ToHexStringLower(SHA256.HashData(bytes));
            var destination = Path.Combine(Channel, "snapshots", digest);
            Directory.CreateDirectory(destination);
            File.WriteAllBytes(Path.Combine(destination, "channel.json"), bytes);
            var signature = File.ReadAllText(Path.Combine(Snapshot, "channel.signature.json"));
            File.WriteAllText(Path.Combine(destination, "channel.signature.json"), signature.Trim());
            File.WriteAllText(Path.Combine(Channel, "current"), digest + "\n");
        }
        protected override void Dispose(bool disposing)
        {
            base.Dispose(disposing);
            if (disposing && Directory.Exists(Data)) Directory.Delete(Data, true);
        }
    }

    [Fact]
    public async Task Signed_catalogue_preserves_same_name_authors_and_claims_no_client_trust()
    {
        using var fixture = new Fixture();
        using var client = fixture.CreateClient();
        var before = Directory.GetFiles(fixture.Channel, "*", SearchOption.AllDirectories)
            .ToDictionary(p => p, p => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(p))));
        var response = await client.GetAsync("/api/common/channels/common");
        Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        using var doc = JsonDocument.Parse(await response.Content.ReadAsStringAsync());
        var view = doc.RootElement;
        Assert.Equal("verified", view.GetProperty("signature").GetString());
        foreach (var fact in new[] { "trust", "review", "runtimeTesting" })
            Assert.Equal("not-evaluated", view.GetProperty(fact).GetString());
        Assert.Equal("not-checked", view.GetProperty("payload").GetString());
        Assert.Equal("unknown", view.GetProperty("freshness").GetString());
        var entries = view.GetProperty("document").GetProperty("body").GetProperty("entries");
        Assert.Equal(2, entries.GetArrayLength());
        Assert.Equal(2, entries.EnumerateArray().Select(e => e.GetProperty("descriptor").GetProperty("body").GetProperty("authority").GetString()).Distinct().Count());
        foreach (var entry in entries.EnumerateArray())
        {
            var body = entry.GetProperty("descriptor").GetProperty("body");
            Assert.Equal("same-name", body.GetProperty("name").GetString());
            Assert.Equal("3", body.GetProperty("revision").GetString());
            Assert.Equal("x86_64-aros-v11-native", body.GetProperty("target").GetString());
        }
        foreach (var file in before) Assert.Equal(file.Value, Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(file.Key))));
        Assert.Equal(before.Count, Directory.GetFiles(fixture.Channel, "*", SearchOption.AllDirectories).Length);
        foreach (var relative in new[] { "current", $"snapshots/{fixture.Head}/channel.json", $"snapshots/{fixture.Head}/channel.signature.json" })
        {
            var raw = await client.GetAsync("/common/" + relative);
            Assert.Equal(HttpStatusCode.OK, raw.StatusCode);
            Assert.Equal(File.ReadAllBytes(Path.Combine(fixture.Channel, relative)), await raw.Content.ReadAsByteArrayAsync());
            Assert.Equal(relative == "current" ? "no-cache" : "public, max-age=31536000, immutable", raw.Headers.CacheControl!.ToString());
        }
        var member = Path.Combine(fixture.Snapshot, "channel.json");
        using var range = new HttpRequestMessage(HttpMethod.Get, $"/common/snapshots/{fixture.Head}/channel.json");
        range.Headers.Range = new System.Net.Http.Headers.RangeHeaderValue(0, 15);
        var partial = await client.SendAsync(range);
        Assert.Equal(HttpStatusCode.PartialContent, partial.StatusCode);
        Assert.Equal(File.ReadAllBytes(member)[..16], await partial.Content.ReadAsByteArrayAsync());
    }

    [Theory]
    [InlineData("signature")]
    [InlineData("schema")]
    [InlineData("digest")]
    [InlineData("reference")]
    [InlineData("size")]
    public async Task Invalid_common_metadata_refuses_even_when_a_legacy_index_exists(string failure)
    {
        using var fixture = new Fixture();
        File.WriteAllText(Path.Combine(fixture.Channel, "index"), "legacy 1.0 x86_64 " + new string('a', 64) + "\n");
        var document = JsonNode.Parse(File.ReadAllText(Path.Combine(fixture.Snapshot, "channel.json")))!;
        if (failure == "signature") { document["body"]!["sequence"] = "99"; fixture.Rewrite(document); }
        if (failure == "schema") { document["body"]!["review"] = "approved"; fixture.Rewrite(document); }
        if (failure == "digest") File.AppendAllText(Path.Combine(fixture.Snapshot, "channel.json"), " ");
        if (failure == "reference") File.AppendAllText(Path.Combine(fixture.Channel, "current"), "\n");
        if (failure == "size") File.WriteAllBytes(Path.Combine(fixture.Snapshot, "channel.signature.json"), new byte[16385]);
        using var client = fixture.CreateClient();
        Assert.Equal(HttpStatusCode.UnprocessableEntity, (await client.GetAsync("/api/common/channels/common")).StatusCode);
    }

    [Fact]
    public async Task Missing_engine_is_unavailable_and_missing_common_head_is_not_a_legacy_fallback()
    {
        using var fixture = new Fixture();
        fixture.Engine = Path.Combine(fixture.Data, "missing-engine");
        using var client = fixture.CreateClient();
        Assert.Equal(HttpStatusCode.ServiceUnavailable, (await client.GetAsync("/api/common/channels/common")).StatusCode);
        File.Delete(Path.Combine(fixture.Channel, "current"));
        Assert.Equal(HttpStatusCode.NotFound, (await client.GetAsync("/api/common/channels/common")).StatusCode);
    }

    [Fact]
    public async Task Linked_metadata_is_refused()
    {
        using var fixture = new Fixture();
        var file = Path.Combine(fixture.Snapshot, "channel.json");
        var outside = Path.Combine(fixture.Data, "outside.json");
        File.Move(file, outside);
        File.CreateSymbolicLink(file, outside);
        using var client = fixture.CreateClient();
        Assert.Equal(HttpStatusCode.UnprocessableEntity, (await client.GetAsync("/api/common/channels/common")).StatusCode);
    }

    [Theory]
    [InlineData("current", ChannelPaths.Kind.Current)]
    [InlineData("snapshots/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/channel.json", ChannelPaths.Kind.Snapshot)]
    [InlineData("snapshots/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/channel.signature.json", ChannelPaths.Kind.Snapshot)]
    [InlineData("snapshots/../channel.json", ChannelPaths.Kind.None)]
    [InlineData("snapshots/aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa/other.json", ChannelPaths.Kind.None)]
    public void Common_members_are_served_but_cannot_bypass_the_legacy_commit(string path, ChannelPaths.Kind kind)
    {
        Assert.Equal(kind, ChannelPaths.Classify(path));
        Assert.False(ChannelPaths.Pushable(path));
    }
}
