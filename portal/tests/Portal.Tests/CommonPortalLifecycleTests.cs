// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Net;
using System.Text.Json;

namespace Portal.Tests;

public sealed class CommonPortalLifecycleTests
{
    [Fact]
    public async Task Published_build_is_browsable_installable_by_both_common_clients_and_removable_offline()
    {
        using var site = new CommonPushTests.Site(rich: true) { Loopback = true };
        site.UseKestrel(0);
        using var client = site.CreateClient();
        client.DefaultRequestHeaders.Authorization = new("Bearer", site.Key);
        var first = site.Snapshot(1);
        await site.Upload(client, site.Files(first), parts: true);
        Assert.Equal(HttpStatusCode.OK, (await site.Commit(client, first)).StatusCode);
        using var catalogue = JsonDocument.Parse(await client.GetStringAsync("/api/common/channels/common/packages"));
        Assert.Equal(1, catalogue.RootElement.GetProperty("total").GetInt32());
        var item = catalogue.RootElement.GetProperty("builds")[0];
        var link = item.GetProperty("page").GetString()!;
        var page = await client.GetStringAsync(link);
        Assert.Contains("Packaging revision", page);
        Assert.Contains("18446744073709551615", page);
        var temporary = Directory.CreateTempSubdirectory("portal-client-cycle-");
        try
        {
            var script = Path.Combine(site.Source.Repository, "portal", "tests", "common_http_lifecycle.py");
            var engine = site.Source.Engine;
            var endpoint = new Uri(client.BaseAddress!, "/common").AbsoluteUri;
            var output = CommonChannelTests.Fixture.Run("python3", script, "install", engine, temporary.FullName,
                site.Source.Data, endpoint, first, item.GetProperty("build").GetProperty("digest").GetString()!);
            Assert.Contains("actual PkgManager worker", output);
            var second = site.Snapshot(2);
            await site.Upload(client, site.Files(second));
            Assert.Equal(HttpStatusCode.OK, (await site.Commit(client, second, first)).StatusCode);
            using var updated = JsonDocument.Parse(await client.GetStringAsync("/api/common/channels/common/packages"));
            Assert.Equal(2, updated.RootElement.GetProperty("total").GetInt32());
            Assert.Contains(first, await client.GetStringAsync(link));
            // Stop the actual socket listener before testing offline removal.
            site.Dispose();
            output = CommonChannelTests.Fixture.Run("python3", script, "remove", engine, temporary.FullName);
            Assert.Contains("Offline removal passed", output);
        }
        finally { temporary.Delete(recursive: true); }
    }
}
