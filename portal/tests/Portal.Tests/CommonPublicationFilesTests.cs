// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
using System.Net;
using Microsoft.AspNetCore.Mvc.Testing;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.DependencyInjection;
using Portal.Push;
namespace Portal.Tests;

public sealed class CommonPublicationFilesTests
{
    [Fact]
    public async Task Publication_lock_is_private_cooperative_cancellable_and_reusable()
    {
        var root=Directory.CreateTempSubdirectory("common-publication-files-");
        try
        {
            var files=new CommonPublicationFiles();
            using(var first=await files.Lock(root.FullName,root.FullName,CancellationToken.None))
            {
                using var cancel=new CancellationTokenSource(TimeSpan.FromMilliseconds(100));
                await Assert.ThrowsAnyAsync<OperationCanceledException>(()=>files.Lock(root.FullName,root.FullName,cancel.Token));
            }
            using var again=await files.Lock(root.FullName,root.FullName,CancellationToken.None);
            if(!OperatingSystem.IsWindows())Assert.Equal(UnixFileMode.UserRead|UnixFileMode.UserWrite,File.GetUnixFileMode(Path.Combine(root.FullName,".publish.lock")));
        }
        finally{root.Delete(recursive:true);}
    }
    [Fact]
    public async Task Linked_special_or_hardlinked_lock_files_are_rejected_without_blocking()
    {
        var root=Directory.CreateTempSubdirectory("common-publication-members-");
        try
        {
            var files=new CommonPublicationFiles();var ordinary=Path.Combine(root.FullName,"regular");File.WriteAllText(ordinary,"data");
            var linked=Path.Combine(root.FullName,"linked");File.CreateSymbolicLink(linked,ordinary);
            await Assert.ThrowsAsync<InvalidDataException>(()=>files.ReadBounded(linked,100,root.FullName,CancellationToken.None));
            var fifo=Path.Combine(root.FullName,"fifo");CommonChannelTests.Fixture.Run("/usr/bin/mkfifo",fifo);
            await Assert.ThrowsAsync<InvalidDataException>(()=>files.ReadBounded(fifo,100,root.FullName,CancellationToken.None));
            var gate=Path.Combine(root.FullName,".publish.lock");File.WriteAllText(gate,"");CommonChannelTests.Fixture.Run("/bin/ln",gate,Path.Combine(root.FullName,"alias"));
            await Assert.ThrowsAsync<InvalidDataException>(()=>files.Lock(root.FullName,root.FullName,CancellationToken.None));
        }
        finally{root.Delete(recursive:true);}
    }
    [Fact]
    public async Task Immutable_members_and_current_keep_exact_bytes_and_refuse_replacement_or_stale_heads()
    {
        var root=Directory.CreateTempSubdirectory("common-immutable-");
        try
        {
            var files=new CommonPublicationFiles();var member=Path.Combine(root.FullName,"member");
            await files.ImmutableBytes(member,"exact"u8.ToArray(),root.FullName,CancellationToken.None);
            await files.ImmutableBytes(member,"exact"u8.ToArray(),root.FullName,CancellationToken.None);
            await Assert.ThrowsAsync<InvalidDataException>(()=>files.ImmutableBytes(member,"changed"u8.ToArray(),root.FullName,CancellationToken.None));
            await files.ReplaceCurrent(root.FullName,null,new string('a',64),root.FullName,CancellationToken.None);
            await Assert.ThrowsAsync<InvalidDataException>(()=>files.ReplaceCurrent(root.FullName,null,new string('b',64),root.FullName,CancellationToken.None));
            Assert.Equal(new string('a',64),await files.Current(root.FullName,root.FullName,CancellationToken.None));
            Assert.Empty(Directory.GetFiles(root.FullName,".incoming-*"));Assert.Empty(Directory.GetFiles(root.FullName,".current-*"));
        }
        finally{root.Delete(recursive:true);}
    }

    sealed class FaultingFiles(string endpoint,bool afterVisibility):CommonPublicationFiles
    {
        public bool Fail=true;
        public override void SyncDirectory(string path)
        {
            if(Fail && afterVisibility && path==endpoint && File.Exists(Path.Combine(endpoint,"current")))throw new IOException("Injected final flush failure");
            base.SyncDirectory(path);
        }
        public override Task ImmutableArchive(string path,string source,string root,string sourceRoot,(string Digest,long Size) expected,CancellationToken ct)
        {
            if(Fail && !afterVisibility)throw new IOException("Injected archive write failure");
            return base.ImmutableArchive(path,source,root,sourceRoot,expected,ct);
        }
    }
    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public async Task Publication_reports_visibility_after_io_failure_and_identical_retry_completes(bool afterVisibility)
    {
        using var site=new CommonPushTests.Site();var files=new FaultingFiles(site.Live,afterVisibility);
        using var factory=site.WithWebHostBuilder(builder=>builder.ConfigureTestServices(services=>services.AddSingleton<CommonPublicationFiles>(files)));
        using var client=factory.CreateClient(new WebApplicationFactoryClientOptions{BaseAddress=new Uri("https://localhost")});
        client.DefaultRequestHeaders.Authorization=new("Bearer",site.Key);
        var snapshot=site.Snapshot(1);await site.Upload(client,site.Files(snapshot));
        var failure=await site.Commit(client,snapshot);
        Assert.Equal(afterVisibility?HttpStatusCode.ServiceUnavailable:HttpStatusCode.UnprocessableEntity,failure.StatusCode);
        Assert.Equal(afterVisibility,File.Exists(Path.Combine(site.Live,"current")));
        Assert.Equal(afterVisibility,File.Exists(Path.Combine(site.Live,".portal-owner.json")));
        if(afterVisibility)Assert.Contains("result: uncertain",await failure.Content.ReadAsStringAsync());
        files.Fail=false;
        var retry=await site.Commit(client,snapshot);Assert.Equal(HttpStatusCode.OK,retry.StatusCode);
        Assert.Contains(afterVisibility?"result: unchanged":"result: published",await retry.Content.ReadAsStringAsync());
        Assert.Equal(snapshot+"\n",File.ReadAllText(Path.Combine(site.Live,"current")));
    }
}
