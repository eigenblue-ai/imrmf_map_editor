# ui — shared ImGui UI kit

The shared ImGui theme, widgets, and layout helpers. One place for the look and
the common building blocks, so the editor does not restyle ImGui in each module.

Depend on this instead of styling ImGui yourself: apply the theme, load the
bundled font, and use the widgets below for the recurring patterns (forms,
modals, toolbars, status display, canvas overlays, docked layouts).

Small on purpose. This is a thin layer on top of [Dear ImGui](https://github.com/ocornut/imgui),
not a framework.

## What's in it

| Target            | Header                  | What it gives you                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                       |
| ----------------- | ----------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `:theme`          | `ui/theme.hpp`          | The brand palette and `theme::apply()`, which paints an `ImGuiStyle` in the corporate colors. Call it once after `ImGui::CreateContext()`.                                                                                                                                                                                                                                                                                                                                                                                              |
| `:widgets`        | `ui/widgets.hpp`        | The 2-column form layout (`BeginFormTable` / `FormRow` / `FormControlWidth`) that keeps labels and controls aligned, `ButtonGroupSelector` (a segmented control), `BeginModal` / `ModalActions` for consistent brand-styled modals, `BeginOverlayCard` / `EndOverlayCard` for floating cards over a canvas, status helpers (`StatusText`, `StatusLine`), toolbar helpers (`ToolbarToggle`, `ToolbarSeparator`, `SameLineRight`), `DangerButton`, `ItemTooltip`, and `RefreshTimer` / `RefreshControls` for auto-refreshing data panels. |
| `:notifications`  | `ui/notifications.hpp`  | `Notify(theme::Signal, text)` from anywhere, `DrawNotifications()` once per frame: transient cards in the bottom right, on the foreground list so they sit over modals. `NotificationsHeight()` reserves room in a self-sizing host, `NotificationsHovered()` lets a canvas leave the click to the card. Needs `:icons`.                                                                                                                                                                                                                |
| `:font`           | `ui/font.hpp`           | `theme::load_default_font()`, loads the bundled Inter font with a fallback to ImGui's built-in font. Add `:fonts` to your binary's `data`.                                                                                                                                                                                                                                                                                                                                                                                              |
| `:core`           | —                       | The three above in one dep. ImGui only, no other third-party libraries.                                                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| `:icons`          | `ui/icons.hpp`          | Material Design Icons (Apache-2.0) merged into the atlas. `ICON_MDI_*` macros + `theme::load_icons()`. Needs ImGui built with `IMGUI_USE_WCHAR32` (MDI codepoints are above U+FFFF). Add `:font_mdi` to your binary's `data`.                                                                                                                                                                                                                                                                                                           |
| `:schema_ui`      | `ui/schema_ui.hpp`      | `render_schema_ui()`, builds a form straight from a JSON Schema using `x-ui-*` annotations. Opt-in because it pulls in nlohmann_json.                                                                                                                                                                                                                                                                                                                                                                                                   |
| `:image_utils`    | `ui/image_utils.hpp`    | `remove_white_background()`, an edge flood-fill for logo and icon textures.                                                                                                                                                                                                                                                                                                                                                                                                                                                             |
| `:stb_image`      | `ui/stb_image.h`        | Vendored stb_image / stb_image_write for loading textures.                                                                                                                                                                                                                                                                                                                                                                                                                                                                              |
| `:dockspace`      | `ui/dockspace.hpp`      | `BeginDockSpaceHost()` / `EndDockSpaceHost()`, the fullscreen host window + `DockSpace` boilerplate at the top of every docked tool's frame.                                                                                                                                                                                                                                                                                                                                                                                            |
| `:window_manager` | `ui/window_manager.hpp` | `ui::WindowManager` + `ui::WindowInterface`: registry of tool windows with `draw_all()`, a Window visibility menu, and `name=0\|1` visibility persistence.                                                                                                                                                                                                                                                                                                                                                                              |
| `:viewport`       | `ui/viewport.hpp`       | `ui::ViewState` + `view_world_to_screen` / `view_screen_to_world` / `handle_pan_zoom`: cursor-centered wheel zoom and middle-drag pan for a 2D world-space canvas.                                                                                                                                                                                                                                                                                                                                                                      |
| `:format`         | `ui/format.hpp`         | `ui::human_size`, `format_duration`, `format_time`, `format_timestamp_ms`, `short_id`. Value-to-string formatters, no ImGui dependency.                                                                                                                                                                                                                                                                                                                                                                                                 |

Namespaces today are `theme::`, `ImGuiWidgets::`, `ui::` (the non-widget
helpers), and the free function `render_schema_ui`. Unifying them under one
namespace is a pre-1.0 decision, not done yet so the kit stays a drop-in match
for the code it came from.

`theme::` also carries the semantic status layer: `theme::Signal`
(success/warning/danger/info/muted), `signal_color()`, and `signal_for(value,
warn_at, ok_at)` for threshold-colored metrics — use these instead of mapping
states to colors by hand.

`bazel test //...` runs a headless smoke test that pushes every widget through
a real ImGui frame.

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

Dear ImGui comes from `@imgui_docking//:imgui` (docking branch, commit
`92e2df5`, pinned in `MODULE.bazel`). `extensions/imgui_docking.BUILD.bazel` is
its build file, and it keeps `IMGUI_USE_WCHAR32` defined so the icon font works.

```starlark
# BUILD.bazel
cc_binary(
    name = "my_tool",
    srcs = ["main.cpp"],
    data = ["//ui:fonts"],
    deps = [
        "//ui:core",  # theme + widgets + font
        "@imgui_docking//:imgui",
    ],
)
```

```cpp
#include "ui/theme.hpp"
#include "ui/widgets.hpp"

ImGui::CreateContext();
theme::apply();                       // brand colors
theme::load_default_font();           // Inter, with fallback

if (ImGuiWidgets::BeginFormTable("##settings")) {
  ImGuiWidgets::FormRow("Update interval");
  ImGui::SetNextItemWidth(ImGuiWidgets::FormControlWidth());
  ImGui::SliderFloat("##interval", &interval, 1.0f, 60.0f, "%.1f s");
  ImGui::EndTable();
}
```

## How it ships

This is a private, internal repo. It is not published anywhere public.

Consumers vendor it into their own tree (a plain copy, or `git subtree` against
this repo so fixes flow both ways) rather than pull it as a public dependency.
The include prefix is stable no matter where it lands, so a target depends on
`//ui:core` (or wherever it was vendored) and the `#include`
lines never change.

## Font license

Inter ships under the SIL Open Font License. `ui/fonts/OFL.txt` travels
with it as the license requires.
