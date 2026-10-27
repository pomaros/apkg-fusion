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
    public async Task<JsonElement> Verify(string domain, string document, string signature,
                                         CancellationToken ct = default)
    {
        var start = new ProcessStartInfo(options.Value.ApkgPath)
        {
            UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true,
        };
        foreach (var arg in new[] { "verify-object", domain, document, signature }) start.ArgumentList.Add(arg);
        using var process = Process.Start(start) ?? throw new IOException("common verifier did not start");
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(ct);
        stop.CancelAfter(options.Value.CheckTimeout);
        async Task<string> Read(StreamReader reader)
        {
            try
            {
                var result = new StringBuilder();
                var buffer = new char[4096];
                int count;
                while ((count = await reader.ReadAsync(buffer.AsMemory(), stop.Token)) != 0)
                {
                    if (result.Length + count > 65536) throw new InvalidDataException("verifier output exceeds bound");
                    result.Append(buffer, 0, count);
                }
                return result.ToString();
            }
            catch { stop.Cancel(); throw; }
        }
        var stdout = Read(process.StandardOutput);
        var stderr = Read(process.StandardError);
        try
        {
            await Task.WhenAll(stdout, stderr, process.WaitForExitAsync(stop.Token));
            if (process.ExitCode != 0) throw new InvalidDataException("common channel verification refused");
            using var json = JsonDocument.Parse(await stdout);
            return json.RootElement.Clone();
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
