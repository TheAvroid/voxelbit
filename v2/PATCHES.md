# The Falcor fork, and what was changed in it

`v2/external/` is not committed — it is 678 MB, and 18 directories under
`external/falcor/external/` are NTFS junctions into `C:\packman-repo` rather
than real trees. **`external/falcor` is not a git repository of its own**, so
there is no diff to take against upstream Falcor 8.0 and no patch file to
apply. This document is the record instead.

Without these changes v2 does not build. Rebuilding the fork from a clean
Falcor 8.0 checkout means re-applying all six by hand.

## 1. `ShaderModel::SM6_10` — `Source/Falcor/Core/API/Types.h:51-75`

The enum stops at SM6_9 upstream. Cooperative vectors need 6.10.

The trap: the enum's values are `major*10 + minor`, so the obvious next value
is 70 — and 70 is *SM7_0*. SM6_10 must be **610**, and the `/100` and `%100`
accessors alongside it exist because of that. Registered in the feature table
at `Core/API/Device.cpp:409` (`"sm_6_10"`), which slang-gfx reports as a
feature string.

## 2. `Gui::setSliderWidth()` — `Utils/UI/Gui.h:100`, `Gui.cpp:891-894, 972`

Falcor hardcodes `PushItemWidth(200)` for every slider. The settings menu needs
longer ones.

The trap: the call to patch is in `addScalarSliderHelper`, **not**
`addScalarVarHelper` — the two look nearly identical and patching the wrong one
changes nothing visible.

## 3. `Device::Desc::existingVulkanHandles` — `Core/API/Device.h:178`

Lets v2 hand slang-gfx a VkInstance/VkPhysicalDevice/VkDevice it already made,
which is what the CUDA and cooperative-vector paths need.

## 4. `experimentalFeaturesDesc` pushed only when non-empty — `Core/API/Device.cpp:502-504`

Passing a zero-length experimental-features list to D3D12 fails device creation
outright rather than being ignored.

## 5. `gfxEnableDebugLayer(true)`

Left on deliberately; the validation output is how several of the bugs above
were found.

## 6. `/wd4996`

Falcor 8.0 does not compile clean against the MSVC that ships with VS2026
without it.

## Slang

The build uses a **local Slang 2026.13.1**, not the packman one, via
`-DFALCOR_LOCAL_SLANG=ON`, `-DFALCOR_LOCAL_SLANG_DIR`, and
`-DFALCOR_LOCAL_SLANG_BUILD_DIR:STRING=.`

The trap: that last one must be typed `STRING`. CMake absolutises anything
typed `PATH`, which turns `.` into the build directory and mangles the lookup.
