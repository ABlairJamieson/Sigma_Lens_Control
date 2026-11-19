# Live view, focus and independent housing power

These frontend and ASI bridge changes require no Teensy firmware change. They use
existing SF (set focus) and ST (status) commands. Hardware testing of the new live
settings, speed, focus crop and autofocus remains pending; the tests use simulated
cameras/lenses. No lens or camera was connected during development.

## Live exposure and response time

Live view normally follows the selected slot's capture exposure and gain, including
changes made while it is running. Uncheck **Use capture exposure/gain** to set a
separate live exposure/gain without changing still-capture settings. Live exposure
is limited to 1–2000 ms; longer capture exposures are left unchanged and the live
status identifies the 2 s limit. SDK-confirmed live exposure/gain appear in status.
A frame already exposing or queued in the SDK may still use the previous settings.

The frontend now requests the next frame as soon as the preceding frame has been
decoded/displayed; it no longer adds a 200 ms delay (which previously imposed a
maximum below 5 fps). It keeps only one request in flight. The bridge applies live
exposure/gain changes without restarting video, and avoids repeating unchanged
SDK control transactions. The displayed fps measures delivered frame intervals. Hover over live status for
SDK frame-read time, image processing/JPEG time, request round trip and payload
size; these help separate camera/host work from network/display costs.

Full view uses the largest advertised binning that does not exceed the requested
scale, then reduces the image for display. Fullscreen can request a larger image
and therefore more processing/transfer. **Focus detail (crop)** instead reads
an unbinned central sensor ROI, up to 1024×768 pixels. This reduces transferred
sensor pixels and retains fine detail for focus; it does not show the full scene.
The display still fits that crop to the preview/fullscreen window.

USB 3 only speeds the camera-to-host part of the path. Sensor readout/exposure,
SDK buffering, image conversion/JPEG, the network and GUI display also contribute.
No measured hardware speedup is claimed. Use the frame rate readout to compare
full/focus view and fullscreen, and verify the actual negotiated USB speed on the
host when testing (e.g. `lsusb -t`), rather than inferring it from image size.

## Autofocus

Put a stationary, textured part of the subject in the centre, set a useful live
exposure/gain, and manually get reasonably close to focus. Connect the lens and
camera, ensure lens status is **RegularMessaging**, and click **Autofocus**.
The **± raw** value limits the search around the current focus (default ±256,
clipped to the existing supported lens range 8232–12777). Increase it if necessary;
a narrow local search cannot guarantee a global focus solution.

The GUI first verifies a fresh focus-capable frame from the host bridge before
sending any focus movement. It then temporarily selects focus detail and freezes
exposure/gain/aperture/manual lens controls. The search:

- measures nine coarse positions, then up to nine fine positions around the best;
- approaches every position, including the final one, from the same direction,
  using a 32-unit backoff where the lens limits permit it;
- polls ST every 250 ms and waits for reported actual focus to match the request;
- waits another 350 ms, changes a unique frame token and restarts the small ROI
  video stream, discarding two startup frames before scoring;
- takes the median of three scores at each position, and returns to the best
  measured position with a final telemetry check.

The score is mean squared Sobel gradient magnitude (Tenengrad) after a light 3×3
Gaussian blur, measured in the central 80% of an at-most 1024×768 uncompressed sensor patch before
resizing/JPEG. This is contrast-detection autofocus. See the
[OpenCV comparison of focus measures](https://opencv.org/blog/autofocus-using-opencv-a-comparative-study-of-focus-measures-for-sharpness-assessment/)
and [Pertuz et al., Analysis of focus measure operators (2013)](https://isp-utb.github.io/seminario/papers/Pattern_Recognition_Pertuz_2013.pdf).
Coarse-to-fine search, the bounds, timing and backlash allowance are this project's
initial implementation choices; they need tuning against the real lens/housing.

Dark/saturated frames, missing position/frame feedback, insufficient score
variation or a three-minute search timeout cancel the search. **Cancel** requests
the original position and waits for telemetry before releasing the controls.
Closing the window during autofocus also cancels/returns before closing. If the
lens is disconnected or does not acknowledge, the result explicitly says the
position could not be verified. A peak at the search edge is flagged for manual
checking or a wider search. This is a first implementation, not yet calibrated for
moving subjects, changing illumination, underwater backscatter or low texture.

## Power/status without an ASI connection

On a camera slot's **Power/status** tab, click **Connect host**. It uses the host
and camera port configured on that slot's Connection tab (normally 5001), but opens
an independent TCP connection and sends `carrier_read`, not ASI `status`/open.
No additional network port is needed. The connected host endpoint is displayed.

Camera 12 V switching and INA226/TMP117 readbacks work when the ASI camera is off,
unplugged, or disconnected in the frontend. Disconnecting the camera leaves host
telemetry connected. **Disconnect host** closes only housing control/telemetry.
Changing the host/port disconnects housing control so old readings cannot be used
for a newly selected host. BeagleY hosts and missing services remain unsupported.

Readbacks continue on their own five-second polling connection during capture/live
view; they are marked stale if replies cease. Power changes are disabled during
capture/live view/autofocus, and the bridge also rejects conflicting power commands.
Stop live view before power-off. The computer's 5 V rail remains monitoring only.

## Deployment and validation

Update **both** the desktop frontend and each host's `asi_bridge/asi_bridge.py`.
After updating the host checkout used by its service:

```sh
sudo systemctl restart asi-bridge
```

Do not restart sigma-carrier merely to deploy these changes: restarting that
service defaults camera power OFF. Rebuild the desktop frontend with qmake/make
(or use the prepared x86_64 executable) and launch through
`frontend/run_sigma_control.sh` to avoid the WaveDump Qt library conflict.
An older bridge can still supply its older live view, but autofocus refuses to move
until the new focus frame fields are present.

Hardware-free checks:

```sh
python3 -m unittest discover -s tests -v
# Build/run these .pro files in separate build directories:
# frontend/tests/live_view_test.pro       (QT_QPA_PLATFORM=offscreen)
# frontend/tests/autofocus_test.pro
# frontend/tests/carrier_control_test.pro (QT_QPA_PLATFORM=offscreen)
# frontend/tests/environment_history_test.pro (QT_QPA_PLATFORM=offscreen)
```

When hardware returns: first confirm host-only power/readbacks with the camera
connection closed; then compare live gain/exposure changes and frame rates, test
focus detail, and finally run a small autofocus search on a stationary focus chart.
Confirm the selected position against manual focus, repeat from both approach
directions and check cancellation. Repeat cold starts through the single surface
switch separately to assess USB 3 negotiation; live-view changes do not validate
that hardware power-sequencing fix.
