using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using Il2CppGasSim;
using MelonLoader.Utils;

namespace CombustorPng;

internal sealed class PngImage
{
    internal readonly int Width, Height;
    internal readonly uint[] Pixels;
    private PngImage(int width, int height, uint[] pixels) { Width = width; Height = height; Pixels = pixels; }
    [UnmanagedFunctionPointer(CallingConvention.Cdecl, CharSet = CharSet.Unicode)]
    private delegate IntPtr ReadPng(int kind, string path, StringBuilder error, int capacity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate IntPtr PngPixels(IntPtr image, out int width, out int height);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate void FreePng(IntPtr image);
    private static ReadPng read;
    private static PngPixels pixels;
    private static FreePng free;

    internal static void Initialize()
    {
        string path = Path.Combine(MelonEnvironment.GameRootDirectory, "UserData", "CombustorPng", "CombustorPngDecoder.dll");
        IntPtr dll = NativeLibrary.Load(path);
        read = Marshal.GetDelegateForFunctionPointer<ReadPng>(NativeLibrary.GetExport(dll, "ReadPng"));
        pixels = Marshal.GetDelegateForFunctionPointer<PngPixels>(NativeLibrary.GetExport(dll, "PngPixels"));
        free = Marshal.GetDelegateForFunctionPointer<FreePng>(NativeLibrary.GetExport(dll, "FreePng"));
    }

    internal static unsafe PngImage Read(int kind, string path, out string error)
    {
        var message = new StringBuilder(512);
        IntPtr image = read(kind, path, message, message.Capacity);
        error = message.ToString();
        if (image == IntPtr.Zero) return null;
        try
        {
            var data = pixels(image, out int width, out int height);
            var samples = new ReadOnlySpan<uint>((void*)data, checked(width * height));
            int left = width, top = height, right = 0, bottom = 0;
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++)
                    if (samples[y * width + x] >> 24 >= 128)
                    {
                        left = Math.Min(left, x);
                        top = Math.Min(top, y);
                        right = Math.Max(right, x + 1);
                        bottom = Math.Max(bottom, y + 1);
                    }
            if (left >= right)
            {
                error = "PNG has no opaque pixels to place.";
                return null;
            }
            int croppedWidth = right - left;
            int croppedHeight = bottom - top;
            var result = new uint[croppedWidth * croppedHeight];
            for (int y = 0; y < croppedHeight; y++)
                samples.Slice((y + top) * width + left, croppedWidth).CopyTo(result.AsSpan(y * croppedWidth));
            return new PngImage(croppedWidth, croppedHeight, result);
        }
        finally { free(image); }
    }
}

internal static class TintFiles
{
    private static readonly byte[] Magic = Encoding.ASCII.GetBytes("PNGCOLOR");

    private static byte[] Digest(string path)
    {
        using var stream = File.OpenRead(path);
        using var sha = SHA256.Create();
        return sha.ComputeHash(stream);
    }

    private static string Cache(byte[] digest) => Path.Combine(MelonEnvironment.GameRootDirectory, "UserData", "CombustorPng", "ColorCache", Convert.ToHexString(digest) + ".pngcolors");

    internal static void Save(string path, GasGrid grid, Dictionary<int, PngMod.Tint> art)
    {
        string adjacent = path + ".pngcolors";
        if (art.Count == 0 && !File.Exists(adjacent)) return;
        var solid = grid.Solid.AsSpan();
        var valid = new List<KeyValuePair<int, PngMod.Tint>>();
        foreach (var entry in art)
            if ((uint)entry.Key < solid.Length && entry.Value.Material == solid[entry.Key]) valid.Add(entry);
        byte[] digest = Digest(path);
        using var bytes = new MemoryStream(56 + valid.Count * 9);
        using var writer = new BinaryWriter(bytes);
        writer.Write(Magic);
        writer.Write(1);
        writer.Write(grid.Width);
        writer.Write(grid.Height);
        writer.Write(valid.Count);
        writer.Write(digest);
        foreach (var (index, tint) in valid)
        {
            writer.Write(index);
            writer.Write(tint.Color);
            writer.Write(tint.Material);
        }
        writer.Flush();
        byte[] contents = bytes.ToArray();
        Write(adjacent, contents);
        Write(Cache(digest), contents);
    }

    private static void Write(string target, byte[] bytes)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(target));
        string temp = target + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try { File.WriteAllBytes(temp, bytes); File.Move(temp, target, true); }
        finally { if (File.Exists(temp)) File.Delete(temp); }
    }

    internal static Dictionary<int, PngMod.Tint> Load(string path, GasGrid grid)
    {
        if (!File.Exists(path)) return new();
        byte[] digest = Digest(path);
        return Read(path + ".pngcolors", grid, digest) ?? Read(Cache(digest), grid, digest) ?? new();
    }

    private static Dictionary<int, PngMod.Tint> Read(string file, GasGrid grid, byte[] digest)
    {
        if (!File.Exists(file)) return null;
        using var reader = new BinaryReader(File.OpenRead(file));
        if (reader.BaseStream.Length < 56 || !reader.ReadBytes(8).SequenceEqual(Magic) || reader.ReadInt32() != 1) return null;
        if (reader.ReadInt32() != grid.Width || reader.ReadInt32() != grid.Height) return null;
        var solid = grid.Solid.AsSpan();
        int count = reader.ReadInt32();
        if (count < 0 || count > solid.Length || reader.BaseStream.Length != 56L + count * 9L || !reader.ReadBytes(32).SequenceEqual(digest)) return null;
        var result = new Dictionary<int, PngMod.Tint>(count);
        for (int i = 0; i < count; i++)
        {
            int index = reader.ReadInt32();
            uint color = reader.ReadUInt32();
            byte material = reader.ReadByte();
            if ((uint)index >= solid.Length || color >> 24 != 255 || material == 0 || result.ContainsKey(index)) return null;
            if (material == solid[index]) result[index] = new(color, material);
        }
        return result;
    }
}
