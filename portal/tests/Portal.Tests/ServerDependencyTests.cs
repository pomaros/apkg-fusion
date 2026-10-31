// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
using System.Net;
using System.Text.Json;
namespace Portal.Tests;

[CollectionDefinition("Process environment",DisableParallelization=true)]
public sealed class ProcessEnvironmentCollection;

[Collection("Process environment")]
public sealed class ServerDependencyTests
{
    [Fact]
    public async Task Publication_and_qualification_work_without_python_on_the_server_path()
    {
        using var publisher=new CommonPushTests.Site();
        using var reader=new CommonChannelTests.Fixture(rich:true);
        using var fixture=JsonDocument.Parse(CommonChannelTests.Fixture.Run("python3",Path.Combine(reader.Repository,"portal/tests/qualification_fixture.py"),reader.Repository,reader.Data,reader.Engine));
        reader.Settings["Portal:QualificationPolicyPath"]=Path.Combine(reader.Data,"qualification-policy.json");
        var snapshot=publisher.Snapshot(1);var files=publisher.Files(snapshot);
        using var upload=publisher.Client();using var browse=reader.CreateClient();
        var path=$"/api/channels/common/builds/{fixture.RootElement.GetProperty("descriptor").GetString()}?snapshot={reader.Head}";
        var original=Environment.GetEnvironmentVariable("PATH");
        try
        {
            Environment.SetEnvironmentVariable("PATH","/no-server-interpreters");
            await publisher.Upload(upload,files);
            Assert.Equal(HttpStatusCode.OK,(await publisher.Commit(upload,snapshot)).StatusCode);
            using var response=JsonDocument.Parse(await browse.GetStringAsync(path));
            Assert.Equal("evaluated",response.RootElement.GetProperty("qualification").GetProperty("status").GetString());
        }
        finally{Environment.SetEnvironmentVariable("PATH",original);}
    }
}
