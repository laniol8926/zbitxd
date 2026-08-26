"use strict";

const GRIDMAP = (function gridmap() {
  // Private variables and functions
  var containerDiv;
  // Offscreen canvas
  let width;
  let height;
  const ofsCanvas = document.createElement('canvas');
  const ofsCtx = ofsCanvas.getContext('2d');
  // Onscreen canvas (fixed size)
  const canvasDiv = document.createElement("div");
  const onsCanvas = document.createElement("canvas");
  const onsCtx = onsCanvas.getContext('2d');
  // ZBITXD LOCAL CHANGE (2026-08-24): const -> let, plus the resize()
  // export near the bottom of this file -- everything else in this
  // file is unmodified from ~/Downloads/sbitx/web/'s copy. Lets the
  // map canvas actually track its container's real size (draggable/
  // resizable .sbitx-panel in this app) instead of staying pinned at
  // one fixed size regardless of how the panel gets resized. Keep this
  // diff minimal and clearly marked so a future sync from upstream
  // stays easy to reconcile.
  let fixedWidth = 520; // Fixed canvas width
  let fixedHeight = 360; // Fixed canvas height

  const slider = document.createElement("input");
  const zoomSpan = document.createElement("span");
  const infoDiv = document.createElement("div");
  const infoSpan = document.createElement("span");

  const btnGridsJustLogged = document.createElement("button");

  const projection = proj4('+proj=merc +lat_0=0 +lon_0=0 +ellps=WGS84 +datum=WGS84 +units=m');
  const img = new Image();

  img.crossOrigin = "anonymous";
  // ZBITXD LOCAL CHANGE (2026-08-25): user's own ask -- swapped for a
  // much higher-resolution base map (6930x5870 vs the original
  // 2068x2060) to hold up at the new 800% zoom cap without turning
  // into a blurry blown-up JPEG. Same cartographer/source imagery
  // (Strebe, NASA Blue Marble derivative) as the file this replaces,
  // just a different, larger rendering of it -- covers 82 degrees N/S
  // (not 85), hence ky below. CC BY-SA 3.0 -- requires attribution to
  // Strebe if this app or its output is ever redistributed/published.
  // https://commons.wikimedia.org/wiki/File:Mercator_projection_SW.jpg
  img.src = "./Mercator_projection_SW.jpg";

  const kx = projection.forward([180, 0])[0]; // x-coordinate at (180�, 0�) in meters
  const ky = projection.forward([0, 82])[1];  // y-coordinate at (0�, 82�) in meters -- see img.src's own comment

  const scaleMin = 25;
  // ZBITXD LOCAL CHANGE (2026-08-25): user's own ask -- 200% (the
  // slider's old max, shown as "2.00") wasn't tight enough to separate
  // nearby grid boxes for close-range daytime groundwave contacts
  // (40M especially). Grid box positions come from real lat/lon math
  // (gmGridIdToPoint()/gmGridToWorldPoint()), not from the base map
  // image's own pixel detail, so zooming in this far still places them
  // correctly -- the underlying photo just looks softer/blockier at
  // the high end since it's a fixed 2068x2060 source image, same as
  // enlarging any photo past its native resolution.
  const scaleMax = 800;
  const scaleStep = 5;
  var scaleCur = 25; // Changed to 30% initial size

  const btnGridsLogged = document.createElement("button");
  const gridsLogged = new Set();
  let showGridsLogged = false;

  const btnGridsSeen = document.createElement("button");
  const gridsSeenLogged = new Set();
  const gridsSeenNotLogged = new Set();
  const gridsSeenJustLogged = new Set();
  let showGridsSeen = false;
  let showGridsUnlogged = true;

  // ZBITXD LOCAL CHANGE (2026-08-25): user's own ask -- a station's
  // box+label used to get baked onto the canvas once and stay forever
  // (real report, live: a station last heard sending a one-way report
  // minutes earlier, never replied to, still sitting on the map with
  // no way to tell it was stale). This is a real-time display, and a
  // typical FT8 QSO runs 60-90s end to end, so anything not re-heard
  // within STATION_TTL_MS is noise, not signal. callsign -> {grid,
  // lastSeen}; gmPruneStaleStations() (below) sweeps this on a timer
  // and fully rebuilds the canvas (reloadGridMap()) whenever anything
  // actually expired -- a baked box/label can't be selectively erased
  // any other way.
  const stationLastSeen = new Map();
  const STATION_TTL_MS = 90 * 1000;
  const STATION_SWEEP_MS = 10 * 1000;

  const btnGridsUnLogged = document.createElement("button");

  const btnShowRoundDots = document.createElement("button");
  var use_square_dots = true;
  
  var viewOffsetX = 0; // Tracks panning offset (replaces scrollLeft)
  var viewOffsetY = 0; // Tracks panning offset (replaces scrollTop)

  // ZBITXD LOCAL CHANGE (2026-08-24): QSO line + animated direction
  // arrow, user's own ask (inspired by FT8AW's map) -- a line between
  // my own grid and the station currently being worked, with a small
  // arrowhead animating along it in whichever direction the
  // transmission is actually going (mine->theirs while transmitting,
  // theirs->mine while receiving/idle -- see setQsoLine()'s own
  // comment). Direction/endpoints are pushed in from index.html
  // (setQsoLine()/clearQsoLine(), the only two new exports this
  // feature needs) rather than gridmap.js reaching for CALL/EXCH/in_tx
  // itself -- keeps this file self-contained, matching how it already
  // has no other awareness of this app's own field/exchange model.
  var qsoLineActive = false;
  var qsoLineMyGrid = null, qsoLineTheirGrid = null;
  var qsoLineForward = true; // true: animate my->their (I'm transmitting). false: their->my.
  var qsoLineT = 0; // 0..1, current position along the line
  var qsoLineTimer = null;
  const qsoLineStepMs = 150; // ~7fps -- see gmStartQsoLineAnim()'s own comment on why this stays throttled

  // ZBITXD LOCAL CHANGE (2026-08-24): "the lines between stations
  // calling each other" -- user's own follow-up ask, same FT8AW-
  // inspired feature, now extended to every QSO visible in decoded
  // traffic, not just my own. Keyed by a stable "CALLA,CALLB" (sorted)
  // string per pair; each entry is {gridA, gridB, fromCall, lastSeen}.
  // Fed entirely from index.html (trackExchange(), the one new export
  // this needs) -- band scoping, like everything else map-related, is
  // index.html's job, not this file's (see gmClearSeen()'s own comment
  // on that split). Visually subdued relative to my own line (thinner,
  // dimmer) -- dozens of these can be on screen on a busy band, and
  // only one QSO here is actually mine. User's own precise spec, live:
  // arrow points toward whoever *hasn't* just transmitted (i.e. from
  // the sender of the last decode toward the other station -- flips
  // every real ~15s cycle as a genuine back-and-forth continues), and
  // the whole line disappears once neither side has been heard from in
  // a while -- see thirdPartyTtlMs's own comment.
  var thirdPartyPairs = new Map();
  var thirdPartyTimer = null;
  var thirdPartyT = 0; // 0..1, one shared animation phase for every pair's arrow -- see gmStartThirdPartyAnim()'s own comment on why a single shared phase, not one per pair
  // Real report, live (2026-08-24): "20 seconds is too long" (this
  // constant's prior value, 25s) -- "no decodes between station123 or
  // stationabc in either direction causes the line to disappear",
  // user's own spec, against FT8's real ~15s message cadence. Tightened
  // to 18s (barely more than one real cycle) -- enough slack for a
  // *slightly* late decode without every line flickering off between
  // every single cycle, but close enough to 15s that a pair which
  // genuinely stopped exchanging (QSO logged, one side gave up) drops
  // its line almost immediately, not minutes later.
  const thirdPartyTtlMs = 18 * 1000;

  function setToolTip(elt, tip) {
    elt.className = "tooltip";
    const tipSpan = document.createElement("span");
    tipSpan.innerText = tip;
    tipSpan.className = "tooltiptext";
    elt.appendChild(tipSpan);
  }

  function setBtnsStateEnable(b) {
    btnGridsSeen.className = showGridsSeen ? "gm_btn_on" : "gm_btn_off";
    btnGridsSeen.enabled = b;
    btnGridsLogged.className = showGridsLogged ? "gm_btn_on" : "gm_btn_off";
    btnGridsLogged.enabled = b;
    btnGridsUnLogged.className = showGridsUnlogged ? "gm_btn_on" : "gm_btn_off";
    btnGridsUnLogged.enabled = b;
    btnShowRoundDots.className = use_square_dots ? "gm_btn_off" : "gm_btn_on";
    btnShowRoundDots.enabled = b;
  }

  function gmBuildHtml() {
    const eltStyle = document.createElement("style");
    document.head.appendChild(eltStyle);
    eltStyle.textContent =
      ".gm_btn_on { background-color: white; }" +
      ".gm_btn_off { background-color: lightgray; }" +
      ".boxed { border: 1px solid black; }";
    containerDiv.style = "overflow: hidden; display: inline-block;";
    canvasDiv.appendChild(onsCanvas);
    canvasDiv.className = "boxed";
    canvasDiv.style = `overflow: hidden; width: ${fixedWidth}px; height: ${fixedHeight}px;`;
    onsCanvas.width = fixedWidth;
    onsCanvas.height = fixedHeight;

    zoomSpan.style = "width: 45px; display: inline-flex;";
    const sliderDiv = document.createElement("div");
    btnShowRoundDots.innerText = "Round";
    btnShowRoundDots.title = "Show big round dots";
    sliderDiv.appendChild(btnShowRoundDots);
    slider.type = "range";
    slider.min = scaleMin;
    slider.max = scaleMax;
    slider.value = scaleCur; // Reflects initial scaleCur = 30
    slider.step = scaleStep;
    slider.id = "gridzoom";
    slider.style = "width: 120px;";
    sliderDiv.appendChild(zoomSpan);
    sliderDiv.appendChild(slider);
    // ZBITXD LOCAL CHANGE (2026-08-25): user's own ask -- this map is a
    // real-time display of what's happening right now, not a tool for
    // browsing worked-station history, so filtering by logged/seen/
    // unlogged status doesn't fit it (echoes the user's own framing
    // from a couple days earlier: "seeing on a map where someone is
    // that i worked a week ago means nothing"). Left as dead code
    // rather than deleted -- gmGridIdLogged()/NotLogged()/JustLogged()
    // still use the underlying showGridsLogged/Seen/Unlogged flags to
    // decide whether to draw, so simply not attaching these three
    // buttons freezes the current defaults (Unlogged on, Logged/Seen
    // off) as permanent behavior without touching any of that logic.
    btnGridsLogged.innerText = "Logged";
    btnGridsLogged.title = "Show all logged grids";
    btnGridsSeen.innerText = "Seen";
    btnGridsSeen.title = "Show all grids seen during this session";
    btnGridsUnLogged.innerText ="Unlogged";
    btnGridsUnLogged.title = "Show all unlogged grids seen during this session";
    setBtnsStateEnable(false);
    sliderDiv.appendChild(infoDiv);
    infoDiv.appendChild(infoSpan);
    infoSpan.style = "font-size: 12px; width: 120px; text-align: center; display: inline-flex;";
    setToolTip(infoDiv, "(longitude,latitude) GridId");
    // ZBITXD LOCAL CHANGE (2026-08-25): user's own ask -- base map
    // image (img.src's own comment above) is CC BY-SA 3.0, which
    // requires visible attribution to the author wherever it's used.
    const mapCreditLink = document.createElement("a");
    mapCreditLink.innerText = "Map: Strebe, CC BY-SA 3.0";
    mapCreditLink.href = "https://commons.wikimedia.org/wiki/File:Mercator_projection_SW.jpg";
    mapCreditLink.target = "_blank";
    mapCreditLink.style = "font-size: 10px; color: white; margin-left: 8px; white-space: nowrap;";
    sliderDiv.appendChild(mapCreditLink);
    containerDiv.appendChild(canvasDiv);
    containerDiv.appendChild(sliderDiv);
  }

  function gmDrawScaledCanvas(scale) {
    if (scaleCur === scale) {
      console.log("redraw");
    }
    scaleCur = scale;

    const fScale = scale / 100;
    const scaledWidth = ofsCanvas.width * fScale;
    const scaledHeight = ofsCanvas.height * fScale;

    // Clear the onscreen canvas
    onsCtx.clearRect(0, 0, fixedWidth, fixedHeight);

    // Calculate source rectangle based on view offset
    const sourceWidth = fixedWidth / fScale;
    const sourceHeight = fixedHeight / fScale;
    let sourceX = viewOffsetX / fScale;
    let sourceY = viewOffsetY / fScale;

    // Clamp source coordinates to stay within map bounds
    const maxSourceX = ofsCanvas.width - sourceWidth;
    const maxSourceY = ofsCanvas.height - sourceHeight;
    sourceX = Math.max(0, Math.min(sourceX, maxSourceX));
    sourceY = Math.max(0, Math.min(sourceY, maxSourceY));

    // Update view offsets to clamped values
    viewOffsetX = sourceX * fScale;
    viewOffsetY = sourceY * fScale;

    // Draw the map, preserving aspect ratio
    onsCtx.drawImage(
      ofsCanvas,
      sourceX, sourceY, sourceWidth, sourceHeight,
      0, 0, fixedWidth, fixedHeight
    );

    // ZBITXD LOCAL CHANGE (2026-08-24): QSO line overlay(s), drawn
    // fresh on top of the just-blitted base map every time this runs
    // (pan/zoom, either animation/prune timer below) -- unlike the
    // grid dots (gmSetGridMark, baked directly into ofsCanvas), these
    // are drawn straight onto onsCtx/screen space every frame, never
    // persisted into the offscreen canvas, so they can move/come and
    // go without needing to ever "erase" a previous frame. sourceX/
    // sourceY/fScale are this exact frame's already-computed, already-
    // clamped world->screen transform -- reused as-is. Third parties
    // drawn first, my own line last/on top, so mine stays the visually
    // dominant one if a pair happens to overlap it.
    gmDrawThirdPartyLines(sourceX, sourceY, fScale);
    gmDrawQsoLine(sourceX, sourceY, fScale);

    slider.value = scaleCur;
    zoomSpan.innerText = (scale / 100).toFixed(2);
  }

  // ZBITXD LOCAL CHANGE (2026-08-24): draws every currently-tracked
  // third-party QSO line -- thin, static (no per-frame animation, to
  // keep drawing dozens of these on a busy band cheap), deliberately
  // subdued relative to my own line (gmDrawQsoLine() below) so my own
  // QSO stays the visually dominant one. User's own report, live:
  // "blue is not a good idea because the water is blue" (the original
  // color) and "no arrows" -- switched to light gray/silver (reads
  // against both the map's blue water and green/brown land) and added
  // a static arrowhead at each line's midpoint, oriented toward
  // whichever side sent the *most recently decoded* message for that
  // pair (entry.fromCall, updated every time gmTrackExchange() sees a
  // new one -- see its own comment) -- flips naturally as a real QSO's
  // own back-and-forth continues, without needing continuous
  // animation to show direction. No-op cheaply if the map is empty
  // (nothing tracked yet, or everything's expired and pruned).
  function gmDrawThirdPartyLines(sourceX, sourceY, fScale) {
    if (thirdPartyPairs.size === 0)
      return;
    const toScreen = (p) => [(p[0] - sourceX) * fScale, (p[1] - sourceY) * fScale];
    onsCtx.save();
    thirdPartyPairs.forEach(function (entry) {
      const pA = toScreen(gmGridToWorldPoint(entry.gridA));
      const pB = toScreen(gmGridToWorldPoint(entry.gridB));
      // User's own ask, live: "why not make the line color and width
      // identical to the one that appears when I transmit... makes it
      // easier to see what is going on" -- matched exactly to
      // gmDrawQsoLine()'s own styling for now (black 3px outline + gold
      // 1.5px), explicitly as a temporary debugging aid, not a final
      // design choice -- "we can adjust the colors and width later".
      onsCtx.strokeStyle = "black";
      onsCtx.lineWidth = 1.5; // user's own ask (2026-08-25): half of the original 3px
      onsCtx.beginPath();
      onsCtx.moveTo(pA[0], pA[1]);
      onsCtx.lineTo(pB[0], pB[1]);
      onsCtx.stroke();
      onsCtx.strokeStyle = "red"; // user's own ask (2026-08-25): was gold
      onsCtx.lineWidth = 0.75; // half of the original 1.5px
      onsCtx.stroke();

      // from/to: whichever point sent the last known message for this
      // pair -> the other -- the arrow always points sender->recipient,
      // same direction the actual transmission travels. Falls back to
      // A->B if fromCall was never set (shouldn't normally happen --
      // gmTrackExchange() always receives one -- but degrades
      // gracefully rather than drawing nothing if it ever is).
      const fromIsA = !entry.fromCall || entry.fromCall === entry.callA;
      const from = fromIsA ? pA : pB;
      const to = fromIsA ? pB : pA;
      // thirdPartyT: one shared 0..1 phase driving every pair's arrow
      // at once (gmStartThirdPartyAnim()) -- a real per-pair phase
      // would need its own state and timer bookkeeping for what could
      // be a couple dozen simultaneous pairs on a busy band, for a
      // visual difference nobody would actually notice (all the arrows
      // moving in sync reads perfectly fine as "traffic is flowing").
      const arrowX = from[0] + (to[0] - from[0]) * thirdPartyT;
      const arrowY = from[1] + (to[1] - from[1]) * thirdPartyT;
      const angle = Math.atan2(to[1] - from[1], to[0] - from[0]);
      const size = 5.5; // user's own ask (2026-08-25): half of the original 11
      onsCtx.save();
      onsCtx.translate(arrowX, arrowY);
      onsCtx.rotate(angle);
      onsCtx.beginPath();
      onsCtx.moveTo(size, 0);
      onsCtx.lineTo(-size, -size * 0.6);
      onsCtx.lineTo(-size, size * 0.6);
      onsCtx.closePath();
      onsCtx.fillStyle = "gold"; // user's own ask (2026-08-25): keep the arrowhead itself gold, only the line went red
      onsCtx.fill();
      onsCtx.lineWidth = 1.5;
      onsCtx.strokeStyle = "black";
      onsCtx.stroke();
      onsCtx.restore();
    });
    onsCtx.restore();
  }

  // ZBITXD LOCAL CHANGE (2026-08-24): draws the QSO line + its
  // animated arrowhead in screen space, using this frame's own
  // world->screen transform (sourceX/sourceY/fScale, from
  // gmDrawScaledCanvas() just above). No-op entirely if there's no
  // active line -- the common case (idle, or CQ Panel showing instead
  // of an actual exchange) costs nothing extra per frame.
  function gmDrawQsoLine(sourceX, sourceY, fScale) {
    if (!qsoLineActive || !qsoLineMyGrid || !qsoLineTheirGrid)
      return;
    const myPt = gmGridToWorldPoint(qsoLineMyGrid);
    const theirPt = gmGridToWorldPoint(qsoLineTheirGrid);
    const toScreen = (p) => [(p[0] - sourceX) * fScale, (p[1] - sourceY) * fScale];
    const myScreen = toScreen(myPt);
    const theirScreen = toScreen(theirPt);

    // Real report, live (2026-08-24): "no lines no arrows" -- but the
    // end-cap boxes (drawn from these exact same myScreen/theirScreen
    // coords, further down) *did* show, confirming this function was
    // running with valid coordinates the whole time. Real cause: a
    // 2px, 75%-opacity, half-dashed gold line is easy to lose entirely
    // against a busy map image -- not a logic bug. Bolder now: solid,
    // fully opaque, thicker, and outlined in black for contrast against
    // both light and dark parts of the map.
    // User's own follow-up, live: confirmed visible, "the line can be
    // thinner" -- thinned back down from the initial (deliberately
    // over-bold, to first confirm it was a visibility problem and not
    // a logic bug) 6px/3px.
    onsCtx.save();
    onsCtx.strokeStyle = "black";
    onsCtx.lineWidth = 1.5; // user's own ask (2026-08-25): half of the original 3px
    onsCtx.beginPath();
    onsCtx.moveTo(myScreen[0], myScreen[1]);
    onsCtx.lineTo(theirScreen[0], theirScreen[1]);
    onsCtx.stroke();
    onsCtx.strokeStyle = "gold";
    onsCtx.lineWidth = 0.75; // half of the original 1.5px
    onsCtx.stroke();

    // qsoLineForward: true while transmitting (arrow travels my->their),
    // false while receiving/idle (their->my) -- see setQsoLine()'s own
    // comment. Swapping which endpoint is "from"/"to" here, rather than
    // reversing qsoLineT itself, is what lets the arrow keep moving
    // smoothly through a direction change instead of jumping.
    const from = qsoLineForward ? myScreen : theirScreen;
    const to = qsoLineForward ? theirScreen : myScreen;
    const arrowX = from[0] + (to[0] - from[0]) * qsoLineT;
    const arrowY = from[1] + (to[1] - from[1]) * qsoLineT;
    const angle = Math.atan2(to[1] - from[1], to[0] - from[0]);
    const arrowSize = 11;

    onsCtx.translate(arrowX, arrowY);
    onsCtx.rotate(angle);
    onsCtx.beginPath();
    onsCtx.moveTo(arrowSize, 0);
    onsCtx.lineTo(-arrowSize, -arrowSize * 0.6);
    onsCtx.lineTo(-arrowSize, arrowSize * 0.6);
    onsCtx.closePath();
    onsCtx.fillStyle = "gold";
    onsCtx.fill();
    onsCtx.lineWidth = 1.5;
    onsCtx.strokeStyle = "black";
    onsCtx.stroke();
    onsCtx.restore();

    // Small end-caps at each station's own point, same "boxed" visual
    // language the grid dots already use, so the two ends of the line
    // read as real, specific locations rather than just where a line
    // happens to stop.
    onsCtx.fillStyle = "white";
    onsCtx.fillRect(myScreen[0] - 2, myScreen[1] - 2, 4, 4);
    onsCtx.fillRect(theirScreen[0] - 2, theirScreen[1] - 2, 4, 4);
  }

  // ZBITXD LOCAL CHANGE (2026-08-24): throttled animation loop (~7fps,
  // qsoLineStepMs) -- deliberately not requestAnimationFrame, which
  // would run up to 60fps and (per this app's own established history:
  // an unthrottled per-frame redraw once pegged this same device's GPU
  // near 100% for hours and froze keyboard/mouse) is far more headroom
  // than a slowly-moving arrow on a QSO line actually needs. Stops
  // itself the instant there's no active line (clearQsoLine(), or the
  // panel becoming hidden -- containerDiv.offsetParent is null exactly
  // when a display:none ancestor exists, the same check the browser's
  // own layout engine uses) rather than continuing to redraw an
  // invisible canvas at all.
  function gmStartQsoLineAnim() {
    if (qsoLineTimer)
      return; // already running
    qsoLineTimer = setInterval(function () {
      if (!qsoLineActive || !containerDiv || containerDiv.offsetParent === null) {
        clearInterval(qsoLineTimer);
        qsoLineTimer = null;
        return;
      }
      qsoLineT += 0.04;
      if (qsoLineT > 1)
        qsoLineT -= 1;
      gmDrawScaledCanvas(scaleCur);
    }, qsoLineStepMs);
  }

  function gmToMercatorPoint(longitude, latitude) {
    const xOfs = 0;
    const yOfs = 0;
    let point = projection.forward([longitude, latitude]);
    point[0] = (point[0] + kx) / (2 * kx) * ofsCanvas.width + xOfs;
    point[1] = (ky - point[1]) / (2 * ky) * ofsCanvas.height + yOfs;
    return point;
  }

  function gmFromMercatorPoint(mapX, mapY) {
    const xOfs = 0;
    const yOfs = 0;
    let mercX = (mapX - xOfs) / ofsCanvas.width * (2 * kx) - kx;
    let mercY = ky - (mapY - yOfs) / ofsCanvas.height * (2 * ky);
    let pos = projection.inverse([mercX, mercY]);
    return pos;
  }

  function gmIsValidGridId(gridId) {
    return (
      gridId.length == 4 &&
      gridId[0] >= 'A' && gridId[0] <= 'R' &&
      gridId[1] >= 'A' && gridId[1] <= 'R' &&
      gridId[2] >= '0' && gridId[2] <= '9' &&
      gridId[3] >= '0' && gridId[3] <= '9'
    );
  }

  function gmGridIdToPoint(gridId) {
    let point = [0, 0];
    if (gmIsValidGridId(gridId)) {
      point[0] = (gridId.charCodeAt(0) - 'A'.charCodeAt(0)) * 10 + (gridId.charCodeAt(2) - '0'.charCodeAt(0));
      point[1] = (gridId.charCodeAt(1) - 'A'.charCodeAt(0)) * 10 + (gridId.charCodeAt(3) - '0'.charCodeAt(0));
    }
    return point;
  }

  // ZBITXD LOCAL CHANGE (2026-08-24): grid square -> world/ofsCanvas
  // pixel coords in one step, same lon/lat formula gmSetGridMark()
  // already uses (col/row -> longitude/latitude), just returning the
  // Mercator point instead of drawing a mark with it. Used by the QSO
  // line below.
  function gmGridToWorldPoint(gridId) {
    const p = gmGridIdToPoint(gridId);
    const longitude = p[0] * 2 - 180 + 0.0;
    const latitude = p[1] - 90 + 1.0;
    return gmToMercatorPoint(longitude, latitude);
  }

  function gmSetPix(x, y) {
    const latitude = -y * 180.0 / ofsCanvas.height + 90;
    const longitude = x * 360.0 / ofsCanvas.width - 180;
    const point = gmToMercatorPoint(longitude, latitude);
    ofsCtx.fillStyle = "red";
    ofsCtx.fillRect(point[0] - 1, point[1] - 1, 3, 3);
  }

  function gmMarkPlace(longitude, latitude, clr) {
    if (latitude > 82 || latitude < -82) return; // matches img.src's own coverage -- see its comment above
    const point = gmToMercatorPoint(longitude, latitude);
    ofsCtx.fillStyle = clr;
    ofsCtx.fillRect(point[0] - 1, point[1] - 1, 3, 3);
  }

  // Replace gmSetGridMark:
  function gmSetGridMark(col, row, clr) {
    const f = 150.0;
    const longitude = col * 2 - 180 + 0.0;
    const latitude = row - 90 + 1.0;

    const point = gmToMercatorPoint(longitude, latitude);
    
    if (use_square_dots) {
      // const sx = Math.round(ofsCanvas.width / f);
      const sy = Math.round(ofsCanvas.height / f);
      // const oldMode = ofsCtx.globalCompositeOperation;
      // ofsCtx.globalCompositeOperation = 'difference';
      ofsCtx.fillStyle = "PowderBlue";
      ofsCtx.fillRect(point[0], point[1], sy/2+1, sy/2+1);
      // ofsCtx.globalCompositeOperation = oldMode;
      ofsCtx.fillStyle = clr;
      ofsCtx.fillRect(point[0]+1, point[1]+1, sy/2-1, sy/2-1);
    }
    else {
      const sx = Math.round(ofsCanvas.width / f);
      const sy = Math.round(ofsCanvas.height / f);
      const radius = Math.min(sx, sy) / 2; // Radius for circles
      ofsCtx.fillStyle = clr;
      ofsCtx.beginPath();
      ofsCtx.arc(point[0] + radius, point[1] + radius, radius, 0, 2 * Math.PI);
      ofsCtx.fill();
    }    
    
  }
  function gmShowGridId(gridId, clr) {
    const point = gmGridIdToPoint(gridId);
    gmSetGridMark(point[0], point[1], clr);
  }

  // ZBITXD LOCAL CHANGE (2026-08-24): diagnostic step, user's own ask
  // ("we know the grids, they're the boxes. Display the station call
  // sign at the grid" -- verify the underlying callsign<->grid data is
  // actually correct before debugging the QSO lines any further).
  // Baked directly onto ofsCanvas next to the grid's own dot, same as
  // the dots themselves (gmSetGridMark) -- so it pans/zooms with the
  // map for free and doesn't need any per-frame redraw cost. Black
  // outline + white fill for contrast against any part of the map.
  function gmLabelCallsign(gridId, callsign) {
    const point = gmGridIdToPoint(gridId);
    const longitude = point[0] * 2 - 180 + 0.0;
    const latitude = point[1] - 90 + 1.0;
    const p = gmToMercatorPoint(longitude, latitude);
    ofsCtx.font = "10px sans-serif";
    ofsCtx.textBaseline = "bottom";
    ofsCtx.lineWidth = 3;
    ofsCtx.strokeStyle = "black";
    ofsCtx.strokeText(callsign, p[0] + 4, p[1] - 2);
    ofsCtx.fillStyle = "white";
    ofsCtx.fillText(callsign, p[0] + 4, p[1] - 2);
  }

  function gmSetGridDot(gridId, clr) {
    const point = gmGridIdToPoint(gridId);
    const longitude = point[0] * 2 - 180 + 0.0;
    const latitude = point[1] - 90 + 1.0;
    gmMarkPlace(longitude, latitude, clr);
  }

  var pick_x = 0;
  var pick_y = 0;
  var pick_left = 0;
  var pick_top = 0;

  function gmPick(event) {
    const bounding = canvasDiv.getBoundingClientRect();
    pick_x = Math.round(event.clientX - bounding.left);
    pick_y = Math.round(event.clientY - bounding.top);
    pick_left = viewOffsetX;
    pick_top = viewOffsetY;
  }

  function gmMouseMove(event) {
    const bounding = canvasDiv.getBoundingClientRect();
    let x = Math.round(event.clientX - bounding.left);
    let y = Math.round(event.clientY - bounding.top);
    if (event.buttons) {
      const fScale = scaleCur / 100;
      let newOffsetX = pick_left + (x - pick_x) * -1;
      let newOffsetY = pick_top + (y - pick_y) * -1;

      // Clamp offsets to prevent panning beyond map bounds
      const maxOffsetX = ofsCanvas.width * fScale - fixedWidth;
      const maxOffsetY = ofsCanvas.height * fScale - fixedHeight;
      viewOffsetX = Math.max(0, Math.min(newOffsetX, maxOffsetX));
      viewOffsetY = Math.max(0, Math.min(newOffsetY, maxOffsetY));

      gmDrawScaledCanvas(scaleCur); // Redraw to update view
    } else {
      const fScaleCur = scaleCur / 100;
      const mapX = viewOffsetX / fScaleCur + x / fScaleCur;
      const mapY = viewOffsetY / fScaleCur + y / fScaleCur;
      let pos = gmFromMercatorPoint(mapX, mapY);

      x = pos[0] / 2 + 90;
      y = 90 - pos[1];
      if (x >= 0 && x < 180 && y >= 0 && y < 180 && Math.abs(pos[1]) <= 82) { // matches img.src's own coverage
        let gridId = "";
        gridId += String.fromCharCode(65 + Math.round(x) / 10);
        gridId += String.fromCharCode(65 + 18 - Math.round(y) / 10);
        gridId += String.fromCharCode(48 + Math.round(x) % 10);
        gridId += String.fromCharCode(48 + 9 - Math.round(y) % 10);

        infoSpan.textContent = '(' +
          pos[0].toFixed(4) + ',' + pos[1].toFixed(4) + ') ' + gridId;
      } else {
        infoSpan.textContent = '(' +
          pos[0].toFixed(4) + ',' + pos[1].toFixed(4) + ')';
      }
    }
  }

  function gmZoomToScale(x, y, scale) {
    if (scale < scaleMin || scale > scaleMax) {
      console.log(`no scale ${scale} min ${scaleMin} max ${scaleMax}`);
      return;
    }

    const fScaleCur = scaleCur / 100;
    const mapX = viewOffsetX / fScaleCur + x / fScaleCur;
    const mapY = viewOffsetY / fScaleCur + y / fScaleCur;

    scaleCur = scale; // Update scale before drawing
    const newScale = scale / 100;

    // Adjust view offset to keep the same map point under the mouse
    viewOffsetX = mapX * newScale - x;
    viewOffsetY = mapY * newScale - y;

    // Clamp offsets to prevent panning beyond map bounds
    const maxOffsetX = ofsCanvas.width * newScale - fixedWidth;
    const maxOffsetY = ofsCanvas.height * newScale - fixedHeight;
    viewOffsetX = Math.max(0, Math.min(viewOffsetX, maxOffsetX));
    viewOffsetY = Math.max(0, Math.min(viewOffsetY, maxOffsetY));

    gmDrawScaledCanvas(scale);
  }

  function gmMouseZoom(event) {
    const bounding = canvasDiv.getBoundingClientRect();
    const x = Math.round(event.clientX - bounding.left);
    const y = Math.round(event.clientY - bounding.top);
    event.preventDefault();
    let scale = scaleCur;
    if (event.deltaY > 0)
      scale -= scaleStep;
    else
      scale += scaleStep;
    gmZoomToScale(x, y, scale);
  }

  function gmSliderZoom(event) {
    gmZoomToScale(fixedWidth / 2, fixedHeight / 2, parseInt(slider.value));
  }

  function gmLogXYPos(longitude, latitude) {
    const point = projection.forward([longitude, latitude]);
    console.log(`Mercator-projection: (${longitude},${latitude}) x = ${point[0]}, y = ${point[1]}`);
  }

  function gmGridIdLogged(gridId) {
    gridsSeenLogged.add(gridId);
    if (showGridsSeen) {
      if (!gridsSeenJustLogged.has(gridId)) {
        gmShowGridId(gridId, "darkgreen");
        gmDelayedRefresh();
      }
    }
  }

  function gmGridIdNotLogged(gridId) {
    gridsSeenNotLogged.add(gridId);
    if (showGridsUnlogged) {
      gmShowGridId(gridId, "rgb(247, 247, 38)");
      gmDelayedRefresh();
    }
  }

  function gmGridIdJustLogged(gridId) {
    gridsSeenJustLogged.add(gridId);
    gridsLogged.add(gridId);
    if (showGridsSeen) {
      gmShowGridId(gridId, "red");
      gmDelayedRefresh();
    }
  }

  // Draws one station's box+label -- the one place both actually get
  // drawn, shared by gmTouchStation() (incremental, the instant a
  // station is newly seen or its grid changes) and gmRedrawStations()
  // (bulk, after a full canvas rebuild).
  function gmDrawStation(callsign, gridId) {
    gmShowGridId(gridId, "rgb(247, 247, 38)");
    gmLabelCallsign(gridId, callsign);
  }

  // User's own ask (2026-08-25): the one call site index.html now uses
  // any time it resolves a callsign<->grid pair confidently enough to
  // display it (CQ/directed-message decodes, a worked station from the
  // logbook, a third-party pair, the persistent server-side lookup
  // response) -- replaces the older gmEnsureGridShown()/hasVisibleBox()
  // pair, which had no concept of staleness at all. Refreshes lastSeen
  // on every call (not just the first) so a station actively being
  // heard never expires out from under itself; only redraws when
  // actually new or the grid changed, same idempotence the old
  // function had.
  function gmTouchStation(callsign, gridId) {
    const existing = stationLastSeen.get(callsign);
    const isNew = !existing || existing.grid !== gridId;
    stationLastSeen.set(callsign, {grid: gridId, lastSeen: Date.now()});
    if (isNew) {
      gmDrawStation(callsign, gridId);
      gmDelayedRefresh();
    }
  }

  // Redraws every still-tracked station in one pass -- called from
  // reloadGridMap() (a full rebuild already wipes the canvas back to
  // the bare base image) so this is the ONLY place a station's box/
  // label gets erased: never redrawn here means it silently stops
  // appearing once the rebuild finishes.
  function gmRedrawStations() {
    stationLastSeen.forEach(function (info, callsign) {
      gmDrawStation(callsign, info.grid);
    });
  }

  // Runs on a timer (see setInterval below) -- a baked box/label can't
  // be selectively erased any other way than rebuilding the whole
  // canvas, so this only actually reloads when something really did
  // expire, not on every tick.
  function gmPruneStaleStations() {
    const now = Date.now();
    let changed = false;
    stationLastSeen.forEach(function (info, callsign) {
      if (now - info.lastSeen > STATION_TTL_MS) {
        stationLastSeen.delete(callsign);
        changed = true;
      }
    });
    if (changed)
      reloadGridMap(false);
  }
  setInterval(gmPruneStaleStations, STATION_SWEEP_MS);

  function clickShowRoundDots() {
    use_square_dots = !use_square_dots;
    setBtnsStateEnable(false);
    reloadGridMap();
  }

  function clickGridsLogged() {
    showGridsLogged = !showGridsLogged;
    setBtnsStateEnable(false);
    reloadGridMap();
  }

  function clickGridsSeen() {
    showGridsSeen = !showGridsSeen;
    if(showGridsSeen) {
      showGridsUnlogged = true;
    }
    setBtnsStateEnable(false);
    reloadGridMap();
  }

  function clickGridsUnlogged() {
    showGridsUnlogged = !showGridsUnlogged;
    setBtnsStateEnable(false);
    reloadGridMap();
  }

  function gmMarkSeenGridIds() {
    let setIterator = gridsSeenLogged.entries();
    for (const gridId of setIterator) {
      gmShowGridId(gridId[0], "darkgreen");
    }
    setIterator = gridsSeenJustLogged.entries();
    for (const gridId of setIterator) {
      gmShowGridId(gridId[0], "red");
    }
  }

  function gmMarkUnloggedGridIds() {
    let setIterator = gridsSeenNotLogged.entries();
    for (const gridId of setIterator) {
      gmShowGridId(gridId[0], "rgb(250, 250, 38)");
    }
    setIterator = gridsSeenJustLogged.entries();
    for (const gridId of setIterator) {
      gmShowGridId(gridId[0], "red");
    }
  }

  function gmMarkLoggedGridIds() {
    const setIterator = gridsLogged.entries();
    for (const gridId of setIterator) {
      gmShowGridId(gridId[0], "green");
    }
  }

  let refreshPending = false;

  function gmRefresh() {
    refreshPending = false;
    gmDrawScaledCanvas(scaleCur);
  }

  function gmDelayedRefresh() {
    if (!refreshPending) {
      refreshPending = true;
      setTimeout(gmRefresh, 1.0 * 1000);
    }
  }

  function reloadGridMap(do_center) {
    ofsCanvas.width = img.width;
    ofsCanvas.height = img.height;
    ofsCtx.drawImage(img, 0, 0);
    img.style.display = "none";
    console.log(`Map dimensions: ${img.width}x${img.height}`); // Debug

    if (do_center) {
      // Center map horizontally at 30% scale
      const fScale = scaleCur / 100; 
      const scaledWidth = ofsCanvas.width * fScale;
      viewOffsetX = (scaledWidth - fixedWidth) / 2; // Center horizontally
      viewOffsetY = 0; // Top-aligned, as in original
    }

    if (showGridsLogged) {
      gmMarkLoggedGridIds();
    }
    if (showGridsSeen) {
      gmMarkSeenGridIds();
    }
    if (showGridsUnlogged) {
      gmMarkUnloggedGridIds();
    }
    gmRedrawStations();
    gmDrawScaledCanvas(scaleCur);
    setBtnsStateEnable(true);
  }

  // ZBITXD LOCAL CHANGE (2026-08-24): grids.txt is unfiltered (every
  // band, every grid ever logged) -- replaced entirely by index.html's
  // own band-scoped equivalent (gridmap_apply_band() there, fed from
  // the client's already-existing full historical 'QSO' stream).
  // gmLoadGridIds() itself is left defined below but no longer called
  // automatically, so it can't race the band-scoped feed and
  // repopulate the unfiltered gridsLogged set out from under it the
  // moment that network fetch resolved. gridIdsLoaded now defaults to
  // true (rather than only becoming true once grids.txt's callback
  // ran) so the base map image's own load handler below still triggers
  // the first real reloadGridMap() on its own, independent of this.
  let gridIdsLoaded = true;
  let worldMapLoaded = false;

  function gmLoadGridIds() {
    const rnd = "?x=" + Math.floor(Math.random() * 10000);
    jQuery.get("grids.txt" + rnd, function (data) {
      let gridId = "";
      for (let i = 0; i < data.length; i++) {
        gridId += data[i];
        if (gridId.length == 4) {
          gridsLogged.add(gridId);
          gridId = "";
        }
      }
      gridIdsLoaded = true;
      if (worldMapLoaded) {
        reloadGridMap(true);
      }
    });
  }

  img.addEventListener("load", () => {
    worldMapLoaded = true;
    if (gridIdsLoaded) {
      reloadGridMap(true);
    }
  });

  btnShowRoundDots.addEventListener("click", clickShowRoundDots);
  // ZBITXD LOCAL CHANGE (2026-08-24): 'change' only fires on release for
  // a range input -- looked like the slider "didn't work" next to the
  // wheel zoom's immediate feedback. 'input' fires continuously while
  // dragging, matching the wheel's responsiveness.
  slider.addEventListener('input', gmSliderZoom, false);
  btnGridsLogged.addEventListener("click", clickGridsLogged);
  btnGridsSeen.addEventListener("click", clickGridsSeen);
  btnGridsUnLogged.addEventListener("click", clickGridsUnlogged);
  canvasDiv.addEventListener("wheel", gmMouseZoom, { passive: false });
  canvasDiv.addEventListener("mousemove", (event) => gmMouseMove(event));
  canvasDiv.addEventListener("mousedown", (event) => gmPick(event));

  // ZBITXD LOCAL CHANGE (2026-08-24): new function, see fixedWidth's
  // own comment. Applies the same width/height treatment gmBuildHtml()
  // does once at startup, then redraws -- gmDrawScaledCanvas()'s own
  // clamping (sourceWidth/sourceHeight/maxOffsetX/maxOffsetY) already
  // reads fixedWidth/fixedHeight fresh every call, so updating those
  // vars first is enough for it to self-correct viewOffsetX/Y to the
  // new size on its own.
  function gmResize(w, h) {
    if (!containerDiv || w <= 0 || h <= 0)
      return;
    fixedWidth = w;
    fixedHeight = h;
    onsCanvas.width = fixedWidth;
    onsCanvas.height = fixedHeight;
    canvasDiv.style.width = fixedWidth + "px";
    canvasDiv.style.height = fixedHeight + "px";
    gmDrawScaledCanvas(scaleCur);
  }

  // ZBITXD LOCAL CHANGE (2026-08-24): new function, user's own ask --
  // scaleCur's hardcoded 25% starting value was calibrated for the
  // original fixed 520x360 canvas; now that fixedWidth/fixedHeight
  // track the panel's real (often much larger) size, 25% at a bigger
  // canvas requests a source rectangle wider than the actual world
  // image (ofsCanvas), so the map rendered smaller than the canvas
  // instead of filling it -- looked like a canvas/panel size mismatch
  // even though the canvas itself was already sized correctly.
  //
  // This canvas has no letterbox/contain behavior -- gmDrawScaledCanvas()
  // always stretches whatever source rectangle it's given to exactly
  // fill fixedWidth x fixedHeight, so a source rect that exceeds the
  // image's actual bounds on either axis is what leaves the map not
  // occupying the whole panel. Guaranteeing *neither* axis's source
  // rect ever exceeds the image means using the LARGER of the two
  // axis ratios (crop-to-fill/"cover"), not the smaller ("contain") --
  // first version of this function had that backwards. Then centers
  // whichever axis ends up with slack (the one that isn't the crop
  // boundary) -- same idea reloadGridMap()'s own do_center already
  // used, just computed instead of assumed. Called once on panel open
  // (index.html gridmap_open()), not on every resize, so an in-progress
  // pan/zoom the operator is actively using doesn't get reset out from
  // under them just from dragging the panel's edge.
  function gmFitToCanvas() {
    if (!ofsCanvas.width || !ofsCanvas.height || fixedWidth <= 0 || fixedHeight <= 0)
      return;
    // Real report, live (2026-08-25): the exact cover-fit scale leaves
    // *zero* pan slack on whichever axis is the tighter fit -- for this
    // nearly-square map image in a wider-than-tall panel, that's width,
    // so panning worked vertically but not horizontally at all right
    // after opening. A small extra margin keeps the map fully filling
    // the panel (the whole point of the cover fit) while leaving real
    // room to shift the view left/right or up/down without having to
    // zoom in manually first -- e.g. recentering off the default
    // Atlantic-ish view toward the US or toward EU depending on which
    // side of the band is actually active tonight.
    var fit = 1.15 * 100 * Math.max(fixedWidth / ofsCanvas.width, fixedHeight / ofsCanvas.height);
    fit = Math.max(scaleMin, Math.min(scaleMax, fit));
    scaleCur = fit;
    var fScale = fit / 100;
    var scaledWidth = ofsCanvas.width * fScale;
    var scaledHeight = ofsCanvas.height * fScale;
    viewOffsetX = Math.max(0, (scaledWidth - fixedWidth) / 2);
    viewOffsetY = Math.max(0, (scaledHeight - fixedHeight) / 2);
    gmDrawScaledCanvas(fit);
  }

  // ZBITXD LOCAL CHANGE (2026-08-24): new function -- see index.html's
  // gridmap_apply_band() for why this exists (per-band data scoping).
  // Clears the *data* (all four tracking sets) but not the Logged/
  // Seen/Unlogged/Round display-preference toggles, which are the
  // operator's own standing choice, not something a band change should
  // reset. reloadGridMap(false) is the same redraw gmGridIdLogged() et
  // al. already trigger elsewhere -- necessary because the dots are
  // painted directly onto ofsCanvas (gmSetGridMark's fillRect calls),
  // not a separate layer that could be cleared independently; the only
  // way to actually erase old marks is to redraw the base map image
  // fresh and re-mark from whatever's left in the (now-empty, until
  // the caller re-feeds it) sets.
  function gmClearSeen() {
    gridsSeenLogged.clear();
    gridsSeenNotLogged.clear();
    gridsSeenJustLogged.clear();
    gridsLogged.clear();
    // Third-party pairs are band-scoped the same as everything else
    // here -- index.html re-feeds its own per-band memory of these
    // after a band change (gridmap_apply_band()'s own comment), same
    // pattern as the grid dot sets above.
    thirdPartyPairs.clear();
    // Can now fire as early as the very first band-change event (see
    // index.html's FREQ case), possibly before the ~1.1MB map image
    // has finished loading over the network. reloadGridMap() assumes
    // img is already loaded (sizes ofsCanvas from img.width/height) --
    // skip the redraw here if it isn't; the sets are empty anyway at
    // that point, and img's own load handler below already calls
    // reloadGridMap(true) once it's genuinely ready, which is a
    // real (not skipped) redraw.
    if (worldMapLoaded)
      reloadGridMap(false);
  }

  // ZBITXD LOCAL CHANGE (2026-08-24): shows/updates the QSO line --
  // myGrid/theirGrid are plain grid IDs (validated here, not assumed
  // valid), txActive is whatever truthy/falsy value the caller's own
  // "am I transmitting right now" state already is (in_tx, index.html).
  // Safe to call on every relevant state change (CALL/EXCH updating,
  // in_tx toggling) rather than only on genuine transitions -- redundant
  // calls with the same grids/direction are cheap (just reassigns the
  // same values) and starting the animation loop is itself a no-op if
  // it's already running (gmStartQsoLineAnim's own guard).
  function gmSetQsoLine(myGrid, theirGrid, txActive) {
    if (!myGrid || !theirGrid || !gmIsValidGridId(myGrid) || !gmIsValidGridId(theirGrid)) {
      gmClearQsoLine();
      return;
    }
    qsoLineActive = true;
    qsoLineMyGrid = myGrid;
    qsoLineTheirGrid = theirGrid;
    qsoLineForward = !!txActive;
    gmStartQsoLineAnim();
  }

  // Stops the animation loop (it stops itself on its own next tick too,
  // per gmStartQsoLineAnim()'s own check -- called directly here as
  // well so the line disappears immediately on this redraw rather than
  // lingering for up to one more qsoLineStepMs tick) and redraws once
  // more without it.
  function gmClearQsoLine() {
    qsoLineActive = false;
    qsoLineMyGrid = null;
    qsoLineTheirGrid = null;
    if (qsoLineTimer) {
      clearInterval(qsoLineTimer);
      qsoLineTimer = null;
    }
    gmDrawScaledCanvas(scaleCur);
  }

  // ZBITXD LOCAL CHANGE (2026-08-24): records/refreshes one third-
  // party pair. callA/callB order doesn't matter -- the key is sorted
  // so "K1ABC,W2XYZ" and "W2XYZ,K1ABC" are always the same entry
  // rather than silently creating two. Cheap and safe to call
  // redundantly (index.html calls this on every relevant decode, not
  // just the first time a pair is seen) -- existing entries just get
  // their lastSeen refreshed and their grids re-confirmed.
  // fromCall: whichever of callA/callB actually sent *this* particular
  // decoded message -- used to orient the static direction arrow (see
  // gmDrawThirdPartyLines()'s own comment). Redraws on a genuinely new
  // pair or a direction change (a real QSO's own natural back-and-
  // forth); a same-direction refresh (lastSeen only) is cheap and
  // doesn't force one, since nothing currently on screen would
  // actually change.
  function gmTrackExchange(callA, gridA, callB, gridB, fromCall) {
    if (!callA || !callB || !gmIsValidGridId(gridA) || !gmIsValidGridId(gridB))
      return;
    const key = callA < callB ? (callA + "," + callB) : (callB + "," + callA);
    const existing = thirdPartyPairs.get(key);
    const resolvedFrom = fromCall || (existing && existing.fromCall) || callA;
    const changed = !existing || existing.fromCall !== resolvedFrom;
    thirdPartyPairs.set(key, {
      callA: callA, gridA: gridA, callB: callB, gridB: gridB,
      fromCall: resolvedFrom, lastSeen: Date.now()
    });
    gmStartThirdPartyAnim();
    if (changed)
      gmDrawScaledCanvas(scaleCur);
  }

  // Animates every third-party arrow (one shared thirdPartyT phase,
  // see its own comment) and prunes stale pairs, both in the same
  // tick -- user's own ask, live: "preferably moving" arrows, plus a
  // TTL tight enough (thirdPartyTtlMs) that this needs to check often
  // to actually keep up with it, not the separate/slower prune-only
  // timer this used to be. Same qsoLineStepMs cadence as my own line's
  // animation for consistency -- cheap regardless of how many pairs
  // exist (each tick is just a Map iteration + a few canvas draw calls
  // per pair, nothing like the per-pixel cost that pegged this same
  // device's GPU once before). Self-stops the same way
  // gmStartQsoLineAnim() does (nothing left to animate, or the panel
  // isn't visible) rather than ticking forever in the background.
  function gmStartThirdPartyAnim() {
    if (thirdPartyTimer)
      return;
    thirdPartyTimer = setInterval(function () {
      if (thirdPartyPairs.size === 0 || !containerDiv || containerDiv.offsetParent === null) {
        clearInterval(thirdPartyTimer);
        thirdPartyTimer = null;
        return;
      }
      thirdPartyT += 0.04;
      if (thirdPartyT > 1)
        thirdPartyT -= 1;
      const now = Date.now();
      thirdPartyPairs.forEach(function (entry, key) {
        if (now - entry.lastSeen > thirdPartyTtlMs)
          thirdPartyPairs.delete(key);
      });
      // Unconditional now (not just "if anything was pruned") -- the
      // arrow position itself changes every tick regardless.
      gmDrawScaledCanvas(scaleCur);
    }, qsoLineStepMs);
  }

  return {
    init: function (moduleDiv, w = 520, h = 360) {
      width = w;
      height = h;
      containerDiv = moduleDiv;
      gmBuildHtml();
    },
    redraw: function () { gmDrawScaledCanvas(scaleCur); },
    resize: gmResize,
    fitToCanvas: gmFitToCanvas,
    clearSeen: gmClearSeen,
    setQsoLine: gmSetQsoLine,
    clearQsoLine: gmClearQsoLine,
    trackExchange: gmTrackExchange,
    labelCallsign: gmLabelCallsign,
    setGridDot: gmSetGridDot,
    showGridId: gmShowGridId,
    markPlace: gmMarkPlace,
    isValidGridId: gmIsValidGridId,
    gridIdLogged: gmGridIdLogged,
    gridIdNotLogged: gmGridIdNotLogged,
    gridIdJustLogged: gmGridIdJustLogged,
    touchStation: gmTouchStation,
  };
})();