// SPDX-License-Identifier: MIT
// Copyright (c) 2026 John Knipper
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Microsoft.Win32.SafeHandles;
namespace Portal.Push;

/// POSIX publication primitives shared with cooperative file-channel writers.
public class CommonPublicationFiles
{
    [DllImport("libc",SetLastError=true)]static extern int open([MarshalAs(UnmanagedType.LPUTF8Str)]string path,int flags);
    [DllImport("libc",SetLastError=true)]static extern int fsync(SafeFileHandle descriptor);
    [DllImport("libc",SetLastError=true)]static extern int fstat(SafeFileHandle descriptor,[Out]byte[] information);
    [DllImport("libc",SetLastError=true)]static extern int flock(SafeFileHandle descriptor,int operation);
    [DllImport("libc",SetLastError=true)]static extern int link([MarshalAs(UnmanagedType.LPUTF8Str)]string source,[MarshalAs(UnmanagedType.LPUTF8Str)]string destination);
    static bool Mac=>OperatingSystem.IsMacOS();
    static int NoFollow=>Mac?0x100:0x20000;
    static int NonBlock=>Mac?4:0x800;
    static int CloseExec=>Mac?0x1000000:0x80000;
    static void SetMode(SafeFileHandle file)
    {
        if(OperatingSystem.IsWindows())throw new PlatformNotSupportedException("POSIX publication required");
        File.SetUnixFileMode(file,UnixFileMode.UserRead|UnixFileMode.UserWrite|UnixFileMode.GroupRead|UnixFileMode.OtherRead);
    }
    static IOException Error(string operation)=>new(operation+" failed (errno "+Marshal.GetLastPInvokeError()+")");
    static SafeFileHandle Open(string path,int flags)
    {
        if(!Mac && !OperatingSystem.IsLinux())throw new PlatformNotSupportedException("POSIX publication required");
        var descriptor=open(path,flags|NoFollow|NonBlock|CloseExec);
        if(descriptor<0)
        {
            if(Marshal.GetLastPInvokeError()==2)throw new FileNotFoundException("Publication member missing",path);
            throw Error("open");
        }
        return new SafeFileHandle((IntPtr)descriptor,ownsHandle:true);
    }
    static (uint Mode,ulong Links) Properties(SafeFileHandle descriptor)
    {
        var value=new byte[256];if(fstat(descriptor,value)!=0)throw Error("stat");
        if(Mac)return (BitConverter.ToUInt16(value,4),BitConverter.ToUInt16(value,6));
        return RuntimeInformation.ProcessArchitecture switch
        {
            Architecture.X64=>(BitConverter.ToUInt32(value,24),BitConverter.ToUInt64(value,16)),
            Architecture.Arm64=>(BitConverter.ToUInt32(value,16),BitConverter.ToUInt32(value,20)),
            _=>throw new PlatformNotSupportedException("Unqualified POSIX stat ABI")
        };
    }
    public virtual void NoLinks(string path,string root)
    {
        root=Path.TrimEndingDirectorySeparator(Path.GetFullPath(root));path=Path.GetFullPath(path);
        if(path!=root && !path.StartsWith(root+Path.DirectorySeparatorChar,StringComparison.Ordinal))throw new InvalidDataException("Path escapes publication tree");
        for(string? member=path;member is not null;member=Path.GetDirectoryName(member))
        {
            var attributes=File.GetAttributes(member);
            if((attributes&FileAttributes.ReparsePoint)!=0)throw new InvalidDataException("Linked publication member");
            if(member==root)break;
        }
    }
    public virtual FileStream Read(string path,string root)
    {
        NoLinks(path,root);var handle=Open(path,0);
        try
        {
            if((Properties(handle).Mode&0xf000)!=0x8000)throw new InvalidDataException("Ordinary publication file required");
            return new FileStream(handle,FileAccess.Read);
        }
        catch { handle.Dispose();throw; }
    }
    public virtual async Task<byte[]> ReadBounded(string path,int maximum,string root,CancellationToken ct)
    {
        await using var file=Read(path,root);
        if(file.Length>maximum)throw new InvalidDataException("Publication metadata exceeds bound");
        using var result=new MemoryStream();var bytes=new byte[8192];int count;
        while((count=await file.ReadAsync(bytes,ct))>0)
        { if(result.Length+count>maximum)throw new InvalidDataException("Publication metadata exceeds bound");await result.WriteAsync(bytes.AsMemory(0,count),ct); }
        return result.ToArray();
    }
    public virtual void SyncDirectory(string path)
    {
        using var handle=Open(path,0);
        if((Properties(handle).Mode&0xf000)!=0x4000)throw new InvalidDataException("Publication directory required");
        if(fsync(handle)!=0)throw Error("directory flush");
    }
    public virtual void DirectoryReady(string path,string root)
    {
        path=Path.GetFullPath(path);root=Path.GetFullPath(root);
        if(path!=root && Path.GetDirectoryName(path) is { } ancestor)
        {
            if(!Directory.Exists(ancestor))DirectoryReady(ancestor,root);
            NoLinks(ancestor,root);
        }
        Directory.CreateDirectory(path);NoLinks(path,root);SyncDirectory(path);
        if(Path.GetDirectoryName(path) is { } parent)SyncDirectory(parent);
    }
    static FileStream CreateLock(string path)
    {
        if(OperatingSystem.IsWindows())throw new PlatformNotSupportedException("POSIX publication required");
        return new FileStream(path,new FileStreamOptions{Mode=FileMode.CreateNew,Access=FileAccess.ReadWrite,
            Share=FileShare.ReadWrite|FileShare.Delete,UnixCreateMode=UnixFileMode.UserRead|UnixFileMode.UserWrite});
    }
    public virtual async Task<IDisposable> Lock(string endpoint,string root,CancellationToken ct)
    {
        if(OperatingSystem.IsWindows())throw new PlatformNotSupportedException("POSIX publication required");
        var path=Path.Combine(endpoint,".publish.lock");NoLinks(endpoint,root);
        try{NoLinks(path,root);}
        catch(FileNotFoundException)
        {
            try
            {
                using var created=CreateLock(path);
            }
            catch(IOException) when(File.Exists(path)){}
        }
        var handle=Open(path,2);
        try
        {
            var info=Properties(handle);
            if((info.Mode&0xf000)!=0x8000 || info.Links!=1)throw new InvalidDataException("Invalid publication lock");
            while(flock(handle,2|4)!=0)
            {
                var error=Marshal.GetLastPInvokeError();
                if(error!=11 && error!=35 && error!=4)throw Error("publication lock");
                await Task.Delay(20,ct);
            }
            return handle;
        }
        catch {handle.Dispose();throw;}
    }
    public virtual async Task<string?> Current(string endpoint,string root,CancellationToken ct)
    {
        try
        {
            var data=await ReadBounded(Path.Combine(endpoint,"current"),65,root,ct);
            if(data.Length!=65 || data[64]!=10 || data.AsSpan(0,64).IndexOfAnyExcept("0123456789abcdef"u8)>=0)throw new InvalidDataException("Invalid current reference");
            return Encoding.ASCII.GetString(data,0,64);
        }
        catch(FileNotFoundException){return null;}
    }
    public virtual async Task<(string Digest,long Size)> Hash(string path,string root,CancellationToken ct)
    {
        await using var stream=Read(path,root);
        if(stream.Length>uint.MaxValue)throw new InvalidDataException("Archive exceeds PKG/1 envelope bound");
        var digest=await SHA256.HashDataAsync(stream,ct);
        return (Convert.ToHexStringLower(digest),stream.Position);
    }
    public virtual async Task ImmutableBytes(string path,byte[] bytes,string root,CancellationToken ct)
    {
        if(File.Exists(path))
        {
            if(!(await ReadBounded(path,bytes.Length,root,ct)).AsSpan().SequenceEqual(bytes))throw new InvalidDataException("Immutable publication metadata differs");
            using var original=Read(path,root);if(fsync(original.SafeFileHandle)!=0)throw Error("immutable file flush");SyncDirectory(Path.GetDirectoryName(path)!);return;
        }
        await WriteImmutable(path,root,async output=>await output.WriteAsync(bytes,ct),ct);
    }
    public virtual async Task ImmutableArchive(string path,string source,string root,string sourceRoot,(string Digest,long Size) expected,CancellationToken ct)
    {
        if(File.Exists(path))
        {
            if(await Hash(path,root,ct)!=expected)throw new InvalidDataException("Existing immutable archive differs");
            using var original=Read(path,root);if(fsync(original.SafeFileHandle)!=0)throw Error("immutable file flush");SyncDirectory(Path.GetDirectoryName(path)!);return;
        }
        await WriteImmutable(path,root,async output=>
        {
            await using var incoming=Read(source,sourceRoot);
            using var hash=IncrementalHash.CreateHash(HashAlgorithmName.SHA256);var buffer=new byte[65536];long size=0;int count;
            while((count=await incoming.ReadAsync(buffer,ct))>0)
            {
                size+=count;if(size>expected.Size)throw new InvalidDataException("Archive grew during publication");
                hash.AppendData(buffer,0,count);await output.WriteAsync(buffer.AsMemory(0,count),ct);
            }
            if((Convert.ToHexStringLower(hash.GetHashAndReset()),size)!=expected)throw new InvalidDataException("Archive changed after validation");
        },ct);
    }
    async Task WriteImmutable(string path,string root,Func<FileStream,Task> write,CancellationToken ct)
    {
        var parent=Path.GetDirectoryName(path)!;NoLinks(parent,root);
        var temporary=Path.Combine(parent,".incoming-"+Guid.NewGuid().ToString("N"));
        try
        {
            await using(var output=new FileStream(temporary,FileMode.CreateNew,FileAccess.ReadWrite,FileShare.None))
            {await write(output);SetMode(output.SafeFileHandle);output.Flush(flushToDisk:true);}
            ct.ThrowIfCancellationRequested();
            if(link(temporary,path)!=0)throw Error("immutable publication");
            SyncDirectory(parent);
        }
        finally{File.Delete(temporary);}
    }
    public virtual async Task ReplaceCurrent(string endpoint,string? previous,string digest,string root,CancellationToken ct)
    {
        var temporary=Path.Combine(endpoint,".current-"+Guid.NewGuid().ToString("N"));
        try
        {
            await using(var stream=new FileStream(temporary,FileMode.CreateNew,FileAccess.Write,FileShare.None))
            {await stream.WriteAsync(Encoding.ASCII.GetBytes(digest+"\n"),ct);SetMode(stream.SafeFileHandle);stream.Flush(flushToDisk:true);}
            if(await Current(endpoint,root,ct)!=previous)throw new InvalidDataException("Current reference changed during publication");
            ct.ThrowIfCancellationRequested();File.Move(temporary,Path.Combine(endpoint,"current"),overwrite:true);SyncDirectory(endpoint);
        }
        finally{File.Delete(temporary);}
    }
}
