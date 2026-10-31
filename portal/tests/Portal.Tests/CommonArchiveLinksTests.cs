// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
using System.Net;
using Portal.Push;
namespace Portal.Tests;
public sealed class CommonArchiveLinksTests
{
    [Theory]
    [InlineData("127.0.0.1")][InlineData("10.0.0.1")][InlineData("100.64.0.1")][InlineData("169.254.169.254")]
    [InlineData("172.31.0.1")][InlineData("192.168.1.1")][InlineData("192.0.2.1")][InlineData("198.18.0.1")]
    [InlineData("224.1.1.1")][InlineData("::1")][InlineData("::ffff:127.0.0.1")][InlineData("fe80::1")]
    [InlineData("fc00::1")][InlineData("2001:db8::1")][InlineData("2002:7f00::1")][InlineData("3fff::1")]
    public void Non_public_addresses_are_refused(string text)=>Assert.False(CommonArchiveLinks.IsPublic(IPAddress.Parse(text)));
    [Theory]
    [InlineData("1.1.1.1")][InlineData("8.8.8.8")][InlineData("20.50.60.70")][InlineData("2606:4700:4700::1111")]
    public void Public_addresses_are_accepted(string text)=>Assert.True(CommonArchiveLinks.IsPublic(IPAddress.Parse(text)));
    [Theory]
    [InlineData("http://example.com/a")][InlineData("https://user:password@example.com/a")]
    [InlineData("https://@example.com/a")][InlineData("https://example.com/a#fragment")]
    [InlineData("https://example.com/\\evil")][InlineData("https://example.com/a b")]
    public void Unsafe_link_forms_refuse(string value)=>Assert.Throws<InvalidDataException>(()=>CommonArchiveLinks.Address(value,[],false));
    [Fact]
    public async Task Dns_results_are_all_checked_and_host_policy_does_not_accept_suffix_spoofing()
    {
        Assert.Throws<InvalidDataException>(()=>CommonArchiveLinks.Address("https://allowed.example.evil/a",["allowed.example"],false));
        Assert.Equal("cdn.allowed.example",CommonArchiveLinks.Address("https://cdn.allowed.example/a",["allowed.example"],false).Host);
        var links=new CommonArchiveLinks((host,ct)=>Task.FromResult(new[]{IPAddress.Parse("1.1.1.1"),IPAddress.Loopback}));
        var root=Directory.CreateTempSubdirectory("common-link-dns-");
        try
        {
            var target=Path.Combine(root.FullName,"archive");
            await Assert.ThrowsAsync<InvalidDataException>(()=>links.Download("https://public.example/a",target,(new string('a',64),12),[],false,CancellationToken.None));
            Assert.False(File.Exists(target));
        }
        finally{root.Delete(recursive:true);}
    }
}
