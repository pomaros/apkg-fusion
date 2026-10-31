// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
using System.Net;
using System.Text.Json;
using System.Text.Json.Nodes;
namespace Portal.Tests;
public sealed class CommonQualificationTests
{
    [Fact]
    public async Task Policy_evaluation_preserves_revocation_history_rejects_self_review_and_rechecks_role_withdrawal()
    {
        using var site=new CommonChannelTests.Fixture(rich:true);
        using var fixture=JsonDocument.Parse(CommonChannelTests.Fixture.Run("python3",Path.Combine(site.Repository,"portal/tests/qualification_fixture.py"),site.Repository,site.Data,site.Engine));
        var policy=Path.Combine(site.Data,"qualification-policy.json");
        site.Settings["Portal:QualificationPolicyPath"]=policy;
        using var client=site.CreateClient();
        var endpoint=$"/api/channels/common/builds/{fixture.RootElement.GetProperty("descriptor").GetString()}?snapshot={site.Head}";
        using var first=JsonDocument.Parse(await client.GetStringAsync(endpoint));
        var evaluation=first.RootElement.GetProperty("qualification");
        Assert.Equal("evaluated",evaluation.GetProperty("status").GetString());
        Assert.Equal("not-asserted",evaluation.GetProperty("current_approval").GetString());
        Assert.Equal("unknown",evaluation.GetProperty("freshness").GetString());
        var claims=evaluation.GetProperty("observations").EnumerateArray().ToArray();
        Assert.Equal(4,claims.Length);
        var self=claims.Single(claim=>claim.GetProperty("issuer").GetString()==new string('1',32));
        Assert.Equal("accepted",self.GetProperty("issuer_trust").GetString());
        Assert.False(self.GetProperty("independent").GetBoolean());
        var revoke=claims.Single(claim=>claim.GetProperty("result").GetString()=="revoked");
        Assert.Equal("accepted",revoke.GetProperty("issuer_trust").GetString());
        Assert.Equal("linked-prefix",revoke.GetProperty("history").GetString());
        Assert.Equal("2",revoke.GetProperty("sequence").GetString());
        Assert.True(revoke.GetProperty("independent").GetBoolean());
        Assert.Contains(claims,claim=>claim.GetProperty("kind").GetString()=="runtime" && claim.GetProperty("result").GetString()=="failed");
        var root=JsonNode.Parse(File.ReadAllText(policy))!;root["body"]!["reviewers"]=new JsonArray();root["body"]!["testers"]=new JsonArray();
        // Preserve canonical encoding before the common parser reads the new policy.
        File.WriteAllText(policy,root.ToJsonString());
        CommonChannelTests.Fixture.Run("python3","-c","import json,sys;p=sys.argv[1];v=json.load(open(p));open(p,'w').write(json.dumps(v,sort_keys=True,separators=(',',':')))",policy);
        using var withdrawn=JsonDocument.Parse(await client.GetStringAsync(endpoint));
        Assert.NotEqual(evaluation.GetProperty("policy_sha256").GetString(),withdrawn.RootElement.GetProperty("qualification").GetProperty("policy_sha256").GetString());
        Assert.All(withdrawn.RootElement.GetProperty("qualification").GetProperty("observations").EnumerateArray(),claim=>Assert.Equal("rejected",claim.GetProperty("issuer_trust").GetString()));
        foreach(var malformed in new[]{"{}","[]","{\"body\":[]}","{\"body\":\"invalid\"}"})
        {
            File.WriteAllText(policy,malformed);
            using var invalid=JsonDocument.Parse(await client.GetStringAsync(endpoint));
            Assert.Equal("unavailable",invalid.RootElement.GetProperty("qualification").GetProperty("status").GetString());
        }
        Assert.Equal(HttpStatusCode.OK,(await client.GetAsync("/api/channels/common/packages")).StatusCode); // publication stays independent
    }
}
