// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
using System.Net;
using System.Security.Claims;
using System.Text.Encodings.Web;
using System.Text.RegularExpressions;
using Microsoft.AspNetCore.Authentication;
using Microsoft.AspNetCore.Hosting;
using Microsoft.AspNetCore.Mvc.Testing;
using Microsoft.AspNetCore.TestHost;
using Microsoft.Extensions.DependencyInjection;
using Microsoft.Extensions.Logging;
using Microsoft.Extensions.Options;
using Portal.Accounts;
using Portal.Channels;
namespace Portal.Tests;

public sealed class CommonAccountTests
{
    sealed class TestIdentity(IOptionsMonitor<AuthenticationSchemeOptions> options,ILoggerFactory logger,UrlEncoder encoder)
        : AuthenticationHandler<AuthenticationSchemeOptions>(options,logger,encoder)
    {
        protected override Task<AuthenticateResult> HandleAuthenticateAsync()
        {
            var value=Request.Headers["X-Test-User"].ToString().Split(':');
            if(value.Length!=2)return Task.FromResult(AuthenticateResult.NoResult());
            var identity=new ClaimsIdentity([new Claim(ClaimTypes.NameIdentifier,value[0]),new Claim(ClaimTypes.Name,value[1])],"Fixture");
            return Task.FromResult(AuthenticateResult.Success(new AuthenticationTicket(new ClaimsPrincipal(identity),Scheme.Name)));
        }
    }
    sealed class Site:CommonChannelTests.Fixture
    {
        public Site()
        {
            Settings["Portal:GitHub:ClientId"]="synthetic-client";
            Settings["Portal:GitHub:ClientSecret"]="synthetic-secret";
            Settings["Portal:Admins"]="maintainer";
        }
        protected override void ConfigureWebHost(IWebHostBuilder builder)
        {
            base.ConfigureWebHost(builder);
            builder.ConfigureTestServices(services=>services.AddAuthentication(options=>
                options.DefaultAuthenticateScheme="Fixture").AddScheme<AuthenticationSchemeOptions,TestIdentity>("Fixture",_=>{}));
        }
    }
    static string Token(string page)=>WebUtility.HtmlDecode(Regex.Match(page,"name=\"__RequestVerificationToken\"[^>]*value=\"([^\"]+)\"").Groups[1].Value);
    static FormUrlEncodedContent Form(params (string Key,string Value)[] values)=>new(values.Select(pair=>new KeyValuePair<string,string>(pair.Key,pair.Value)));

    [Fact]
    public async Task Account_registration_requires_csrf_and_stable_identity_then_admin_grants_neither_review_nor_client_trust()
    {
        using var site=new Site();using var client=site.CreateClient(new WebApplicationFactoryClientOptions { AllowAutoRedirect=false });
        client.DefaultRequestHeaders.Add("X-Test-User","7:publisher");
        var publicKey=new string('a',64);
        var refused=await client.PostAsync("/account",Form(("PublisherName","Test Publisher"),("PublicKey",publicKey),("Channel","my-tools")));
        Assert.Equal(HttpStatusCode.BadRequest,refused.StatusCode);
        Assert.Null(site.Services.GetRequiredService<Registry>().ByAccount(7));
        var token=Token(await client.GetStringAsync("/account"));Assert.NotEmpty(token);
        var saved=await client.PostAsync("/account",Form(("__RequestVerificationToken",token),("PublisherName","Test Publisher"),("PublicKey",publicKey),("Channel","my-tools")));
        Assert.Equal(HttpStatusCode.OK,saved.StatusCode);
        var registry=site.Services.GetRequiredService<Registry>();
        Assert.False(registry.ByAccount(7)!.Files);
        Assert.True(site.Services.GetRequiredService<CommonVisibility>().IsUnlisted("my-tools"));
        Assert.Equal(HttpStatusCode.NotFound,(await client.GetAsync("/admin")).StatusCode);
        client.DefaultRequestHeaders.Remove("X-Test-User");client.DefaultRequestHeaders.Add("X-Test-User","8:maintainer");
        token=Token(await client.GetStringAsync("/admin"));Assert.NotEmpty(token);
        Assert.Equal(HttpStatusCode.BadRequest,(await client.PostAsync("/admin?handler=Rights",Form(("id","7"),("files","true")))).StatusCode);
        Assert.False(registry.ByAccount(7)!.Files);
        Assert.Equal(HttpStatusCode.Redirect,(await client.PostAsync("/admin?handler=Rights",Form(("__RequestVerificationToken",token),("id","7"),("files","true")))).StatusCode);
        Assert.True(registry.ByAccount(7)!.Files);
        Assert.False(File.Exists(Path.Combine(site.Data,"policy.json")));
        Assert.Equal(HttpStatusCode.BadRequest,(await client.PostAsync("/signout",Form())).StatusCode);
        Assert.Equal(HttpStatusCode.OK,(await client.GetAsync("/privacy")).StatusCode);
    }

    [Fact]
    public async Task Admin_api_needs_its_own_tls_key_and_respects_disabled_policy()
    {
        using var site=new Site();
        var (secret,configuration)=Portal.Admin.AdminKeys.Create("operator");
        site.Settings["Portal:AdminKeys"]=configuration;
        using var client=site.CreateClient(new WebApplicationFactoryClientOptions { BaseAddress=new Uri("https://localhost") });
        Assert.Equal(HttpStatusCode.Unauthorized,(await client.GetAsync("/_admin/publishers")).StatusCode);
        client.DefaultRequestHeaders.Authorization=new("Bearer",secret);
        Assert.Equal(HttpStatusCode.OK,(await client.GetAsync("/_admin/publishers")).StatusCode);
        Assert.Equal(HttpStatusCode.Forbidden,(await client.GetAsync("http://localhost/_admin/publishers")).StatusCode);
        using var disabled=new Site();disabled.Settings["Portal:Policy:Admin"]="false";disabled.Settings["Portal:AdminKeys"]=configuration;
        using var other=disabled.CreateClient(new WebApplicationFactoryClientOptions { BaseAddress=new Uri("https://localhost") });
        other.DefaultRequestHeaders.Authorization=new("Bearer",secret);other.DefaultRequestHeaders.Add("X-Test-User","8:maintainer");
        Assert.Equal(HttpStatusCode.Forbidden,(await other.GetAsync("/_admin/publishers")).StatusCode);
        Assert.Equal(HttpStatusCode.NotFound,(await other.GetAsync("/admin")).StatusCode);
    }
}
