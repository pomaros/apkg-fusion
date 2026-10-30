// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
using System.Security.Cryptography;
using System.Text;
using Microsoft.Extensions.Options;
namespace Portal.Channels;

/// Directory discovery and visibility only; signed facts belong to CommonChannels.
public sealed class CommonVisibility(IOptions<PortalOptions> options, IHttpContextAccessor? web = null)
{
    readonly PortalOptions o = options.Value;
    public const string ViewCookie = "apkg-view";
    public IEnumerable<string> ChannelNames() => Directory.Exists(o.ChannelsDir)
        ? Directory.EnumerateDirectories(o.ChannelsDir).Select(Path.GetFileName).OfType<string>()
            .Where(ChannelPaths.IsChannelName).Order(StringComparer.Ordinal) : [];
    public bool Exists(string channel) => ChannelPaths.IsChannelName(channel)
        && Directory.Exists(Path.Combine(o.ChannelsDir, channel));
    public bool HasPublication(string channel) => Exists(channel)
        && (File.Exists(Path.Combine(o.ChannelsDir, channel, "current"))
            || File.Exists(Path.Combine(o.ChannelsDir, channel, ".portal-owner.json")));
    public string UnlistedPath(string channel) => Path.Combine(o.StateDir, channel, "unlisted");
    public bool IsUnlisted(string channel) => o.Unlisted.Split(';', ',').Any(value => value.Trim() == channel)
        || File.Exists(UnlistedPath(channel));
    public bool SeesUnlisted() => web?.HttpContext?.Request.Cookies[ViewCookie] is { } key && IsViewKey(key);
    public bool IsViewKey(string key)
    {
        var digest = SHA256.HashData(Encoding.UTF8.GetBytes(key));
        return o.ViewKeys.Split(';', StringSplitOptions.RemoveEmptyEntries).Any(entry =>
            entry.Split(':') is [_, { Length: 64 } hash] && hash.All(Uri.IsHexDigit)
            && CryptographicOperations.FixedTimeEquals(Convert.FromHexString(hash), digest));
    }
    public void SetListed(string channel, bool listed)
    {
        if (!ChannelPaths.IsChannelName(channel)) throw new ArgumentException("Invalid channel");
        var marker = UnlistedPath(channel);
        if (listed) File.Delete(marker);
        else { Directory.CreateDirectory(Path.GetDirectoryName(marker)!); File.WriteAllText(marker, "unlisted\n"); }
    }
}
