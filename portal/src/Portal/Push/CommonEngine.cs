// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Diagnostics;
using System.Text;
using System.Text.Json;
using Microsoft.Extensions.Options;

namespace Portal.Push;

/// Cryptography and contract validation belong to the common engine. No legacy fallback.
public sealed class CommonEngine(IOptions<PortalOptions> options)
{
    public Task<JsonElement> Verify(string domain, string document, string signature,
                                   CancellationToken ct = default) =>
        Run(options.Value.ApkgPath, ["verify-object", domain, document, signature], ct);

    public async Task<JsonElement> Run(string executable, IEnumerable<string> arguments,
                                      CancellationToken ct = default,int outputLimit=65536)
    {
        using var json=JsonDocument.Parse(await RunText(executable,arguments,ct,outputLimit));
        return json.RootElement.Clone();
    }

    public async Task<string> RunText(string executable,IEnumerable<string> arguments,
                                     CancellationToken ct=default,int outputLimit=65536)
    {
        var start = new ProcessStartInfo(executable)
        {
            UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true,
        };
        foreach (var arg in arguments) start.ArgumentList.Add(arg);
        using var process = Process.Start(start) ?? throw new IOException("common verifier did not start");
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(ct);
        stop.CancelAfter(options.Value.CheckTimeout);
        async Task<string> Read(StreamReader reader,int limit)
        {
            try
            {
                var result = new StringBuilder();
                var buffer = new char[4096];
                int count;
                while ((count = await reader.ReadAsync(buffer.AsMemory(), stop.Token)) != 0)
                {
                    if (result.Length + count > limit) throw new InvalidDataException("verifier output exceeds bound");
                    result.Append(buffer, 0, count);
                }
                return result.ToString();
            }
            catch { stop.Cancel(); throw; }
        }
        var stdout = Read(process.StandardOutput,outputLimit);
        var stderr = Read(process.StandardError,65536);
        try
        {
            await Task.WhenAll(stdout, stderr, process.WaitForExitAsync(stop.Token));
            if (process.ExitCode != 0) throw new InvalidDataException("Common engine refused (status "+process.ExitCode+"): "+await stderr);
            return await stdout;
        }
        finally
        {
            if (!process.HasExited)
            {
                try { process.Kill(entireProcessTree: true); }
                catch (InvalidOperationException) { /* Exited between observation and kill. */ }
                await process.WaitForExitAsync(CancellationToken.None);
            }
        }
    }
}
