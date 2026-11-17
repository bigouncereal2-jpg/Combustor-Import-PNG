using System.Reflection;
using System.Runtime.InteropServices;
using HarmonyLib;
using Il2CppGasSim;
using Il2CppInterop.Runtime.InteropTypes.Arrays;
using MelonLoader;
using UnityEngine;

[assembly: MelonInfo(typeof(CombustorPng.PngMod), "Import PNG", "1.0.0", "theop")]
[assembly: MelonGame("DefaultCompany", "EngineSimulator")]

namespace CombustorPng;

public sealed class PngMod : MelonMod
{
    private static GasSimInput owner;
    private static GasSimInput readingKeys;
    private static Il2CppStructArray<int> clip;
    private static PngImage source;
    private static int width, height, lastWheelFrame = -1;
    private static float wheelCarry;
    private static uint[] colors;
    private static readonly Dictionary<IntPtr, Dictionary<int, Tint>> Tints = new();
    private static readonly Dictionary<IntPtr, List<Edit>> History = new();
    private static readonly HarmonyLib.Harmony Hooks = new("combustor.png-import");
    private static bool enabled;

    internal readonly struct Tint
    {
        internal readonly uint Color;
        internal readonly byte Material;
        internal Tint(uint color, byte material) { Color = color; Material = material; }
    }
    private readonly struct Edit
    {
        internal readonly int Index;
        internal readonly Tint Before, After;
        internal Edit(int index, Tint before, Tint after) { Index = index; Before = before; After = after; }
    }

    public override void OnInitializeMelon()
    {
        try
        {
            PngImage.Initialize();
            Patch(typeof(GasSimInput), "HandleClipboardKeys", nameof(Clipboard), null);
            Patch(typeof(GasSimInput), "HandleKeys", nameof(KeysStart), nameof(KeysEnd));
            Patch(typeof(SimInputAdapter), "get_ScrollDelta", null, nameof(Scroll));
            Patch(typeof(GasSimInput), "StampPaste", nameof(Stamp), null);
            Patch(typeof(GasSimRenderer), "DrawPasteGhost", nameof(Ghost), null);
            Patch(typeof(GasSimRenderer), "DrawGenerators", nameof(Paint), null);
            Patch(typeof(GasSimHud), "DrawPauseMenu", nameof(PauseMenu), null);
            Patch(typeof(GasSimulation), "SaveTo", null, nameof(Saved));
            Patch(typeof(GasSimulation), "LoadFrom", null, nameof(Loaded));
            Patch(typeof(GasGrid), "Clear", null, nameof(Cleared));
            Patch(typeof(GasGrid), "Dispose", nameof(Disposed), null);
            Patch(typeof(GasSimInput), "Undo", nameof(BeforeUndo), nameof(AfterUndo));
            Patch(typeof(GasSimInput), "Redo", nameof(BeforeRedo), nameof(AfterRedo));
            enabled = true;
            LoggerInstance.Msg("PNG importer ready.");
        }
        catch (Exception error)
        {
            Hooks.UnpatchSelf();
            LoggerInstance.Error(error.ToString());
        }
    }

    private static void Patch(Type type, string target, string prefix, string postfix)
    {
        var method = AccessTools.DeclaredMethod(type, target) ?? throw new MissingMethodException(type.FullName, target);
        HarmonyMethod Hook(string name) => name == null ? null : new HarmonyMethod(typeof(PngMod).GetMethod(name, BindingFlags.NonPublic | BindingFlags.Static));
        Hooks.Patch(method, Hook(prefix), Hook(postfix));
    }

    private static bool Owns(GasSimInput input) => input != null && owner != null && input.Pointer == owner.Pointer && clip != null && input._clip?.Pointer == clip.Pointer;
    private static bool Active(GasSimInput input) => Owns(input) && input.PastePending;

    private static bool Clipboard(GasSimInput __instance)
    {
        if (!enabled || !SimInputAdapter.CtrlHeld || !SimInputAdapter.KeyPressed(KeyCode.V)) return true;
        return !Import(__instance, 0);
    }

    private static void KeysStart(GasSimInput __instance) => readingKeys = __instance;
    private static void KeysEnd() => readingKeys = null;

    private static void Scroll(ref float __result)
    {
        var input = readingKeys;
        if (!Active(input) || __result == 0 || input.PointerBlocked) return;
        float delta = __result;
        __result = 0;
        if (lastWheelFrame == Time.frameCount) return;
        lastWheelFrame = Time.frameCount;
        if (delta * wheelCarry < 0) wheelCarry = 0;
        wheelCarry += delta;
        int steps = Math.Clamp((int)wheelCarry, -12, 12);
        if (steps == 0) return;
        wheelCarry -= steps;
        int requested = (int)Math.Round(width * Math.Pow(1.15, steps));
        Resize(input, requested == width ? width + Math.Sign(steps) : requested);
    }

    private static void PauseMenu(GasSimHud __instance)
    {
        if (!enabled || __instance._input == null || !__instance._input.Sandbox) return;
        if (!GUILayout.Button("Import PNG...", __instance._button, GUILayout.Height(28f))) return;
        if (Import(__instance._input, 1)) __instance.CloseMenu();
    }

    internal static bool Import(GasSimInput input, int kind, string path = null)
    {
        var grid = input?._sim?.Grid;
        if (grid == null || !input.ToolAllowed(Tool.Wall)) return false;
        try
        {
            var image = PngImage.Read(kind, path, out string error);
            if (image == null)
            {
                if (kind != 0 && !string.IsNullOrEmpty(error)) input._sim.LastSaveMessage = error;
                return false;
            }

            source = image;
            owner = input;
            wheelCarry = 0;
            double fit = Math.Min(1, Math.Min((double)grid.Width / image.Width, (double)grid.Height / image.Height));
            Resize(input, Math.Max(1, (int)Math.Round(image.Width * fit)));
            input._clipParts = null;
            input.PasteTurns = 0;
            input.PasteFlipX = false;
            input.PasteFlipY = false;
            input.BeginPaste();
            input._sim.LastSaveMessage = "PNG ready. Scroll to resize, click to place.";
            return input.PastePending;
        }
        catch (Exception error)
        {
            input._sim.LastSaveMessage = "PNG could not be imported.";
            MelonLogger.Error(error.ToString());
            return false;
        }
    }

    private static void Resize(GasSimInput input, int requested)
    {
        var grid = input._sim.Grid;
        int maxWidth = Math.Clamp(grid.Width * 2, 1, 2048);
        int maxHeight = Math.Clamp(grid.Height * 2, 1, 2048);
        width = Math.Clamp(requested, 1, maxWidth);
        height = Math.Max(1, (int)Math.Round((double)width * source.Height / source.Width));
        if (height > maxHeight)
        {
            height = maxHeight;
            width = Math.Clamp((int)Math.Round((double)height * source.Width / source.Height), 1, maxWidth);
        }

        clip = new Il2CppStructArray<int>(width * height);
        var materials = clip.AsSpan();
        colors = new uint[width * height];
        int selected = input.selectedWall;
        for (int y = 0; y < height; y++)
        {
            int sy = (height - 1 - y) * source.Height / height;
            for (int x = 0; x < width; x++)
            {
                int index = y * width + x;
                uint rgba = source.Pixels[sy * source.Width + x * source.Width / width];
                materials[index] = (rgba >> 24) >= 128 ? selected : -1;
                if (materials[index] >= 0) colors[index] = rgba | 0xff000000;
            }
        }
        input._clip = clip;
        input._clipW = width;
        input._clipH = height;
    }

    private static void Visit(GasSimInput input, GasGrid grid, int cx, int cy, Action<int, int, uint, int> visit)
    {
        var xs = input._smx;
        var ys = input._smy;
        int gridWidth = grid.Width, gridHeight = grid.Height;
        var materials = clip.AsSpan();
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
            {
                int index = y * width + x;
                uint color = colors[index];
                if (color == 0) continue;
                input.PasteTarget(x, y, cx, cy, out int tx, out int ty);
                int count = input.MirrorPoints(tx, ty, xs, ys);
                for (int i = 0; i < count; i++)
                {
                    int xx = xs[i], yy = ys[i];
                    if ((uint)xx < gridWidth && (uint)yy < gridHeight)
                        visit(xx, yy, color, materials[index]);
                }
            }
    }

    private static IntPtr Top(Il2CppSystem.Collections.Generic.List<GasSimInput.EditStep> list) => list == null || list.Count == 0 ? IntPtr.Zero : list[list.Count - 1].Pointer;

    private static bool Stamp(GasSimInput __instance, int __0, int __1)
    {
        var input = __instance;
        if (!Owns(input)) return true;
        var grid = input._sim.Grid;
        var art = GetArt(grid);
        var solid = grid.Solid;
        int gridWidth = grid.Width;
        var edits = new Dictionary<int, Edit>();
        input.CloseStroke();
        var previous = Top(input._undo);
        Visit(input, grid, __0, __1, (x, y, color, material) =>
        {
            if (!input.OffersWall(material) || !input.CanEdit(x, y)) return;
            int index = y * gridWidth + x;
            byte desired = checked((byte)(material + 1));
            byte old = solid[index];
            if (old != desired && input.wallBudget != null && !input.BudgetAllows(old - 1, false, material)) return;
            art.TryGetValue(index, out var before);
            if (before.Material != old) before = default;
            var after = new Tint(color, desired);
            if (before.Color == after.Color && before.Material == after.Material) return;
            input.RecordWall(grid, index);
            if (old != desired) grid.SetSolid(x, y, true, material);
            if (solid[index] != desired) return;
            art[index] = after;
            if (edits.TryGetValue(index, out var first)) before = first.Before;
            edits[index] = new Edit(index, before, after);
        });
        input.CloseStroke();
        var step = Top(input._undo);
        if (step != IntPtr.Zero && step != previous && edits.Count > 0)
        {
            History[step] = edits.Values.ToList();
            var retained = new HashSet<IntPtr>();
            for (int i = 0; i < input._undo.Count; i++) retained.Add(input._undo[i].Pointer);
            foreach (var outdated in History.Keys.Where(key => !retained.Contains(key)).ToArray()) History.Remove(outdated);
        }
        return false;
    }

    private static Dictionary<int, Tint> GetArt(GasGrid grid)
    {
        if (!Tints.TryGetValue(grid.Pointer, out var art)) Tints[grid.Pointer] = art = new();
        return art;
    }

    private static void DrawPixel(Il2CppStructArray<Color32> pixels, int index, uint rgba) => pixels.AsSpan()[index] = new Color32((byte)rgba, (byte)(rgba >> 8), (byte)(rgba >> 16), 255);

    private static void Paint(GasSimRenderer __instance, GasGrid __0, Il2CppStructArray<Color32> __1)
    {
        if ((int)__instance.view != 0 || !Tints.TryGetValue(__0.Pointer, out var art)) return;
        var solid = __0.Solid.AsSpan();
        var pixels = __1.AsSpan();
        foreach (var (index, tint) in art)
        {
            if ((uint)index >= solid.Length || (uint)index >= pixels.Length || solid[index] != tint.Material) continue;
            uint color = tint.Color;
            pixels[index] = new Color32((byte)color, (byte)(color >> 8), (byte)(color >> 16), 255);
        }
    }

    private static bool Ghost(GasSimRenderer __instance, GasGrid __0, Il2CppStructArray<Color32> __1)
    {
        var input = __instance._input;
        if (!Active(input)) return true;
        if (input.HoverValid && !input.PointerBlocked)
        {
            int gridWidth = __0.Width;
            Visit(input, __0, input.HoverX, input.HoverY, (x, y, rgba, _) => DrawPixel(__1, y * gridWidth + x, rgba));
        }
        return false;
    }

    private static void BeforeUndo(GasSimInput __instance, out IntPtr __state) => __state = Top(__instance._undo);
    private static void BeforeRedo(GasSimInput __instance, out IntPtr __state) => __state = Top(__instance._redo);
    private static void AfterUndo(GasSimInput __instance, IntPtr __state) => ApplyHistory(__instance, __state, false);
    private static void AfterRedo(GasSimInput __instance, IntPtr __state) => ApplyHistory(__instance, __state, true);

    private static void ApplyHistory(GasSimInput input, IntPtr oldStep, bool redo)
    {
        if (oldStep == IntPtr.Zero || !History.TryGetValue(oldStep, out var edits)) return;
        var oldStack = redo ? input._redo : input._undo;
        if (Top(oldStack) == oldStep) return;
        History.Remove(oldStep);
        var target = Top(redo ? input._undo : input._redo);
        if (target != IntPtr.Zero) History[target] = edits;
        var art = GetArt(input._sim.Grid);
        foreach (var edit in edits)
        {
            var tint = redo ? edit.After : edit.Before;
            if (tint.Color == 0) art.Remove(edit.Index);
            else art[edit.Index] = tint;
        }
    }

    private static void Saved(GasSimulation __instance, string __0, string __1, bool __result)
    {
        if (!__result || __instance.Grid == null) return;
        try { TintFiles.Save(EnginePath(__0, __1), __instance.Grid, GetArt(__instance.Grid)); }
        catch (Exception error) { MelonLogger.Warning("PNG colors could not be saved: " + error.Message); }
    }

    private static void Loaded(GasSimulation __instance, string __0, string __1, bool __result)
    {
        if (!__result || __instance.Grid == null) return;
        var grid = __instance.Grid;
        Tints.Remove(grid.Pointer);
        History.Clear();
        try { Tints[grid.Pointer] = TintFiles.Load(EnginePath(__0, __1), grid); }
        catch (Exception error) { MelonLogger.Warning("PNG colors could not be loaded: " + error.Message); }
    }

    private static string EnginePath(string directory, string name) => Path.Combine(directory, GasSimulation.SanitizeName(name) + ".engine");
    private static void Cleared(GasGrid __instance, bool __0) { if (__0) Disposed(__instance); }
    private static void Disposed(GasGrid __instance)
    {
        Tints.Remove(__instance.Pointer);
        History.Clear();
        if (owner == null || owner._sim == null || owner._sim.Grid?.Pointer != __instance.Pointer) return;
        owner = null;
        clip = null;
        source = null;
        colors = null;
    }
}
