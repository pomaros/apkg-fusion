// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
using System.Net;
using Microsoft.AspNetCore.Builder;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Hosting.Server;
using Microsoft.AspNetCore.Hosting.Server.Features;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Portal.Push;
namespace Portal.Tests;
public sealed class CommonArchiveTransferTests
{
    sealed class Source:IDisposable
    {
        readonly WebApplication app;
        public string Base {get;}
        public int Requests;
        public Source()
        {
            // Keep the kernel-assigned port bound throughout the fixture lifetime.
            var builder=WebApplication.CreateSlimBuilder();builder.Logging.ClearProviders();
            builder.WebHost.UseKestrel(options=>options.Listen(IPAddress.Loopback,0));
            app=builder.Build();
            app.Run(async context=>
            {
                Interlocked.Increment(ref Requests);var response=context.Response;
                switch(context.Request.Path.Value)
                {
                    case "/redirect":response.StatusCode=302;response.Headers.Location="/good";break;
                    case "/loop":response.StatusCode=302;response.Headers.Location="/loop";break;
                    case "/private":response.StatusCode=302;response.Headers.Location="https://169.254.169.254/archive";break;
                    case "/missing":response.StatusCode=404;break;
                    case "/encoded":response.Headers["Content-Encoding"]="gzip";goto default;
                    default:
                        var bytes=context.Request.Path.Value=="/corrupt"?"evil"u8.ToArray():context.Request.Path.Value=="/long"?"extra"u8.ToArray():"data"u8.ToArray();
                        response.ContentLength=bytes.Length;
                        await response.Body.WriteAsync(bytes);break;
                }
            });
            app.StartAsync().GetAwaiter().GetResult();
            Base=app.Services.GetRequiredService<IServer>().Features.Get<IServerAddressesFeature>()!.Addresses.Single();
        }
        public void Dispose()
        {
            app.StopAsync().GetAwaiter().GetResult();app.DisposeAsync().AsTask().GetAwaiter().GetResult();
        }
    }

    [Theory]
    [InlineData("good")][InlineData("redirect")]
    public async Task Socket_transfer_and_checked_redirect_retain_exact_signed_bytes(string route)
    {
        using var source=new Source();var folder=Directory.CreateTempSubdirectory("common-transfer-");
        try
        {
            var path=Path.Combine(folder.FullName,"archive");
            await new CommonArchiveLinks().Download(source.Base+'/'+route,path,(CommonJson.Digest("data"u8),4),[],true,CancellationToken.None);
            Assert.Equal("data"u8.ToArray(),File.ReadAllBytes(path));
        }
        finally{folder.Delete(recursive:true);}
    }
    [Theory]
    [InlineData("loop")][InlineData("private")][InlineData("missing")][InlineData("encoded")][InlineData("corrupt")][InlineData("long")]
    public async Task Refused_network_content_leaves_no_archive(string route)
    {
        using var source=new Source();var folder=Directory.CreateTempSubdirectory("common-transfer-refusal-");
        try
        {
            var path=Path.Combine(folder.FullName,"archive");
            await Assert.ThrowsAsync<InvalidDataException>(()=>new CommonArchiveLinks().Download(source.Base+'/'+route,path,(CommonJson.Digest("data"u8),4),[],true,CancellationToken.None));
            Assert.False(File.Exists(path));
            if(route=="loop")Assert.Equal(6,source.Requests);
            if(route=="private")Assert.Equal(1,source.Requests);
        }
        finally{folder.Delete(recursive:true);}
    }
}
