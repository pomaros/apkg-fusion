// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
using System.Globalization;
using System.Net;
using System.Text.Json;
using System.Threading.RateLimiting;
using Microsoft.AspNetCore.DataProtection;
using Microsoft.AspNetCore.HttpOverrides;
using Microsoft.Extensions.Options;
using Portal;
using Portal.Channels;
using Portal.Push;

if (args.Length is 3 or 4 && args[0] == "key")
{
    var (key, config) = PublisherKeys.Create(args[1], args[2], args.Length == 4 && args[3] == "files");
    Console.WriteLine($"key: {key}\nconfig: {config}"); return;
}
if (args is ["adminkey", var administrator])
{
    var (key, config) = Portal.Admin.AdminKeys.Create(administrator);
    Console.WriteLine($"key: {key}\nconfig: {config}"); return;
}
if (args is ["viewkey", var viewer])
{
    var key = Convert.ToHexStringLower(System.Security.Cryptography.RandomNumberGenerator.GetBytes(24));
    var hash = Convert.ToHexStringLower(System.Security.Cryptography.SHA256.HashData(System.Text.Encoding.UTF8.GetBytes(key)));
    Console.WriteLine($"key: {key}\nconfig: {viewer}:{hash}"); return;
}
CultureInfo.DefaultThreadCurrentCulture = CultureInfo.DefaultThreadCurrentUICulture = CultureInfo.InvariantCulture;
var builder = WebApplication.CreateBuilder(args);
builder.Services.Configure<PortalOptions>(builder.Configuration.GetSection("Portal"));
builder.Services.PostConfigure<PortalOptions>(options =>
{
    options.DataDir = Path.GetFullPath(options.DataDir);
    if (options.ApkgPath == "apkg" && File.Exists(Path.Combine(AppContext.BaseDirectory, "apkg")))
        options.ApkgPath = Path.Combine(AppContext.BaseDirectory, "apkg");
});
var data = builder.Configuration["Portal:DataDir"] ?? "data";
builder.Services.AddDataProtection().PersistKeysToFileSystem(new DirectoryInfo(Path.Combine(Path.GetFullPath(data), "state", "auth-keys")));
builder.Services.AddHttpContextAccessor();
builder.Services.AddSingleton<CommonVisibility>();
builder.Services.AddSingleton<CommonEngine>();
builder.Services.AddSingleton<CommonChannels>();
builder.Services.AddSingleton<CommonCatalogue>();
builder.Services.AddSingleton<CommonQualifications>();
builder.Services.AddSingleton<PublisherKeys>();
builder.Services.AddSingleton<SignedPush>();
builder.Services.AddSingleton<Portal.Accounts.Registry>();
builder.Services.AddSingleton<Portal.Admin.AdminKeys>();
builder.Services.AddSingleton<PushService>();
builder.Services.AddRazorPages();
// "Sign in with GitHub": who a publisher is. The portal reads the account's
// number and login from the public profile, asks for no scope, keeps no token.
{
    var gh = builder.Configuration.GetSection("Portal:GitHub");
    var auth = builder.Services.AddAuthentication(Microsoft.AspNetCore.Authentication.Cookies.CookieAuthenticationDefaults.AuthenticationScheme)
        .AddCookie(c =>
        {
            c.Cookie.Name = "apkg-account";
            c.Cookie.HttpOnly = true;
            c.Cookie.SameSite = Microsoft.AspNetCore.Http.SameSiteMode.Lax;
            c.ExpireTimeSpan = TimeSpan.FromDays(30);
            c.SlidingExpiration = true;
            c.LoginPath = "/account";
            c.AccessDeniedPath = "/account";
        });
    if (!string.IsNullOrEmpty(gh["ClientId"]) && !string.IsNullOrEmpty(gh["ClientSecret"]))
        auth.AddOAuth("GitHub", g =>
        {
            g.ClientId = gh["ClientId"]!;
            g.ClientSecret = gh["ClientSecret"]!;
            g.CallbackPath = "/signin-github";
            g.AuthorizationEndpoint = "https://github.com/login/oauth/authorize";
            g.TokenEndpoint = "https://github.com/login/oauth/access_token";
            g.UserInformationEndpoint = "https://api.github.com/user";
            g.SaveTokens = false;
            g.CorrelationCookie.SameSite = Microsoft.AspNetCore.Http.SameSiteMode.Lax;
            g.Events.OnCreatingTicket = async ctx =>
            {
                using var req = new HttpRequestMessage(HttpMethod.Get, ctx.Options.UserInformationEndpoint);
                req.Headers.Authorization = new System.Net.Http.Headers.AuthenticationHeaderValue("Bearer", ctx.AccessToken);
                req.Headers.UserAgent.ParseAdd("AROS-Packages-portal");
                req.Headers.Accept.ParseAdd("application/vnd.github+json");
                using var res = await ctx.Backchannel.SendAsync(req, ctx.HttpContext.RequestAborted);
                res.EnsureSuccessStatusCode();
                using var user = System.Text.Json.JsonDocument.Parse(await res.Content.ReadAsStringAsync(ctx.HttpContext.RequestAborted));
                var id = user.RootElement.GetProperty("id").GetInt64().ToString(System.Globalization.CultureInfo.InvariantCulture);
                var login = user.RootElement.GetProperty("login").GetString() ?? "";
                // The account's number and its login: all the portal ever keeps of it.
                ctx.Identity!.AddClaim(new System.Security.Claims.Claim(System.Security.Claims.ClaimTypes.NameIdentifier, id));
                ctx.Identity.AddClaim(new System.Security.Claims.Claim(System.Security.Claims.ClaimTypes.Name, login));
            };
        });
    builder.Services.AddAuthorization();
}

builder.Services.AddRateLimiter(options =>
{
    options.RejectionStatusCode = 429;
    foreach (var (name, permits) in new[] { ("push",1200), ("api",300), ("admin",60) })
        options.AddPolicy(name, context => RateLimitPartition.GetFixedWindowLimiter(
            context.Connection.RemoteIpAddress?.ToString() ?? "unknown",
            _ => new FixedWindowRateLimiterOptions { PermitLimit = permits, Window = TimeSpan.FromMinutes(1) }));
    options.AddPolicy("reads", context => RateLimitPartition.GetConcurrencyLimiter(
        context.Connection.RemoteIpAddress?.ToString() ?? "unknown", _ => new ConcurrencyLimiterOptions
        { PermitLimit = 8, QueueLimit = 64, QueueProcessingOrder = QueueProcessingOrder.OldestFirst }));
});
builder.Services.Configure<ForwardedHeadersOptions>(options =>
{
    options.ForwardedHeaders = ForwardedHeaders.XForwardedFor | ForwardedHeaders.XForwardedProto;
    options.KnownIPNetworks.Clear(); options.KnownProxies.Clear(); options.ForwardLimit = 1;
});
builder.WebHost.ConfigureKestrel((context, options) => options.Limits.MaxRequestBodySize =
    (context.Configuration.GetValue<long?>("Portal:MaxPartBytes") ?? 40L*1024*1024) + 1024*1024);
var app = builder.Build();
var opts = app.Services.GetRequiredService<IOptions<PortalOptions>>().Value;
Directory.CreateDirectory(opts.ChannelsDir);
if (!OperatingSystem.IsWindows() && opts.ApkgPath == Path.Combine(AppContext.BaseDirectory, "apkg") && File.Exists(opts.ApkgPath))
    File.SetUnixFileMode(opts.ApkgPath, File.GetUnixFileMode(opts.ApkgPath) | UnixFileMode.UserExecute | UnixFileMode.GroupExecute | UnixFileMode.OtherExecute);
app.UseForwardedHeaders();
if (Environment.GetEnvironmentVariable("WEBSITE_SITE_NAME") is not null)
    app.Use(async (context, next) => { context.Request.Scheme = context.Request.Headers.ContainsKey("X-ARR-SSL") ? "https" : "http"; await next(); });
app.UseExceptionHandler(branch => branch.Run(async context =>
{
    var error = context.Features.Get<Microsoft.AspNetCore.Diagnostics.IExceptionHandlerFeature>()?.Error;
    Failures.Note(opts.StateDir, $"{context.Request.Method} {context.Request.Path}", error);
    context.Response.StatusCode = 500;
    await context.Response.WriteAsJsonAsync(new { error = "The operation could not finish. Inspect its result before retrying." });
}));
app.Use(async (context, next) =>
{
    context.Response.Headers["X-Robots-Tag"] = "noindex, nofollow";
    context.Response.Headers.XContentTypeOptions = "nosniff";
    context.Response.Headers["Referrer-Policy"] = "same-origin";
    context.Response.Headers.ContentSecurityPolicy = "default-src 'self'; img-src 'self' data:; style-src 'self' 'unsafe-inline'; script-src 'self'; object-src 'none'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'";
    await next();
});
app.UseStaticFiles(); app.UseRouting(); app.UseRateLimiter(); app.UseAuthentication(); app.UseAuthorization();
app.MapGet("/robots.txt", () => Results.Text("User-agent: *\nDisallow: /\n"));
app.MapGet("/health", async (CommonVisibility visibility, CommonChannels channels, CommonEngine engine, CancellationToken ct) =>
{
    try
    {
        var identity=await engine.Run(opts.ApkgPath,["engine-info"],ct);
        if(identity.GetProperty("contract").GetString()!="1")return Results.StatusCode(503);
        foreach (var name in visibility.ChannelNames().Where(channels.HasReference))
            if (await channels.Open(name, ct) is null) return Results.StatusCode(503);
        return Results.Text("ok: common portal and signed channel storage\n");
    }
    catch (Exception error) when (error is IOException or InvalidDataException or JsonException or OperationCanceledException or System.ComponentModel.Win32Exception)
    { return Results.Text("common channel verification unavailable\n", statusCode: 503); }
});
app.MapRazorPages().RequireRateLimiting("reads");
app.MapGet("/signin", () => opts.GitHub.On ? Results.Challenge(
    new Microsoft.AspNetCore.Authentication.AuthenticationProperties { RedirectUri = "/account" }, ["GitHub"]) : Results.NotFound());
app.MapPost("/signout", async (HttpContext context, Microsoft.AspNetCore.Antiforgery.IAntiforgery antiforgery) =>
{
    try { await antiforgery.ValidateRequestAsync(context); }
    catch (Microsoft.AspNetCore.Antiforgery.AntiforgeryValidationException) { context.Response.StatusCode=400; return; }
    await Microsoft.AspNetCore.Authentication.AuthenticationHttpContextExtensions.SignOutAsync(context);
    context.Response.Redirect("/");
}).DisableAntiforgery();
app.MapGet("/see/{key}", (HttpContext context, string key, CommonVisibility visibility) =>
{
    if (key == "off") { context.Response.Cookies.Delete(CommonVisibility.ViewCookie); return Results.Redirect("/"); }
    if (!visibility.IsViewKey(key)) return Results.NotFound();
    context.Response.Cookies.Append(CommonVisibility.ViewCookie,key,new CookieOptions
    { HttpOnly=true, Secure=context.Request.IsHttps, SameSite=SameSiteMode.Lax, MaxAge=TimeSpan.FromDays(30) });
    return Results.Redirect("/");
}).RequireRateLimiting("admin");
var push = app.MapGroup("/{channel}/_push").RequireRateLimiting("push").AddEndpointFilter(async (context, next) =>
{
    var http = context.HttpContext;
    var channel = (string)http.GetRouteValue("channel")!;
    if (!opts.Policy.Push) return Results2.Text(opts.Policy.Refuse("Push","off","Publishing is disabled.","Contact the operator."),403);
    if (!ChannelPaths.IsChannelName(channel)) return Results.BadRequest();
    var signed = http.RequestServices.GetRequiredService<SignedPush>();
    Publisher? who;
    if (http.Request.Path.Value!.EndsWith("/_push/session",StringComparison.Ordinal))
        return opts.Policy.SignedPush ? await next(context) : Results.StatusCode(403);
    if (SignedPush.IsSigned(http.Request.Headers.Authorization))
    {
        if (!opts.Policy.SignedPush) return Results.StatusCode(403);
        var (publisher, refusal, body) = await signed.Check(http,"Register a publishing key at /account.",http.RequestAborted);
        if (refusal is not null) return Results2.Text(refusal,401);
        http.Response.RegisterForDisposeAsync(body!); http.Request.Body=body!; who=publisher;
    }
    else
    {
        if (!BearerAllowed(http,opts)) return Results2.Text(Record.Refused(20,"Bearer credentials require https.","Sign requests or use https."),403);
        who=http.RequestServices.GetRequiredService<PublisherKeys>().Find(http.Request.Headers.Authorization);
    }
    if (who is null) return Results2.Text(Record.Refused(14,"Unknown publishing identity.","Register your key at /account."),401);
    if (!who.MayPush(channel)) return Results2.Text(Record.Refused(14,"The publisher cannot write this channel.","Use an authorized channel."),403);
    if (!opts.Policy.NewChannels && !Directory.Exists(Path.Combine(opts.ChannelsDir,channel))) return Results.StatusCode(403);
    http.Items["publisher"]=who;
    return await next(context);
});
push.MapPost("/session",async (HttpContext http,SignedPush signed) =>
{
    var answer=signed.Open(await ReadBody(http),"Register a publishing key at /account.");
    return Results2.Text(answer,answer.Get("result")=="session"?200:401);
});
push.MapPost("/plan",async (HttpContext http,string channel,PushService service) =>
    Results2.Text(await service.Plan((Publisher)http.Items["publisher"]!,channel,await ReadBody(http))));
push.MapPut("/files/{**path}",async (HttpContext http,string channel,string path,PushService service) =>
{
    var (answer,status)=await service.PutFile((Publisher)http.Items["publisher"]!,channel,path,
        http.Request.Headers.ContentRange.FirstOrDefault(),http.Request.ContentLength,http.Request.Body,http.RequestAborted);
    return Results2.Text(answer,status);
});
push.MapPost("/common-commit", async (HttpContext http, string channel, PushService service) =>
{
    var bytes = new byte[65537];
    var length = 0;
    while (length < bytes.Length)
    {
        var n = await http.Request.Body.ReadAsync(bytes.AsMemory(length), http.RequestAborted);
        if (n == 0) break;
        length += n;
    }
    if (length == bytes.Length)
        return Results2.Text(Record.Refused(20, "common commit body exceeds 65536 bytes", "send snapshot, expectedCurrent and bounded archive links"), 400);
    CommonCommitRequest? request;
    try
    {
        using var parsed = JsonDocument.Parse(bytes.AsMemory(0, length));
        if (parsed.RootElement.ValueKind != JsonValueKind.Object) throw new JsonException();
        var fields = parsed.RootElement.EnumerateObject().Select(p => p.Name).ToArray();
        if (fields.Length != fields.Distinct(StringComparer.Ordinal).Count() ||
            !fields.Contains("snapshot") || !fields.Contains("expectedCurrent") ||
            fields.Any(field => field is not ("snapshot" or "expectedCurrent" or "links")))
            throw new JsonException();
        request = parsed.RootElement.Deserialize<CommonCommitRequest>(JsonSerializerOptions.Web);
    }
    catch (JsonException)
    { return Results2.Text(Record.Refused(20, "invalid common commit object", "send snapshot and expectedCurrent"), 400); }
    if (request is null)
        return Results2.Text(Record.Refused(20, "missing common commit object", "send snapshot and expectedCurrent"), 400);
    var loopbackLinks = opts.AllowLoopbackHttpPush && http.Connection.RemoteIpAddress is { } address
        && System.Net.IPAddress.IsLoopback(address);
    var (answer, status) = await service.CommitCommon((Publisher)http.Items["publisher"]!, channel, request, http.RequestAborted, loopbackLinks);
    return Results2.Text(answer, status);
});


Portal.Api.Endpoints.MapPortalApi(app);
app.MapMethods("/{channel}",["GET","HEAD"],(string channel,CommonChannels channels) =>
    channels.HasReference(channel)?Results.Redirect($"/channels/{channel}"):Results.NotFound());
app.MapMethods("/{channel}/{**path}",["GET","HEAD"],(HttpContext http,string channel,string? path) =>
{
    if (!ChannelPaths.IsChannelName(channel) || path is null || ChannelPaths.Classify(path)==ChannelPaths.Kind.None) return Results.NotFound();
    var file=Path.Combine(opts.ChannelsDir,channel,path);
    if (ChannelPaths.Classify(path)==ChannelPaths.Kind.Archive && File.Exists(file+".url") && !File.Exists(file))
        return Results.Redirect(File.ReadAllText(file+".url").Trim());
    if (!File.Exists(file)) return Results.NotFound();
    http.Response.Headers.CacheControl=path=="current"?"no-cache":"public, max-age=31536000, immutable";
    return Results.File(file,ChannelPaths.ContentType(path),enableRangeProcessing:true);
}).RequireRateLimiting("reads");
app.Run();
static bool BearerAllowed(HttpContext http,PortalOptions options) => http.Request.IsHttps ||
    options.AllowLoopbackHttpPush && http.Connection.RemoteIpAddress is { } ip && IPAddress.IsLoopback(ip);
static async Task<string> ReadBody(HttpContext http)
{
    using var output=new MemoryStream();
    var bytes=new byte[8192];int count;
    while ((count=await http.Request.Body.ReadAsync(bytes,http.RequestAborted))!=0)
    { if(output.Length+count>2*1024*1024) throw new BadHttpRequestException("Request exceeds metadata bound",413); output.Write(bytes,0,count); }
    return System.Text.Encoding.UTF8.GetString(output.ToArray());
}
public partial class Program { }
