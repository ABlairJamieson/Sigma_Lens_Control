# Live view, focus and independent housing power

These frontend and ASI bridge changes require no Teensy firmware change. They use
existing SF (set focus) and ST (status) commands. Live settings, USB speed, focus crops and housing readbacks have been checked
on pch5. Completed autofocus accuracy still needs calibration against the real
lens/scene; automated tests use simulated cameras/lenses.

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
and therefore more processing/transfer. Autofocus temporarily reads an unbinned
sensor ROI, up to 1024×768 pixels, at the selected location. This reduces transferred
sensor pixels and retains fine detail for scoring. Manual wheel zoom is independent
of that autofocus crop and enlarges the received preview for display.

USB 3 only speeds the camera-to-host part of the path. Sensor readout/exposure,
SDK buffering, image conversion/JPEG, the network and GUI display also contribute.
Use the frame rate readout to compare full view, autofocus detail and fullscreen, and verify the actual negotiated USB speed on the
host when testing (e.g. `lsusb -t`), rather than inferring it from image size.

## Selecting the focus region

Click inside the displayed image to move a fixed 1024×768 **sensor-pixel** box.
The yellow box marks the selected region; its screen size changes with the preview
scale, but its sensor size stays fixed. At sensor edges it is clamped and aligned
to the SDK's 8-pixel X / 2-pixel Y grid. Smaller sensors use the largest fitting ROI.
The sharpness readout follows this region in both views, using its central 80%.
Full view can be binned, so compare scores within one view mode/scale. Autofocus
always uses the unbinned crop.

Right-click inside the image to centre the ROI. Each camera slot retains its own
selection for this application session. Black margins around the image are ignored.
Selection works on full live view, frozen/still previews, and the explicit Fullscreen
view. Clicks in an already cropped view map through that crop to full-sensor
coordinates, allowing small location adjustments. Selection is locked during AF.

Single and double clicks no longer open an external image viewer or enlarge the
preview. Use the **Fullscreen** button to enlarge and Esc to return. Before a live
frame has supplied sensor geometry, a crosshair marks the intended location; start
live view to obtain the accurate fixed-size box. Autofocus requires the updated
host bridge to confirm the selected sensor crop before moving the lens.

## Autofocus

Click a stationary, textured part of the subject in the full preview to place the
focus ROI over it, then set a useful live
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
Gaussian blur, measured in the central 80% of the selected ROI before
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

## Power + Connection without an ASI connection

On a PCH's **Power + Connection** tab, click **Connect host**. It uses the host
and camera port configured on that same page (normally 5001), but opens
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

## PCH connections and lens recovery

The eight device pages are PCH1–PCH8, with defaults 192.168.50.2–192.168.50.9
respectively (lens TCP 5000, camera/host TCP 5001). Host and port edits are saved
separately under `pch1` … `pch8` in SigmaControl settings; the old single global
host setting is no longer copied across every page.

Each PCH has three pages: **Camera + Lens**, **Power + Connection**, and
**Environment**. In Power + Connection, click **Connect host**, enable camera
12 V, then connect camera/lens. Host power and telemetry do not require an ASI
camera connection. Existing Connect both/Disconnect both shortcuts remain above
the pages.

A failed TCP lens connection, or no Teensy ID/state within 3 seconds, requests
one `lens_bridge_restart` through that PCH's camera/host bridge. SigmaControl
waits 6 seconds for the service and retries once; no second restart occurs in
that attempt. Missing USB or a failed retry produces an explicit failure.
Disconnect cancels pending recovery. Serial-direct connections are unaffected.
The host camera bridge must be updated for this command. The fixed service must
run as the same user with `Restart=always`; no sudo permission is added. Restart
requests are rate-limited to one per 30 seconds. Only the Teensy bridge restarts;
this does not restart sigma-carrier or change camera 12 V.


Autofocus completion now explicitly reports the lens-confirmed final raw position.
Cancel returns to the starting focus, rather than applying the best partial-search
sample. Progress, each median sharpness score, commanded positions and the return
phase are written to the normal session log. A return timeout reports uncertainty
and the last reported position, without claiming the original focus was restored.
An autofocus result remains visible if subsequent live frames time out.


## Preview zoom

Roll the mouse wheel over the preview to zoom in/out at the cursor (1× to 32×).
Middle-drag pans; **Fit** returns to the entire image. This works in the main and
fullscreen viewer, and each PCH retains its own zoom/pan while the app is open.
Zoom is display-only: it enlarges the received preview, without changing capture
settings, camera acquisition, the autofocus score, or the sensor-pixel focus ROI.
Clicking moves the fixed 1024×768 sensor-pixel ROI through the displayed zoomed
coordinates; its outline scales with zoom. Right-click still centres the ROI.

The manual focus-crop checkbox is replaced by wheel zoom. Autofocus still requests
an unbinned camera crop internally, temporarily shows that crop while running,
and restores the display zoom afterward. Zoom and focus selection are locked
while autofocus is active.

**Use capture exposure/gain** is checked on startup. Duplicate live exposure/gain
inputs are hidden while linked and appear only when that option is unchecked.
The existing 2-second live-exposure cap still applies; still exposure is unchanged.


## Timeout recovery and measured limitations

The host allows six seconds plus twice the exposure for a steady full-field video
read (eight seconds during startup); focus crops retain a three-second allowance.
The full-field setting accounts for a measured 3.87-second SDK read that exceeded
the former three-second deadline. Discarded startup frames share one request
budget so they cannot each consume a separate long wait.

After three consecutive SDK timeouts, the bridge stops/restarts the preview stream
once without changing camera power. It retains ownership during recovery and
continues reporting live-active so power cannot be switched in the restart gap.
Focus samples discard two startup frames again. A good frame resets the recovery
budget; three more timeouts without a good frame stop preview with an explicit
error. The frontend keeps Live view checked while retrying and shows the recovery
state; persistent or other hard errors still turn it off.

pch5 hardware tests on 2026-11-20 confirmed that 1024 MB USB buffering and hardware
binning did not eliminate long full-field stalls. Hardware binning produced fast
bursts (roughly 53–127 ms SDK reads), but a longer run still stalled, so it is not
enabled by default. A 4096×3072 sensor crop was faster in the short benchmark but
reduces field of view. JPEG resizing and display zoom do not reduce sensor-readout
work. These changes improve recovery; they do not establish a fix for the remaining
host/SDK/USB transport stalls. Comparison results are saved with this chat's outputs.
