// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
using System.Text.RegularExpressions;
namespace Portal.Channels;
public static partial class ChannelPaths
{
    static readonly HashSet<string> Reserved = new(StringComparer.Ordinal)
        { "api", "channels", "builds", "account", "admin", "signin", "signout", "signin-github", "see",
          "docs", "publishers", "health", "robots", "css", "js", "error", "_admin", "_push" };
    [GeneratedRegex("^[a-z0-9][a-z0-9-]{0,39}\\z")] private static partial Regex Name();
    [GeneratedRegex("^snapshots/[0-9a-f]{64}/channel(\\.signature)?\\.json\\z")] private static partial Regex Snapshot();
    [GeneratedRegex("^archives/[0-9a-f]{64}\\.pkg\\z")] private static partial Regex Archive();
    public enum Kind { None, Current, Snapshot, Archive }
    public static bool IsChannelName(string name) => Name().IsMatch(name) && !Reserved.Contains(name);
    public static Kind Classify(string path) => path == "current" ? Kind.Current
        : Snapshot().IsMatch(path) ? Kind.Snapshot : Archive().IsMatch(path) ? Kind.Archive : Kind.None;
    public static bool Pushable(string path) => Classify(path) is Kind.Snapshot or Kind.Archive;
    public static bool Immutable(string path) => Pushable(path);
    public static string? PromisedDigest(string path) => Classify(path) == Kind.Archive ? path[9..73]
        : Classify(path) == Kind.Snapshot && path.EndsWith("/channel.json", StringComparison.Ordinal) ? path[10..74] : null;
    public static string ContentType(string path) => path.EndsWith(".json", StringComparison.Ordinal)
        ? "application/json" : path == "current" ? "text/plain; charset=utf-8" : "application/octet-stream";
}
