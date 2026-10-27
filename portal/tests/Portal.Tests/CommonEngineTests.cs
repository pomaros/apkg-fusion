// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper

using System.Diagnostics;
using Microsoft.Extensions.Options;
using Portal.Push;

namespace Portal.Tests;

public sealed class CommonEngineTests
{
    [Theory]
    [InlineData("stdout")]
    [InlineData("stderr")]
    [InlineData("timeout")]
    public async Task Verifier_limits_stop_the_process_instead_of_hanging_or_accepting_output(string failure)
    {
        var directory = Directory.CreateTempSubdirectory("portal-engine-");
        try
        {
            if (OperatingSystem.IsWindows()) throw new PlatformNotSupportedException("This process fixture requires a POSIX host");
            var script = Path.Combine(directory.FullName, "verifier");
            var pid = Path.Combine(directory.FullName, "pid");
            File.WriteAllText(script, "#!/usr/bin/env python3\nimport os,sys,time\n" +
                "open(os.path.join(os.path.dirname(__file__),'pid'),'w').write(str(os.getpid()))\n" +
                (failure == "timeout" ? "time.sleep(60)\n" :
                 $"stream=sys.{failure}\nstream.write('x'*70000)\nstream.flush()\ntime.sleep(60)\n"));
            File.SetUnixFileMode(script, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
            var engine = new CommonEngine(Options.Create(new PortalOptions { ApkgPath = script, CheckTimeout = TimeSpan.FromSeconds(2) }));
            var elapsed = Stopwatch.StartNew();
            var error = await Xunit.Record.ExceptionAsync(() => engine.Verify("channel", "unused", "unused"));
            Assert.NotNull(error);
            if (failure == "timeout") Assert.IsAssignableFrom<OperationCanceledException>(error);
            else Assert.IsType<InvalidDataException>(error);
            Assert.True(elapsed.Elapsed < TimeSpan.FromSeconds(15));
            var processId = int.Parse(File.ReadAllText(pid));
            try
            {
                using var process = Process.GetProcessById(processId);
                Assert.True(process.HasExited, "verifier survived cancellation");
            }
            catch (ArgumentException) { /* Process has been reaped. */ }
        }
        finally { directory.Delete(recursive: true); }
    }
}
