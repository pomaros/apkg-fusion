// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
namespace Portal;
public sealed class PortalOptions
{
    public string DataDir { get; set; } = "data";
    public string ApkgPath { get; set; } = "apkg";
    public string Keys { get; set; } = "";
    public string SignedKeys { get; set; } = "";
    public string AdminKeys { get; set; } = "";
    public string Admins { get; set; } = "";
    public string Unlisted { get; set; } = "";
    public string ViewKeys { get; set; } = "";
    public string PublicUrl { get; set; } = "";
    public bool ShowMigrationNotice { get; set; } = true;
    public const string MigrationNotice = "This portal and the package management system are being merged with APKG. Please do not use this service or its packages until further notice. We're sorry for the inconvenience.";
    public GitHubOptions GitHub { get; set; } = new();
    public PortalPolicy Policy { get; set; } = new();
    public bool AllowLoopbackHttpPush { get; set; }
    public long MaxPartBytes { get; set; } = 40L * 1024 * 1024;
    public long MaxStagingBytes { get; set; } = 2L * 1024 * 1024 * 1024;
    public long MaxLinkOnlyStagingBytes { get; set; } = 16L * 1024 * 1024;
    public long MaxCommonLinkBytes { get; set; } = 128L * 1024 * 1024;
    public long MaxCommonValidationBytes { get; set; } = 512L * 1024 * 1024;
    public TimeSpan CheckTimeout { get; set; } = TimeSpan.FromSeconds(150);
    public TimeSpan StagingLifetime { get; set; } = TimeSpan.FromHours(24);
    public string QualificationPolicyPath { get; set; } = "";
    public string ChannelsDir => Path.Combine(DataDir, "channels");
    public string StagingDir => Path.Combine(DataDir, "staging");
    public string StateDir => Path.Combine(DataDir, "state");
}
public sealed class GitHubOptions
{
    public string ClientId { get; set; } = "";
    public string ClientSecret { get; set; } = "";
    public bool On => ClientId.Length > 0 && ClientSecret.Length > 0;
}
