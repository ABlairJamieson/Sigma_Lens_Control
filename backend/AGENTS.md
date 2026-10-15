# Firmware release policy

The user requires a new branch/version for each future firmware generation.
Once `firmware/v1` has been published, do not modify its firmware in place. Before
further backend behavior changes, create the next `firmware/vN` branch from the
previous released generation and update `include/FirmwareVersion.h` to `N.0.0`.
The published `firmware/v1` snapshot is version `1.0.0`. Preserve camera-specific
local changes in `include/Config.h`; changing a camera ID for deployment does not
require a new firmware version. GUI, Python bridge, and documentation changes alone
do not require a firmware version bump.
