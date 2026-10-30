// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
using System.Text.Json;
using Microsoft.Extensions.Options;
using Portal.Push;
namespace Portal.Channels;

public sealed class CommonQualifications(IOptions<PortalOptions> options,CommonEngine engine)
{
    public async Task<JsonElement> Evaluate(CommonCatalogueView view,CommonBuild build,CancellationToken ct=default)
    {
        var settings=options.Value;
        if(settings.QualificationPolicyPath.Length==0)
            return JsonSerializer.SerializeToElement(new { status="not-evaluated",observations=Array.Empty<object>(),current_approval="not-asserted",freshness="unknown" });
        var scratch=Directory.CreateTempSubdirectory("apkg-portal-qualification-");
        try
        {
            return await engine.Run(settings.PythonPath,[Path.Combine(AppContext.BaseDirectory,"CommonTools","apkg-portal-qualify"),
                "--engine",settings.ApkgPath,"--endpoint",Path.Combine(settings.ChannelsDir,view.Channel),
                "--snapshot",view.Snapshot,"--descriptor",build.Digest,"--policy",settings.QualificationPolicyPath,"--scratch",scratch.FullName],ct);
        }
        catch(Exception error) when(error is IOException or InvalidDataException or System.ComponentModel.Win32Exception or JsonException or OperationCanceledException)
        {
            if(ct.IsCancellationRequested)throw;
            return JsonSerializer.SerializeToElement(new { status="unavailable",observations=Array.Empty<object>(),current_approval="not-asserted",freshness="unknown" });
        }
        finally { scratch.Delete(recursive:true); }
    }
}
