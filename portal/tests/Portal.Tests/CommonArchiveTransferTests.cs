// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
using System.Net;
using System.Net.Sockets;
using Portal.Push;
namespace Portal.Tests;
public sealed class CommonArchiveTransferTests
{
    sealed class Source:IDisposable
    {
        readonly HttpListener listener=new();readonly Task work;
        public string Base {get;}
        public int Requests;
        public Source()
        {
            var socket=new TcpListener(IPAddress.Loopback,0);socket.Start();var port=((IPEndPoint)socket.LocalEndpoint).Port;socket.Stop();
            Base=$"http://127.0.0.1:{port}";listener.Prefixes.Add(Base+"/");listener.Start();
            work=Task.Run(async()=>
            {
                while(listener.IsListening)
                {
                    HttpListenerContext context;
                    try{context=await listener.GetContextAsync();}catch(HttpListenerException){break;}catch(ObjectDisposedException){break;}
                    Interlocked.Increment(ref Requests);var response=context.Response;
                    switch(context.Request.Url!.AbsolutePath)
                    {
                        case "/redirect":response.StatusCode=302;response.RedirectLocation="/good";break;
                        case "/loop":response.StatusCode=302;response.RedirectLocation="/loop";break;
                        case "/private":response.StatusCode=302;response.RedirectLocation="https://169.254.169.254/archive";break;
                        case "/missing":response.StatusCode=404;break;
                        case "/encoded":response.Headers["Content-Encoding"]="gzip";goto default;
                        default:
                            var bytes=context.Request.Url.AbsolutePath=="/corrupt"?"evil"u8.ToArray():context.Request.Url.AbsolutePath=="/long"?"extra"u8.ToArray():"data"u8.ToArray();
                            response.ContentLength64=bytes.Length;
                            await response.OutputStream.WriteAsync(bytes);break;
                    }
                    response.Close();
                }
            });
        }
        public void Dispose(){listener.Close();work.GetAwaiter().GetResult();}
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
