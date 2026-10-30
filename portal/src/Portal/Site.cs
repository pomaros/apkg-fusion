// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
namespace Portal;
public static class Site
{
    public static string ChannelUrl(HttpRequest request, PortalOptions options, string channel) =>
        (options.PublicUrl.Length == 0 ? $"{request.Scheme}://{request.Host}" : options.PublicUrl.TrimEnd('/')) + "/" + channel;
}
