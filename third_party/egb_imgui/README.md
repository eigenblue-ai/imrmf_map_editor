# egb_imgui — shared ImGui UI kit

Eigenblue's shared ImGui theme, widgets, and form layout helpers for internal
GUI tools. It is the one place the brand look and the common UI building blocks
live, so every tool (the simulation runner, the storage explorer, the map
editor) reads as one product instead of each reinventing its own colors and
controls.

Small on purpose. This is a thin layer on top of [Dear ImGui](https://github.com/ocornut/imgui),
not a framework.

## What's in it

| Target         | Header                      | What it gives you                                                                                                                                                                                                      |
| -------------- | --------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `:theme`       | `egb_imgui/theme.hpp`       | The brand palette and `theme::apply()`, which paints an `ImGuiStyle` in the corporate colors. Call it once after `ImGui::CreateContext()`.                                                                             |
| `:widgets`     | `egb_imgui/widgets.hpp`     | The 2-column form layout (`BeginFormTable` / `FormRow` / `FormControlWidth`) that keeps labels and controls aligned and stops controls overflowing on a narrow panel, plus `ButtonGroupSelector`, a segmented control. |
| `:font`        | `egb_imgui/font.hpp`        | `theme::load_default_font()`, loads the bundled Inter font with a fallback to ImGui's built-in font. Add `:fonts` to your binary's `data`.                                                                             |
| `:core`        | —                           | The three above in one dep. ImGui only, no other third-party libraries.                                                                                                                                                |
| `:schema_ui`   | `egb_imgui/schema_ui.hpp`   | `render_schema_ui()`, builds a form straight from a JSON Schema using `x-ui-*` annotations. Opt-in because it pulls in nlohmann_json.                                                                                  |
| `:image_utils` | `egb_imgui/image_utils.hpp` | `remove_white_background()`, an edge flood-fill for logo and icon textures.                                                                                                                                            |
| `:stb_image`   | `egb_imgui/stb_image.h`     | Vendored stb_image / stb_image_write for loading textures.                                                                                                                                                             |

Namespaces today are `theme::`, `ImGuiWidgets::`, and the free function
`render_schema_ui`. Unifying them under one namespace is a pre-1.0 decision, not
done yet so the kit stays a drop-in match for the code it came from.

## Corporate identity colors

Everything routes through `theme::palette` so a rebrand is a one-file edit. Do
not hardcode `ImVec4` literals in a tool, pull the role you need from the
palette.

Built from the four brand colors:

| Role   | Hex       | Used for                                               |
| ------ | --------- | ------------------------------------------------------ |
| blue   | `#5581B0` | secondary interactive: buttons, sliders, headers, tabs |
| green  | `#53684C` | tertiary, calm accents                                 |
| plum   | `#3F2F3A` | deep neutral                                           |
| accent | `#E7FF00` | primary accent, kept for focus and selection only      |

Surfaces and text (dark mode):

| Role    | Hex       | Used for                    |
| ------- | --------- | --------------------------- |
| bg      | `#020617` | window background           |
| surface | `#0F172A` | panels, inputs, popups      |
| border  | `#292E42` | separators, frame borders   |
| text    | `#EDE6EA` | body text                   |
| muted   | `#9E86A6` | disabled and secondary text |

Status signals, high-chroma so they read against the dark background and stay
clear of the accent:

| Role    | Hex       | Meaning           |
| ------- | --------- | ----------------- |
| success | `#2BE06B` | ok, available     |
| warning | `#FFB020` | pending, caution  |
| danger  | `#FF453A` | error, stop       |
| info    | `#40B5FF` | queued, highlight |

Two rules that keep the look consistent:

- The accent (`#E7FF00`) is reserved for selection and focus. Do not use it for
  a normal button or a pressed state.
- Controls stay one hue and ramp by brightness on hover and press (dim, bright,
  full). They never flip color when pressed.

## Using it

egb_imgui expects the consumer to provide Dear ImGui as `@imgui_docking//:imgui`
(docking branch, commit `92e2df5`). Every eigenblue tool already pins that exact
commit, so vendored source compiles as-is.

```starlark
# BUILD.bazel
cc_binary(
    name = "my_tool",
    srcs = ["main.cpp"],
    data = ["@egb_imgui//:fonts"],   # or //third_party/egb_imgui:fonts when vendored
    deps = [
        "@egb_imgui//:core",         # theme + widgets + font
        "@imgui_docking//:imgui",
    ],
)
```

```cpp
#include "egb_imgui/theme.hpp"
#include "egb_imgui/widgets.hpp"

ImGui::CreateContext();
theme::apply();                       // brand colors
theme::load_default_font();           // Inter, with fallback

if (ImGuiWidgets::BeginFormTable("##settings")) {
  ImGuiWidgets::FormRow("Takt time");
  ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
  ImGui::SliderFloat("##takt", &takt, 1.0f, 60.0f, "%.1f s");
  ImGui::EndTable();
}
```

## How it ships

This is a private, internal repo. It is not published anywhere public.

Consumers vendor it into their own tree (a plain copy, or `git subtree` against
this repo so fixes flow both ways) rather than pull it as a public dependency.
The include prefix is stable no matter where it lands, so a target depends on
`//third_party/egb_imgui:core` (or wherever it was vendored) and the `#include`
lines never change.

## Font license

Inter ships under the SIL Open Font License. `egb_imgui/fonts/OFL.txt` travels
with it as the license requires.
