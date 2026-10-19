# IoT Commander — LVGL Editor UI

Target preview: 240 x 320 portrait.

Editor project configuration: LVGL 9.5.0.

## Open in LVGL Editor

Open this folder as the project:
- project.xml
- globals.xml
- screens/

## Important firmware compatibility note

The existing PlatformIO firmware currently declares LVGL 8.4.0.
This editor project is configured for LVGL 9.5.0.

These XML screens are a design/preview starting point. Do not copy
LVGL 9.5 generated C files into the LVGL 8.4 firmware and expect
them to compile unchanged.

Before firmware integration, either:
1. Port the generated UI to the existing LVGL 8.4 API, or
2. Plan and test a controlled firmware migration to LVGL 9.x.

The XML placeholder readings are not live sensor or backend values.
Screen layouts must be connected to the existing application services
before they represent actual device status.

## Generated code and firmware integration

The LVGL Pro Editor project is maintained at the repository root.
The editor preview has been verified, and all four screens are selectable.

### Generated screen entry points

- `Dashboard_create()` — `screens/Dashboard_gen.c`
- `Devices_create()` — `screens/Devices_gen.c`
- `Alerts_create()` — `screens/Alerts_gen.c`
- `Settings_create()` — `screens/Settings_gen.c`

The main generated project files are `IoTCommander_gen.c`,
`IoTCommander_gen.h`, `IoTCommander.c`, and `IoTCommander.h`.

### Version boundary

- Editor and generated UI target: LVGL 9.5.0.
- Existing ESP32 firmware dependency: LVGL 8.4.0.
- Target display: 240 x 320 portrait.

Generated UI files are not integrated into the PlatformIO firmware.
Do not copy them into `src/` or `include/`, and do not change the
firmware LVGL dependency solely to resolve generated-code incompatibilities.

### Before firmware integration

1. Review generated API calls and header dependencies against LVGL 8.4.0.
2. Choose a deliberate compatibility strategy.
3. Connect UI events and display values to existing application services.
4. Build the firmware and review compiler output before considering a flash.
5. Test on hardware only after a successful build and an explicit decision
   to proceed.

Placeholder values in the editor screens are design data, not live telemetry.
The preview build directories `preview-build/` and `preview-bin/` are
local generated artifacts and are excluded by the root `.gitignore`.

### Reopening the editor project

Open the repository root in LVGL Pro Editor using `project.xml`.
Keep `globals.xml`, `translations.xml`, and the `screens/` XML files
alongside it. Regenerate code from the editor when XML designs change.
Generated files may be overwritten during regeneration; keep custom logic
in separate application files.
