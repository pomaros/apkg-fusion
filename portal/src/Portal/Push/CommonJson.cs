// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
using System.Security.Cryptography;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
namespace Portal.Push;

public static class CommonJson
{
    public static string Digest(ReadOnlySpan<byte> bytes)=>Convert.ToHexStringLower(SHA256.HashData(bytes));
    public static string Identity(JsonElement canonicalObject)=>Digest(Encoding.UTF8.GetBytes(canonicalObject.GetRawText()));
    public static byte[] Encode<T>(T value)
    {
        var element=JsonSerializer.SerializeToElement(value);
        using var buffer=new MemoryStream();
        using(var writer=new Utf8JsonWriter(buffer,new(){Encoder=JavaScriptEncoder.UnsafeRelaxedJsonEscaping}))Write(writer,element);
        return buffer.ToArray();
    }
    static void Write(Utf8JsonWriter writer,JsonElement value)
    {
        switch(value.ValueKind)
        {
            case JsonValueKind.Object:
                writer.WriteStartObject();
                foreach(var property in value.EnumerateObject().OrderBy(p=>p.Name,StringComparer.Ordinal))
                { writer.WritePropertyName(property.Name);Write(writer,property.Value); }
                writer.WriteEndObject();break;
            case JsonValueKind.Array:
                writer.WriteStartArray();foreach(var item in value.EnumerateArray())Write(writer,item);writer.WriteEndArray();break;
            default:value.WriteTo(writer);break;
        }
    }
}
