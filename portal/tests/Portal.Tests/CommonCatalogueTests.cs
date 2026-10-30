// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace Portal.Tests;

public sealed class CommonCatalogueTests
{
    const string Packages = "/api/common/channels/common/packages";
    const string Browse = "/packages/apkg/channels/common";

    [Fact]
    public async Task Common_builds_are_browsable_without_losing_author_identity_precision_or_escaping()
    {
        using var site = new CommonChannelTests.Fixture(rich: true);
        using var client = site.CreateClient();
        var before = Directory.GetFiles(site.Channel, "*", SearchOption.AllDirectories)
            .ToDictionary(p => p, p => SHA256.HashData(File.ReadAllBytes(p)));
        var index = await client.GetStringAsync("/packages/apkg");
        Assert.Contains(Browse, index);
        using var data = JsonDocument.Parse(await client.GetStringAsync(Packages));
        var response = data.RootElement;
        Assert.Equal(2, response.GetProperty("total").GetInt32());
        Assert.Equal("verified", response.GetProperty("signature").GetString());
        foreach (var fact in new[] { "trust", "review", "runtimeTesting" })
            Assert.Equal("not-evaluated", response.GetProperty(fact).GetString());
        Assert.Equal("not-checked", response.GetProperty("payload").GetString());
        var page = await client.GetStringAsync(Browse);
        Assert.Equal(2, Regex.Matches(page, ">same-name</a>").Count);
        Assert.Contains("&lt;script&gt;", page);
        Assert.DoesNotContain("<script>alert", page);
        foreach (var item in response.GetProperty("builds").EnumerateArray())
        {
            var build = item.GetProperty("build");
            Assert.Equal("18446744073709551615", build.GetProperty("revision").GetString());
            Assert.Equal(1, build.GetProperty("attestations").GetArrayLength());
            Assert.Equal("passed", build.GetProperty("attestations")[0].GetProperty("record").GetProperty("body").GetProperty("result").GetString());
            // JSON response encoding may escape text; use the exact source bytes
            // to compare the digest with the common client's canonical identity.
            using var original = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(site.Snapshot, "channel.json")));
            var authority = build.GetProperty("authority").GetString();
            var entry = original.RootElement.GetProperty("body").GetProperty("entries").EnumerateArray()
                .Single(e => e.GetProperty("descriptor").GetProperty("body").GetProperty("authority").GetString() == authority);
            var digest = Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(entry.GetProperty("descriptor").GetRawText())));
            Assert.Equal(digest, build.GetProperty("digest").GetString());
            var detail = await client.GetStringAsync(item.GetProperty("page").GetString());
            Assert.Contains(authority!, detail);
            Assert.Contains("18446744073709551615", detail);
            Assert.Contains("Drawer", detail);
            Assert.Contains(".hidden", detail);
            Assert.Contains("Empty", detail);
            Assert.Contains("preserve", detail);
            Assert.Contains("apkg channel-install ROOT", detail);
            Assert.Contains("reviewer and tester authorization have not been evaluated", detail);
            Assert.DoesNotContain("<script>alert", detail);
        }
        Assert.Equal(before.Count, Directory.GetFiles(site.Channel, "*", SearchOption.AllDirectories).Length);
        foreach (var file in before) Assert.Equal(file.Value, SHA256.HashData(File.ReadAllBytes(file.Key)));
    }

    [Fact]
    public async Task Search_filters_complete_author_target_and_file_identity_and_paginates_without_collapsing_names()
    {
        using var site = new CommonChannelTests.Fixture();
        using var client = site.CreateClient();
        using var first = JsonDocument.Parse(await client.GetStringAsync(Packages + "?q=.hidden&take=1"));
        using var second = JsonDocument.Parse(await client.GetStringAsync(Packages + "?q=.hidden&take=1&skip=1"));
        var a = first.RootElement.GetProperty("builds")[0].GetProperty("build");
        var b = second.RootElement.GetProperty("builds")[0].GetProperty("build");
        Assert.NotEqual(a.GetProperty("authority").GetString(), b.GetProperty("authority").GetString());
        Assert.NotEqual(a.GetProperty("digest").GetString(), b.GetProperty("digest").GetString());
        using var filtered = JsonDocument.Parse(await client.GetStringAsync(Packages + "?authority=" + new string('1', 32)));
        Assert.Equal(1, filtered.RootElement.GetProperty("total").GetInt32());
        using var other = JsonDocument.Parse(await client.GetStringAsync(Packages + "?target=i386-aros-v0-native"));
        Assert.Equal(0, other.RootElement.GetProperty("total").GetInt32());
        Assert.Equal(HttpStatusCode.BadRequest, (await client.GetAsync(Packages + "?q=" + new string('x', 257))).StatusCode);
        Assert.Equal(HttpStatusCode.BadRequest, (await client.GetAsync(Browse + "?q=" + new string('x', 257))).StatusCode);
    }

    [Fact]
    public async Task Exact_build_page_retains_its_signed_snapshot_when_the_head_advances()
    {
        using var site = new CommonChannelTests.Fixture();
        using var client = site.CreateClient();
        var newest = site.Head;
        var first = Directory.GetDirectories(Path.Combine(site.Channel, "snapshots"))
            .Single(p => Path.GetFileName(p) != newest);
        File.WriteAllText(Path.Combine(site.Channel, "current"), Path.GetFileName(first) + "\n");
        using var data = JsonDocument.Parse(await client.GetStringAsync(Packages));
        var link = data.RootElement.GetProperty("builds")[0].GetProperty("page").GetString()!;
        var before = await client.GetStringAsync(link);
        File.WriteAllText(Path.Combine(site.Channel, "current"), newest + "\n");
        var after = await client.GetStringAsync(link);
        Assert.Contains(Path.GetFileName(first), after);
        Assert.DoesNotContain(newest, after);
        Assert.Contains("Signed snapshot", before);
        File.WriteAllText(Path.Combine(first, "channel.signature.json"), "{}");
        Assert.Equal(HttpStatusCode.UnprocessableEntity, (await client.GetAsync(link)).StatusCode);
        Assert.Equal(HttpStatusCode.OK, (await client.GetAsync(Packages)).StatusCode);
        Assert.Equal(HttpStatusCode.NotFound, (await client.GetAsync(link.Replace(Path.GetFileName(first), new string('0', 64)))).StatusCode);
    }

    [Fact]
    public async Task Unlisted_common_channels_stay_out_of_discovery_but_direct_links_and_authorized_viewers_work()
    {
        using var site = new CommonChannelTests.Fixture();
        var key = "synthetic-viewer-key";
        site.Settings["Portal:Unlisted"] = "common";
        site.Settings["Portal:ViewKeys"] = "viewer:" + Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(key)));
        using var client = site.CreateClient();
        Assert.DoesNotContain(Browse, await client.GetStringAsync("/packages/apkg"));
        Assert.Equal(HttpStatusCode.OK, (await client.GetAsync(Browse)).StatusCode);
        client.DefaultRequestHeaders.Add("Cookie", Portal.Channels.Catalogue.ViewCookie + "=" + key);
        Assert.Contains(Browse, await client.GetStringAsync("/packages/apkg"));
    }

    [Theory]
    [InlineData("signature", HttpStatusCode.UnprocessableEntity)]
    [InlineData("missing-engine", HttpStatusCode.ServiceUnavailable)]
    [InlineData("absent", HttpStatusCode.NotFound)]
    public async Task Common_browsing_never_falls_back_to_legacy_data_or_leaks_local_errors(string failure, HttpStatusCode expected)
    {
        using var site = new CommonChannelTests.Fixture();
        File.WriteAllText(Path.Combine(site.Channel, "index"), "legacy-secret 1.0 x86_64 " + new string('a', 64) + "\n");
        if (failure == "signature") File.WriteAllText(Path.Combine(site.Snapshot, "channel.signature.json"), "{}");
        if (failure == "missing-engine") site.Engine = Path.Combine(site.Data, "do-not-leak-this-path");
        if (failure == "absent") File.Delete(Path.Combine(site.Channel, "current"));
        using var client = site.CreateClient();
        foreach (var path in new[] { Browse, Packages })
        {
            var response = await client.GetAsync(path);
            Assert.Equal(expected, response.StatusCode);
            Assert.Equal("no-store", response.Headers.CacheControl!.ToString());
            var text = await response.Content.ReadAsStringAsync();
            Assert.DoesNotContain("legacy-secret", text);
            Assert.DoesNotContain(site.Data, text);
            Assert.DoesNotContain("do-not-leak-this-path", text);
        }
    }
}
