// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Net;
using System.Net.Http.Headers;
using System.Net.Http.Json;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.Mvc.Testing;
using Portal.Admin;
using Portal.Push;

namespace Portal.Tests;

public class QualificationTests
{
    sealed class Factory : WebApplicationFactory<Program>
    {
        public readonly string Data = Directory.CreateTempSubdirectory("portal-qualification-").FullName;
        public readonly string AdminKey, PushKey;
        public readonly string[] Digests = new string[2];
        readonly string adminConfig, pushConfig;

        public Factory(string architecture = "x86_64")
        {
            (AdminKey, adminConfig) = AdminKeys.Create("reviewer-alice");
            (PushKey, pushConfig) = PublisherKeys.Create("author", "*");
            var dir = Path.Combine(Data, "channels", "demo");
            Directory.CreateDirectory(Path.Combine(dir, "objects"));
            var index = new StringBuilder();
            for (int i = 0; i < 2; i++)
            {
                // Self-asserted review fields have no authority.
                var manifest = $"Format: pkg-manifest 1\nName: tool\nVersion: 1.{i}\nArchitecture: {architecture}\nKind: application\nReviewed: true\nRuntime-tested: true\n";
                Digests[i] = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(manifest))).ToLowerInvariant();
                File.WriteAllText(Path.Combine(dir, "objects", Digests[i] + ".manifest"), manifest);
                index.Append($"tool 1.{i} {architecture} {Digests[i]}\n");
            }
            File.WriteAllText(Path.Combine(dir, "index"), index.ToString());
        }

        protected override void ConfigureWebHost(Microsoft.AspNetCore.Hosting.IWebHostBuilder b)
        {
            b.UseSetting("Portal:DataDir", Data);
            b.UseSetting("Portal:AdminKeys", adminConfig);
            b.UseSetting("Portal:Keys", pushConfig);
        }

        public HttpClient Https() => CreateClient(new WebApplicationFactoryClientOptions { BaseAddress = new Uri("https://localhost") });
        public string Review(int version = 1) => $"/_admin/channels/demo/qualifications/{Digests[version]}/review";
        public string Runtime => $"/_admin/channels/demo/qualifications/{Digests[1]}/runtime";
        public string Read(int version = 1) => $"/api/qualifications/demo/{Digests[version]}";
    }

    static async Task<HttpResponseMessage> Post(HttpClient client, string url, string? key, object body)
    {
        var request = new HttpRequestMessage(HttpMethod.Post, url) { Content = JsonContent.Create(body) };
        if (key is not null) request.Headers.Authorization = new AuthenticationHeaderValue("Bearer", key);
        return await client.SendAsync(request);
    }
    static object Review(string result = "reviewed") => new { result, summary = "Metadata and packaging reviewed", evidence = "https://example.test/review/42", by = "forged-author" };
    static async Task<JsonElement> Json(HttpResponseMessage r) => JsonDocument.Parse(await r.Content.ReadAsStringAsync()).RootElement.Clone();

    [Fact]
    public async Task Open_publication_and_author_claims_do_not_grant_review()
    {
        using var f = new Factory();
        using var c = f.Https();
        var q = await Json(await c.GetAsync(f.Read()));
        Assert.Equal("unreviewed", q.GetProperty("review").GetString());
        Assert.Equal("cpu-only", q.GetProperty("targetIdentity").GetString());
        Assert.Empty(q.GetProperty("history").EnumerateArray());
        Assert.Empty(q.GetProperty("runtimeTests").EnumerateArray());
        Assert.Equal(1, (await Json(await c.GetAsync("/api/search?q=tool"))).GetProperty("total").GetInt32());
        Assert.Equal(0, (await Json(await c.GetAsync("/api/search?q=tool&review=reviewed"))).GetProperty("total").GetInt32());
        Assert.Equal(HttpStatusCode.OK, (await c.GetAsync("/demo/index")).StatusCode);
        Assert.Equal(HttpStatusCode.Unauthorized, (await Post(c, f.Review(), f.PushKey, Review())).StatusCode);
        Assert.Equal(HttpStatusCode.Unauthorized, (await Post(c, f.Review(), null, Review())).StatusCode);
        Assert.Equal(HttpStatusCode.Forbidden, (await Post(f.CreateClient(), f.Review(), f.AdminKey, Review())).StatusCode);
    }

    [Fact]
    public async Task Review_is_bound_to_exact_build_and_authenticated_reviewer_and_can_be_revoked()
    {
        using var f = new Factory();
        using var c = f.Https();
        var before = await c.GetStringAsync("/demo/index");
        var approved = await Post(c, f.Review(), f.AdminKey, Review());
        Assert.Equal(HttpStatusCode.OK, approved.StatusCode);
        var q = await Json(approved);
        Assert.Equal(f.Digests[1], q.GetProperty("manifest").GetString());
        Assert.Equal("1.1", q.GetProperty("version").GetString());
        Assert.Equal("reviewer-alice", q.GetProperty("history")[0].GetProperty("by").GetString());
        Assert.Empty(q.GetProperty("runtimeTests").EnumerateArray());
        Assert.Equal("unreviewed", (await Json(await c.GetAsync(f.Read(0)))).GetProperty("review").GetString());
        Assert.Equal(1, (await Json(await c.GetAsync("/api/search?review=reviewed"))).GetProperty("total").GetInt32());
        Assert.Contains("reviewer-alice", await c.GetStringAsync("/packages/demo/tool"));
        Assert.Contains("reviewed: x86_64", await c.GetStringAsync("/search?review=reviewed"));
        var revoked = await Json(await Post(c, f.Review(), f.AdminKey, Review("revoked")));
        Assert.Equal(2, revoked.GetProperty("history").GetArrayLength());
        Assert.Equal("revoked", revoked.GetProperty("review").GetString());
        Assert.Equal(0, (await Json(await c.GetAsync("/api/search?review=reviewed"))).GetProperty("total").GetInt32());
        Assert.Equal(before, await c.GetStringAsync("/demo/index"));
    }

    [Fact]
    public async Task Runtime_results_are_independent_target_specific_and_keep_failed_retests()
    {
        using var f = new Factory();
        using var c = f.Https();
        foreach (var result in new[] { "passed", "failed" })
        {
            var response = await Post(c, f.Runtime, f.AdminKey, new { result, target = "x86_64-aros-v11-native", summary = "Launch and exit", evidence = "https://example.test/runs/1" });
            Assert.Equal(HttpStatusCode.OK, response.StatusCode);
        }
        var q = await Json(await c.GetAsync(f.Read()));
        Assert.Equal("unreviewed", q.GetProperty("review").GetString());
        Assert.Equal(2, q.GetProperty("history").GetArrayLength());
        var test = Assert.Single(q.GetProperty("runtimeTests").EnumerateArray());
        Assert.Equal("failed", test.GetProperty("result").GetString());
        Assert.Equal("x86_64-aros-v11-native", test.GetProperty("target").GetString());
        Assert.Equal(HttpStatusCode.BadRequest, (await Post(c, f.Runtime, f.AdminKey,
            new { result = "passed", target = "aarch64-aros-v1-hosted", summary = "Wrong CPU", evidence = "https://example.test/runs/2" })).StatusCode);
        Assert.Equal(HttpStatusCode.BadRequest, (await Post(c, f.Runtime, f.AdminKey,
            new { result = "passed", target = "x86_64", summary = "Missing ABI/runtime", evidence = "https://example.test/runs/2" })).StatusCode);
    }

    [Fact]
    public async Task Missing_builds_and_invalid_evidence_are_refused_without_records()
    {
        using var f = new Factory();
        using var c = f.Https();
        Assert.Equal(HttpStatusCode.NotFound, (await Post(c, f.Review().Replace(f.Digests[1], new string('a', 64)), f.AdminKey, Review())).StatusCode);
        Assert.Equal(HttpStatusCode.BadRequest, (await Post(c, f.Review(), f.AdminKey,
            new { result = "reviewed", summary = "bad link", evidence = "javascript:alert(1)" })).StatusCode);
        Assert.Equal(HttpStatusCode.BadRequest, (await Post(c, f.Review(), f.AdminKey, Review("signed"))).StatusCode);
        Assert.Empty((await Json(await c.GetAsync(f.Read()))).GetProperty("history").EnumerateArray());
    }

    [Fact]
    public async Task A_changed_manifest_cannot_receive_a_review_for_its_old_digest()
    {
        using var f = new Factory();
        using var c = f.Https();
        File.AppendAllText(Path.Combine(f.Data, "channels", "demo", "objects", f.Digests[1] + ".manifest"), "Description: different bytes\n");
        Assert.Equal(HttpStatusCode.BadRequest, (await Post(c, f.Review(), f.AdminKey, Review())).StatusCode);
        Assert.Empty((await Json(await c.GetAsync(f.Read()))).GetProperty("history").EnumerateArray());
    }

    [Fact]
    public async Task Qualified_targets_require_the_same_CPU_ABI_and_runtime()
    {
        using var f = new Factory("x86_64-aros-v11-native");
        using var c = f.Https();
        foreach (var target in new[] { "aarch64-aros-v11-native", "x86_64-aros-v1-native", "x86_64-aros-v11-hosted", "x86_64-aros-v11-raspi" })
        {
            var r = await Post(c, f.Runtime, f.AdminKey, new { result = "passed", target, summary = "Launch", evidence = "https://example.test/runs/1" });
            Assert.Equal(HttpStatusCode.BadRequest, r.StatusCode);
        }
        var ok = await Post(c, f.Runtime, f.AdminKey, new { result = "passed", target = "x86_64-aros-v11-native", summary = "Launch on AROS One", evidence = "https://example.test/runs/1" });
        Assert.Equal(HttpStatusCode.OK, ok.StatusCode);
        Assert.Equal("qualified", (await Json(ok)).GetProperty("targetIdentity").GetString());
    }
}
