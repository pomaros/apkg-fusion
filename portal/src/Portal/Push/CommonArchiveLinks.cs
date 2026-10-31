// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
namespace Portal.Push;

public sealed class CommonArchiveLinks(Func<string,CancellationToken,Task<IPAddress[]>>? resolver=null)
{
    static bool Prefix(byte[] bytes,byte[] network,int bits)
    {
        for(var i=0;i<bits/8;i++)if(bytes[i]!=network[i])return false;
        var remainder=bits%8;
        return remainder==0 || (bytes[bits/8] & (255<<(8-remainder)))==(network[bits/8] & (255<<(8-remainder)));
    }
    public static bool IsPublic(IPAddress address)
    {
        if(address.IsIPv4MappedToIPv6)address=address.MapToIPv4();
        var bytes=address.GetAddressBytes();
        if(bytes.Length==4)
        {
            if(bytes[0] is 0 or 10 or 127 || bytes[0]>=224)return false;
            if(Prefix(bytes,[100,64],10)||Prefix(bytes,[169,254],16)||Prefix(bytes,[172,16],12)||Prefix(bytes,[192,168],16))return false;
            if(Prefix(bytes,[192,0,0],24)||Prefix(bytes,[192,0,2],24)||Prefix(bytes,[192,88,99],24)||Prefix(bytes,[198,18],15)
                ||Prefix(bytes,[198,51,100],24)||Prefix(bytes,[203,0,113],24))return false;
            return true;
        }
        return Prefix(bytes,[0x20],3) && !Prefix(bytes,[0x20,0x01,0],23) && !Prefix(bytes,[0x20,0x01,0x0d,0xb8],32)
            && !Prefix(bytes,[0x20,0x02],16) && !Prefix(bytes,[0x3f,0xff,0],20);
    }
    public static Uri Address(string value,IReadOnlyList<string> hosts,bool allowLoopback)
    {
        if(value.Length>2048 || value.Any(c=>c<=32 || c>126) || value.Contains('\\') || !Uri.TryCreate(value,UriKind.Absolute,out var uri))
            throw new InvalidDataException("Archive link must be a bounded printable HTTPS address");
        var authority=value[(value.IndexOf("://",StringComparison.Ordinal)+3)..].Split('/','?','#')[0];
        if(uri.UserInfo.Length!=0 || authority.Contains('@') || uri.Fragment.Length!=0 || uri.Host.Length==0)
            throw new InvalidDataException("Archive link has credentials, fragment or invalid host");
        var host=uri.IdnHost;
        var local=allowLoopback && host is "127.0.0.1" or "::1";
        if(uri.Scheme!="https" && !(local && uri.Scheme=="http"))throw new InvalidDataException("Archive links require HTTPS");
        if(hosts.Count!=0 && !hosts.Any(h=>host.Equals(h,StringComparison.OrdinalIgnoreCase)||host.EndsWith('.'+h,StringComparison.OrdinalIgnoreCase)))
            throw new InvalidDataException("Archive host is not allowed by this portal");
        return uri;
    }
    public async Task Download(string url,string destination,(string Digest,long Size) expected,IReadOnlyList<string> hosts,bool allowLoopback,CancellationToken ct)
    {
        var next=url;
        try
        {
            for(var redirect=0;redirect<6;redirect++)
            {
                var uri=Address(next,hosts,allowLoopback);
                var local=allowLoopback && uri.IdnHost is "127.0.0.1" or "::1";
                var addresses=IPAddress.TryParse(uri.IdnHost,out var literal)?[literal]:
                    await (resolver?.Invoke(uri.IdnHost,ct)??Dns.GetHostAddressesAsync(uri.IdnHost,ct));
                if(addresses.Length==0 || addresses.Any(address=>!IsPublic(address) && !(local && IPAddress.IsLoopback(address))))
                    throw new InvalidDataException("Archive links must resolve exclusively to public addresses");
                using var handler=new SocketsHttpHandler
                {
                    UseProxy=false,AllowAutoRedirect=false,AutomaticDecompression=DecompressionMethods.None,
                    ConnectCallback=async (context,cancellation)=>
                    {
                        var socket=new Socket(SocketType.Stream,ProtocolType.Tcp);
                        try { await socket.ConnectAsync(addresses,context.DnsEndPoint.Port,cancellation);return new NetworkStream(socket,ownsSocket:true); }
                        catch {socket.Dispose();throw;}
                    }
                };
                using var client=new HttpClient(handler){Timeout=TimeSpan.FromSeconds(20)};
                using var request=new HttpRequestMessage(HttpMethod.Get,uri);
                request.Headers.UserAgent.ParseAdd("APKG-Portal/1");request.Headers.AcceptEncoding.ParseAdd("identity");
                using var response=await client.SendAsync(request,HttpCompletionOption.ResponseHeadersRead,ct);
                if((int)response.StatusCode is 301 or 302 or 303 or 307 or 308)
                {
                    if(redirect==5 || response.Headers.Location is not { } location)throw new InvalidDataException("Archive redirect limit exceeded or location missing");
                    var following=new Uri(uri,location);
                    if(uri.Scheme=="https" && following.Scheme!="https")throw new InvalidDataException("Archive redirect cannot downgrade HTTPS");
                    next=following.AbsoluteUri;continue;
                }
                if(response.StatusCode!=HttpStatusCode.OK || response.Content.Headers.ContentEncoding.Any(value=>!value.Equals("identity",StringComparison.OrdinalIgnoreCase)))
                    throw new InvalidDataException("Archive response must be complete and unencoded");
                if(response.Content.Headers.ContentLength is { } length && length!=expected.Size)throw new InvalidDataException("Archive length differs from signed reference");
                await using var input=await response.Content.ReadAsStreamAsync(ct);
                await using var output=new FileStream(destination,FileMode.CreateNew,FileAccess.Write,FileShare.None);
                using var hash=IncrementalHash.CreateHash(HashAlgorithmName.SHA256);var buffer=new byte[65536];long size=0;int count;
                while((count=await input.ReadAsync(buffer.AsMemory(0,(int)Math.Min(buffer.Length,expected.Size-size+1)),ct))>0)
                {
                    size+=count;if(size>expected.Size)throw new InvalidDataException("Archive exceeds signed size");
                    hash.AppendData(buffer,0,count);await output.WriteAsync(buffer.AsMemory(0,count),ct);
                }
                if(size!=expected.Size || Convert.ToHexStringLower(hash.GetHashAndReset())!=expected.Digest)throw new InvalidDataException("Archive differs from signed digest or size");
                return;
            }
            throw new InvalidDataException("Archive redirect limit exceeded");
        }
        catch {File.Delete(destination);throw;}
    }
}
