# Render Pool

Render Pool lets several RFF-EXP instances generate dynamic video keyframes for one host. It supports password-protected Internet pools and automatic local-network pools.

## Internet pool

1. Open **Video > Rendering Process** and choose **Render Pool**.
2. Choose **Internet**, enter a password, and select **Start**.
3. RFF-EXP uses UPnP IGD to map TCP port `48191` and makes a numeric ID from the router's public IPv4 address.
4. Use **Copy ID**, then share the ID and password with the workers by a trusted channel.
5. Choose **Start Keyframe Job** and select an output folder.

Internet pools always require a non-empty password. If the router has UPnP disabled, has no usable Internet Gateway Device, or is behind carrier-grade NAT, RFF-EXP reports that it could not prepare the public pool. UPnP port mappings are removed when the host leaves the pool or closes RFF-EXP.

To join, choose **Internet**, enter the host's ID and password, then select **Join**. The password is proved with a salted PBKDF2 challenge and is not sent directly.

## Automatic LAN pool

1. On the host, choose **LAN (automatic)** and select **Start**.
2. On each worker on the same local network, choose **LAN (automatic)** and select **Join**.
3. The worker discovers and connects to the host without an ID or password.

LAN discovery uses UDP port `48192`; rendering uses TCP port `48191`. Allow RFF-EXP through the operating-system firewall on private networks when prompted. LAN mode is intended only for a trusted local network.

## Rendering and progress

Leave **Host Also Renders** enabled if the host should render alongside its workers. Workers can use **Render Assigned Keyframes** to pause or resume their own contribution.

The progress dots show every keyframe. Hover a dot to see its frame number, zoom, state, assigned worker, attempt count, and latest error. New assignments can be paused without interrupting frames already being rendered. The dots use the Rendering Process window's scrollbar instead of a nested scrollbar.

Workers see the same overall keyframe count, progress bar, and colored progress dots as the host. RFF-EXP temporarily locks navigation while it renders an assigned frame. When contribution stops or the job finishes, the worker's previous settings, resolution, and location are restored.

The host's output width, output height, and Clarity multiplier are part of the job manifest. Every worker temporarily applies all three, and the host verifies the resulting internal calculation dimensions before accepting a frame. CPU thread count remains local to each worker.

## Recovery and validation

- Jobs use explicit frame IDs instead of choosing the next unused filename.
- The host saves `.rff-render-pool-job` in the keyframe folder. **Resume Keyframe Job** rebuilds progress from that manifest and the verified `.rfm`/`.rfl` files already present.
- Workers render to memory and the host writes a `.partial` file first. A result becomes a numbered `.rfm` only after its job ID, frame ID, zoom, dimensions, and binary structure pass validation.
- A disconnected worker's assignment returns to the queue. Repeated failures remain visible and can be retried from the server window.
- Large worker uploads are queued off the GUI thread.

## Current scope

- Dynamic `.rfm` video keyframes are supported. Static PNG keyframes are not distributed yet.
- Internet pool IDs encode the UPnP gateway's public IPv4 address plus a typo-detection digit and use TCP port `48191`.
- LAN discovery uses IPv4 multicast, IPv4 broadcast, and a loopback fallback. The first compatible LAN host to answer is used.
- Passwords are not transmitted directly; Internet mode uses a salted PBKDF2 proof. Rendered data is not yet end-to-end encrypted, so do not use an Internet pool for sensitive material or across an untrusted network.
- UPnP does not bypass carrier-grade NAT and does not act as a relay. Routers without UPnP support cannot currently host an Internet pool automatically.
- Every worker should run the same RFF-EXP revision. Worker thread counts remain local so each computer can use its own CPU efficiently.
