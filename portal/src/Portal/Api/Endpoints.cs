// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Globalization;
using System.Security;
using System.Text;
using System.Text.Json;
using Microsoft.Extensions.Options;
using Portal.Channels;

namespace Portal.Api;

/// Common-only read API and separately authenticated operator actions.
public static class Endpoints
{
    public static void MapPortalApi(this WebApplication app)
    {
        var api = app.MapGroup("/api").RequireRateLimiting("api");
        api.MapGet("/channels/{channel}/packages", async (string channel, CommonCatalogue catalogue, HttpContext http) =>
        {
            http.Response.Headers.CacheControl = "no-store";
            var result = await catalogue.Read(channel, ct: http.RequestAborted);
            if (result.View is not { } view) return Results.Problem(result.Error, statusCode: result.Status);
            string query = http.Request.Query["q"].ToString(), authority = http.Request.Query["authority"].ToString(),
                target = http.Request.Query["target"].ToString();
            if (query.Length > 256 || authority.Length > 256 || target.Length > 256)
                return Results.BadRequest();
            var skip = int.TryParse(http.Request.Query["skip"], out var offset) ? Math.Max(0, offset) : 0;
            var take = int.TryParse(http.Request.Query["take"], out var count) ? Math.Clamp(count, 1, 200) : 100;
            var builds = view.Search(query, authority, target).ToArray();
            return Results.Json(new { view.Channel, view.Snapshot, view.ChannelId, view.ChannelPublicKey, view.Sequence,
                view.Signature, view.Trust, view.Review, view.RuntimeTesting, view.Payload, view.Freshness,
                total = builds.Length, skip, take,
                builds = builds.Skip(skip).Take(take).Select(build => new { build, page = view.BuildUrl(build) }) });
        });
        api.MapGet("/channels/{channel}", async (string channel, CommonChannels channels, HttpContext http) =>
        {
            http.Response.Headers.CacheControl = "no-store";
            try
            {
                var observed = await channels.Open(channel, http.RequestAborted);
                return observed is null ? Results.NotFound() : Results.Json(observed);
            }
            catch (InvalidDataException) { return Results.Problem("Common channel verification refused", statusCode: 422); }
            catch (OperationCanceledException) when (!http.RequestAborted.IsCancellationRequested)
            { return Results.Problem("Common verifier timed out", statusCode: 504); }
            catch (Exception e) when (e is IOException or System.ComponentModel.Win32Exception or JsonException)
            { return Results.Problem("Common channel verifier unavailable", statusCode: 503); }
        });
        api.MapGet("/channels/{channel}/builds/{digest}",async (string channel,string digest,HttpContext http,CommonCatalogue catalogue,CommonQualifications qualifications)=>
        {
            http.Response.Headers.CacheControl="no-store";
            var snapshot=http.Request.Query["snapshot"].ToString();
            var result=await catalogue.Read(channel,snapshot.Length==0?null:snapshot,http.RequestAborted);
            if(result.View is not { } view)return Results.Problem(result.Error,statusCode:result.Status);
            var build=view.Builds.SingleOrDefault(row=>row.Digest==digest);
            if(build is null)return Results.NotFound();
            return Results.Json(new { view.Channel,view.Snapshot,build,qualification=await qualifications.Evaluate(view,build,http.RequestAborted) });
        });
        api.MapGet("/policy",(Microsoft.Extensions.Options.IOptions<PortalOptions> options) => Results.Json(new
        { options.Value.Policy.Push, options.Value.Policy.Binaries, options.Value.Policy.LinkHosts,
          options.Value.Policy.NewChannels, options.Value.Policy.SignedPush, options.Value.Policy.Note,
          options.Value.Policy.Contact, options.Value.MaxCommonLinkBytes, options.Value.MaxCommonValidationBytes,
          migrationNotice = options.Value.ShowMigrationNotice ? PortalOptions.MigrationNotice : null }));
        var admin=app.MapGroup("/_admin").RequireRateLimiting("admin").AddEndpointFilter(async (context,next) =>
        {
            var http=context.HttpContext;
            var options=http.RequestServices.GetRequiredService<Microsoft.Extensions.Options.IOptions<PortalOptions>>().Value;
            var local=options.AllowLoopbackHttpPush && http.Connection.RemoteIpAddress is { } ip && System.Net.IPAddress.IsLoopback(ip);
            if(!options.Policy.Admin || (!http.Request.IsHttps && !local))return Results.StatusCode(403);
            if(http.RequestServices.GetRequiredService<Portal.Admin.AdminKeys>().Find(http.Request.Headers.Authorization) is null)
                return Results.StatusCode(401);
            return await next(context);
        });
        admin.MapGet("/publishers",(Portal.Accounts.Registry registry)=>Results.Json(registry.All()));
        admin.MapGet("/failures",(Microsoft.Extensions.Options.IOptions<PortalOptions> options)=>
            Portal.Push.Results2.Text(Portal.Push.Failures.Read(options.Value.StateDir)));
        admin.MapPost("/publishers/{id:long}",(long id,PublisherRights rights,Portal.Accounts.Registry registry)=>
            registry.Change(id,record=>record with { Files=rights.Files,Suspended=rights.Suspended }) ? Results.Ok() : Results.NotFound());
        admin.MapPost("/channels/{channel}/visibility",(string channel,ChannelVisibility request,CommonVisibility visibility)=>
        {
            if(!ChannelPaths.IsChannelName(channel))return Results.BadRequest();
            visibility.SetListed(channel,request.Listed);return Results.Ok();
        });
    }
    public sealed record PublisherRights(bool Files,bool Suspended);
    public sealed record ChannelVisibility(bool Listed);
}
