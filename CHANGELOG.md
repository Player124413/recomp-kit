# Changelog

## Unreleased

- A JMP through the entry stack slot now ends the way a RET does. The proof
  behind that emission establishes where the jumped-to value came from, never
  what it is, and a block recovered as a function of its own begins at delta
  zero holding whatever its real caller pushed. Delphi's finally idiom - PUSH
  resume; CALL cleanup; POP EAX; JMP EAX - puts a continuation INTO the
  establishing body there, and setting EIP and returning dropped it: the body's
  epilogue never ran, so it never restored EBP, and its caller went on reading
  its own locals through a frame pointer that had moved. That surfaced as a
  window painting nothing, four blits away, with every handle in the call
  reading as rubbish. Routing it through recomp_return leaves a genuine return
  exactly as cheap as it was and dispatches the rest.

- `RECOMP_WATCH=<hex address>[:<length>]` reports every guest write that
  touches those bytes, and `RECOMP_WATCH_FRAME=1` reports a guest call that
  returns with EBP changed. A routine that loses the frame pointer corrupts
  nothing and crashes nowhere: its caller simply reads its own locals from
  somewhere else afterwards, and the damage surfaces as a wrong value in an
  unrelated place, which is the hardest kind of fault to work backwards from.
  The frame report names the call and the EIP the callee left off at, so the
  routine that did it is read off the log rather than deduced. Both are off by
  default; unarmed they cost one compare that is never taken.

- A configured entry point is never withdrawn again. This file's own policy
  says a `[translate] entry_points` address is established code, and `resolve`
  duly adopts it - but `extend_finally_body` then read a PUSH of that address
  inside another function's span as naming a pushed cleanup continuation,
  re-decoded the bytes into the enclosing body, and retired the configured body
  into an alternate, which emits no dispatch entry at all. The address the port
  had verified was simply absent from the translation, and the only symptom was
  a call to it returning zero. For Delphi that PUSH is how a window procedure is
  handed to `MakeObjectInstance`, so a form's every message - WM_PAINT included
  - ran nowhere. An address that a jump table, `__initterm` or config names is
  now neither absorbed as a continuation nor retired, and a declared entry that
  still fails to be adopted says so rather than going missing quietly.

- A media session posts `MESessionTopologyStatus` carrying
  `MF_TOPOSTATUS_READY`. A player does its renderer setup from that event and
  from nowhere else: it is where `MFGetService` is called for
  `IMFVideoDisplayControl`, where `SetVideoWindow` is called, and where the
  interface every later repaint goes through is stored. A session that posted
  only `MESessionTopologySet` left that field nil, and the failure surfaced as
  a call through a nil interface inside the player, nowhere near this layer -
  the video was decoding and presenting the whole time, through this layer's
  own path rather than the renderer the player thought it had. `MFGetService`
  now also names the object and the service asked of it under
  `RECOMP_MF_TRACE`, because a refusal there is invisible until something calls
  through what it did not return.

- A refused blit reports its geometry and the frame that asked for it, and
  `RECOMP_TRACE_GDI=1` reports every blit, the DC every window paint hangs off
  (`BeginPaint`, and whether the window was one this layer knows),
  `CreateCompatibleDC`, and a `BeginBufferedPaint` that this layer declines.
  Between them they say which painting path a program took, which is the
  question a blank window actually poses. A destination with no bitmap behind
  it and a source of zero are indistinguishable from the guest, which is told
  nothing either way; the geometry says which it was, and the caller chain says
  who asked. It is how "the picture was never drawn" is told apart from "the
  picture was drawn and lost".

- A call to an address the translation does not cover now reports the caller's
  registers. For an indirect call they say what the call was made ON: a virtual
  dispatch reached its target through a word in the object, so the registers
  separate "the vtable slot is empty" from "the pointer is not an object at
  all" - a distinction the address alone cannot make, and one that turned an
  unexplained null call into a pointer that was never an object, its first
  word being two characters of text.

- `RECOMP_HEAP_QUARANTINE=1` retires a freed guest block instead of returning
  it to the free list, and lets no neighbour absorb it. A guest that keeps
  using memory it has freed then reads its own dead object rather than
  whatever was allocated over the top of it, so a use-after-free faults where
  it is rather than wherever the reused block happens to be written next. It
  answers the question either way: a fault that survives quarantine unchanged
  was never a reuse at all. Off by default, and a run with it on never
  recycles a byte, so it is a diagnostic and not a way to play.

- Media Foundation plays a file. `dx/mf.cpp` puts the objects a player builds -
  source resolver, media source, presentation and stream descriptors, media
  type handler, topology and its nodes, renderer activates, media session,
  events, async results, presentation clock, `IMFVideoDisplayControl`,
  `IMFAudioStreamVolume` and `MFGetService` - over the FFmpeg reader in
  `mf_media.cpp`. The topology is recorded rather than resolved: the session
  plays the source its nodes name, which it recognises by the objects handed to
  `SetUnknown` rather than by the attribute GUID naming them, so it does not
  depend on knowing `MF_TOPONODE_SOURCE`. What a player actually observes is
  the event order, so `MESessionTopologySet`, `MESessionStarted`,
  `MESessionPaused`, `MESessionStopped`, `MESessionEnded` and
  `MESessionClosed` are posted as they happen and collected through the
  `IMFAsyncCallback` the player armed with `BeginGetEvent`. `Close` delivers
  its event before returning, because a player blocks on an event it sets from
  its own `Invoke` and the frame pump cannot run while that wait holds the
  guest thread.

- A playing session owns the screen. Video is decoded to the wall clock and
  presented at the guest's own display mode - aspect kept, the remainder
  black - never at the file's resolution, because the host maps pointer
  coordinates back through whatever was last presented. While a session is on
  screen the DirectDraw primary, the GDI window present and GDI window
  compositing all stand aside (`mf_owns_the_screen`, weak in `runtime/`, strong
  in `dx/`), and the screen goes back to the game the moment the file ends.
  Without the compositing half of that the movie was presented and then painted
  out by the game's own black window, which is what a real renderer's window
  would have been sitting in front of.

- `ole32!PropVariantClear` and `PropVariantCopy`. Both are delay imports for a
  Delphi Media Foundation player, and an unresolved delay import is not quiet:
  the stub raises 0xC06D007F, which surfaces as an external-exception dialog
  and takes the process with it. Every PROPVARIANT the shims produce is
  VT_EMPTY, so emptying the sixteen bytes is the whole of clearing one.

- The vendored FFmpeg decodes MS-MPEG-4 part 2 (`msmpeg4v1,v2,v3`). A `.wmv`
  from the Windows Media Encoder era usually carries fourcc MP43 rather than a
  WMV-numbered codec, and the demuxer that reads the container is no use
  without the decoder that reads the frames.

- Diagnostics: `RECOMP_MF_TRACE` narrates a session (what opened, what the
  topology named, every event, whether the video keeps up with the clock), and
  `RECOMP_PRESENT_TRACE` names every frame reaching the screen by geometry,
  depth and whether it is blank - which is how one presenter is told from
  another when two of them reach the same screen.

- A fullscreen DXGI swap chain sets the mode USER32 reports. `GetSystemMetrics`
  and everything else reading the virtual screen took the DirectDraw mode or
  the default desktop, so a game that switched to 1920x1080 through Direct3D
  still laid out and hit-tested against the mode before the switch: in Siege's
  character creator the pop-up lists sat beyond the old 1024-pixel width and
  no click on them landed. Releasing fullscreen hands the screen back to the
  DirectDraw mode.

- DirectDraw locks are cheaper when a guest locks a whole surface to change a
  little of it, which is how a DXR text draw works and why hovering a menu's
  text crawled. The retained-pointer hash takes eight bytes a step instead of
  one (and still catches any changed word, tail bytes included), write-lock
  shadows reuse earlier buffers instead of zero-filling a new one, and the
  unlock diff masks only the band of rows that changed. On a creator hover
  script the text routine's share fell from 23.8 s to 4.6 s; the records it
  produces are unchanged.

- A display mode larger than the screen no longer opens a window larger than
  the screen. The desktop host sized its window at a whole multiple of the
  mode in points, never below one, and made the mode its minimum size, so a
  1920x1080 mode opened a 1920x1080-point window on a 1512x982-point Retina
  laptop and could not be shrunk. Such a mode now takes the largest whole
  multiple of its frame in drawable pixels that fits the usable screen -
  960x540 points there, every guest pixel one drawable pixel - or, when none
  fits, the largest size that does, with the minimum never above the window.
  Modes that fit keep the size they had (`host_window_size_for`).

- Fonts a program registers with `AddFontMemResourceEx` are drawn with.
  The data used to be discarded and every string drawn in the fixed 8x16
  cells; now the fonts are kept and rasterized with stb_truetype (vendored,
  MIT or public domain), and a DC whose font names one measures by its
  advances and metrics - `GetTextExtentPoint32W`, `GetTextMetricsW`,
  `DrawTextW` wrapping and alignment - and draws anti-aliased outlines in
  `ExtTextOutW`, `DrawTextW` and the themed text calls. A face the program
  did not register keeps the bitmap cells; the kit ships no fonts. The tests
  use a generated two-glyph font (`runtime/tests/make_test_font.py`).

- The system `STATIC` control class. Windows draws a static control's text
  in the class's own window procedure, and a program that subclasses it -
  the VCL's `TStaticText` does, through `GetClassInfoW` - hands it every
  message it does not handle. With no such class the lookup failed, the
  subclass fell back to `DefWindowProc`, and every caption stayed blank. The
  procedure keeps `WM_SETFONT`, redraws on `WM_SETTEXT`, asks the parent for
  colours with `WM_CTLCOLORSTATIC`, and paints into the DC `WM_PAINT` or
  `WM_PRINTCLIENT` carries when there is one: a double-buffering program
  paints its control into a memory DC and copies that over the window, so
  painting the window directly was covered by the copy.

- `SetWindowPos` shows and hides. `SWP_SHOWWINDOW` and `SWP_HIDEWINDOW` are
  the transitions `ShowWindow` makes, and the VCL shows every child control
  with the first and never calls `ShowWindow` for one, so each stayed hidden
  and was never asked to paint.

- `WS_CLIPCHILDREN`. A child window's DC writes into its top-level window's
  surface, so a parent repainting painted over its visible children; a DC of
  a window with the style now leaves their areas alone, as Windows does.

- Visual styles are on, with no theme data. `IsThemeActive` and
  `IsAppThemed` answer yes, as every Windows since Vista does; `OpenThemeData`
  finds nothing, so controls draw the classic way. Every uxtheme export a
  VCL program binds is present, since it calls through whatever pointer it
  got and a delay-loaded one that is missing raises instead.
  `DrawThemeParentBackground` does the real work - the parent paints its
  background into the child's DC through `WM_ERASEBKGND` and
  `WM_PRINTCLIENT`, which is how a transparent control shows the window
  behind it - and `DrawThemeText`/`DrawThemeTextEx` draw their text in the
  DC's font rather than failing, or a program with styles on loses its
  captions. Buffered painting and animation report no buffer.

- `comctl32.dll` has a version resource: 6.10 when the executable's manifest
  binds `Microsoft.Windows.Common-Controls` 6.0, else 5.82. The VCL decides
  whether to paint with visual styles by that number.

- `CharUpperBuffA`, `CharLowerBuffA`, `GetStringTypeExA`, `GetStringTypeExW`
  and `FlushInstructionCache`. A Delphi runtime built this decade builds its
  ANSI case tables from inside a unit's initialization by running every
  byte value through the first two, and an import the kit does not know is
  called with its arguments left on the stack: two calls a byte, eight bytes
  a call, and the unit-init loop popped its own counter back as garbage and
  stopped with more than half the program's units never initialized. It was
  the PNG reader's chunk registry that made it visible - "unknown but
  necessary chunk" on a perfect file - but every unit after the one building
  the tables was missing.

- A pushed continuation is decoded and dispatchable. Delphi leaves a
  finally block with `PUSH continuation; ...; POP reg; JMP reg`; the jump
  dispatches on a variable, so nothing names the continuation - not the
  dangling-target check, not recursive descent, which cannot follow a push.
  A body that consumes a pushed address that way now grows into it when its
  listing stopped short, and the address becomes a block entry as a
  jump-table target does, so `recomp_jump` reaches it from any body that
  shares the code - a recovered block can overlap a listed function's grown
  tail and execute the same jump without holding the label. A push followed
  by `PUSH FS:[..]` is a try frame's handler and is left to that recovery.

- `DirectInputCreateW` and `DirectInputCreateEx`. A Unicode program asks for
  the W entry and, refused, runs with no DirectInput at all and reads its
  mouse some slower way. The W object is the A object remembering that
  `DIDEVICEINSTANCEW` carries its two names as 260 UTF-16 units each, at 40
  and 560, in an 1100-byte record - which `GetDeviceInfo` used to reject as
  too large. The Ex entry names the interface by IID; the W IIDs are the A
  ones plus one, and an unknown IID is E_NOINTERFACE.

- The guest's main stack is 8 MB, not 1 MB. Siege of Avalon's 1.19 launcher
  exhausted a megabyte on Play even though the executable reserves only a
  megabyte on Windows: the same code runs deeper under the kit than
  natively, for a reason not yet established, so the guest gets headroom
  rather than the program's own reserve. The region below the stack was
  unused; worker stacks come from the heap.

- Every import the Siege of Avalon 1.19 image names now has an argument
  count: `SafeArrayAccessData`, `SafeArrayUnaccessData`, `ValidateRect` and
  `GetUpdateRect` do what they say, `LoadCursorFromFile{A,W}` hand out a
  handle, and sixteen more are logging-only entries with the right stdcall
  count, from `CombineRgn` to `URLDownloadToFileW`. The runtime's coverage
  gate - every named import must have a count - is the check that would have
  caught the case-table bug before it cost anything, and it is green again.

- A raise the guest goes on to handle is logged at the verbose level with the
  frames it climbed out of. A language exception hides exactly that, and the
  run log used to show only the dialog that reported it.

- Three more shapes of the listing defect `--allow-unmodelled` exists for.
  A direct call whose literal target is not in the image at all becomes a
  trap at its own address; the jump-table pass gets the same tolerance the
  emit path has, so a table that cannot be decoded because one of its own
  instructions cannot be modelled no longer fails the build; and a literal
  dispatch target that no instruction boundary agrees with - a jump decoded
  out of padding, landing inside a real instruction - becomes the trap a
  withdrawn block gets. Without the switch each one still fails the build.

- A SAFEARRAY may have more than one dimension. `SafeArrayCreate` refused
  anything else and the header it allocated was a fixed 24 bytes, so a guest
  that wrote `array[x, y]` got a null array back and had to abandon whatever
  it was building. The header is 16 bytes plus one bound per dimension, and
  creation, validation, copying, element addressing and the two bound queries
  all read `cDims` now. `rgsabound[i]` describes the dimension an index list
  names i'th, which is the order guests use.

- DirectDraw clips a blit that runs off its destination instead of refusing
  it. `Blt` and `BltFast` required the whole rectangle to lie inside the
  surface, so a draw that hung off an edge wrote nothing at all; real
  DirectDraw writes the part that lands, which a game relies on whenever it
  draws a scrolled buffer or a tile page at a border.

- Keyboard input goes to the window with the focus, as Windows sends it.
  Hosts posted it to the first window the guest created, which in a VCL
  application is the invisible application window: every keystroke went
  somewhere that does nothing with one, so a text field could not be typed
  into. The runtime knows which window holds the focus and now routes by it,
  falling back to the active window and then to the first.

- Translator: `--allow-unmodelled REASON` turns an instruction the translator
  cannot model into a trap at its own address instead of refusing the image,
  and reports every one. A listing routinely decodes the data past a
  function's last instruction as code - sixteen-bit addressing and port
  instructions in a thirty-two-bit user-mode image are the signature - and an
  image should not be refused over bytes nothing executes. Without the switch
  such an instruction still refuses the image, and reaching one at run time is
  fatal either way, loudly and with its address.

- An analysis pass no longer crashes a build on a body it cannot parse: a
  function whose instructions will not read is simply not a SEH helper, and
  the translation pass reports it the way it reports every other failure.

- Translator: the port string instructions (INS/OUTS, with and without REP)
  translate instead of failing the build. A user-mode guest never reaches one;
  they appear where a listing misdecodes data as code, and one such byte in a
  startup stub was enough to stop a whole image from translating. They read
  and write through the existing port shims and advance the pointer and count
  exactly as the other string forms do.

- Native overrides are reachable from a game: game.toml `[translate]
  overrides` names a header the generated sources include before they define
  FN_<addr>, so a game can replace one translated function with a native one
  and every call site, tail call and jump-table case for that address follows.
  The translator has emitted the hook since Task 8; nothing set it until now.
  A named header that does not exist is an error, because a path that quietly
  failed to resolve would leave a build looking replaced while running the
  original.

- Input: pending mouse moves are coalesced, as Windows does. The routing pump
  delivers one message per call, so a host reporting motion faster than the
  guest pumps built a backlog and the pointer trailed the hand by its length.
  A move between a press and a release is kept, so a drag is unaffected.

- DirectDraw recorder: what a frame retains is bounded. A frame ends when
  something is presented or drawn, so a screen built entirely from blits into
  a back buffer the guest never flips records into one frame indefinitely, and
  every source lease it takes can cost a full copy of those pixels. The oldest
  leases of an unsealed frame are now let go - nothing can have asked for them,
  since only a sealed frame is offered to a presenter - and the recorder keeps
  a bounded window of sealed frames, releasing anything older. This halves an
  observed growth of 15 MB a second on that kind of screen; the remainder is
  still under investigation.

- Diagnostics are bounded. A guest that generates code writes a new routine at
  a new address every time, so a report keyed by that address is a new key on
  every call: the once-only log now caps its key set, the undeliverable-call
  record caps its sample, and a guest thunk the decoder cannot run is reported
  once per SHAPE of code rather than once per address. The last of those was
  also thousands of formatted writes a second on a drawing path.

- Desktop host: mouse messages are routed by position, as the smoke host and
  Windows both do. They were posted to the main window carrying a screen
  position, so every click reached whichever window the guest created first -
  for a Delphi game, the invisible application window - and that window read
  the screen position as its own client one. A full-screen game never noticed;
  a windowed launcher could not be clicked at all.

- Input: a button press and its release are never applied to the guest in the
  same turn. Queued input arrives in batches, so a real click landed as a
  press and a release between two of the guest's polls, and a guest that reads
  its button state rather than the message queue never saw the button down at
  all. The batch is cut before the release and the rest waits a turn.

- Windows: child windows are composited with their parents, clipped to every
  ancestor. Only top-level windows reached the screen before, so a control
  that paints into its own window - which is most of them - was invisible.

- Smoke host: RECOMP_SMOKE_WINDOW_INPUT routes every scripted pointer step
  through the window mapping a real mouse uses, so a host-side input defect
  can be reproduced without a hand on the mouse.

- GDI: blits convert between colour and monochrome instead of matching the
  nearest palette entry. Into a 1-bit bitmap the source's background colour
  becomes white and everything else black; out of one, white takes the
  destination's background colour and black its text colour. That conversion
  builds and uses every transparency mask, so without it a mask came out as a
  luminance map and a transparent draw kept the wrong half of the image.

- user32: SetLayeredWindowAttributes and GetLayeredWindowAttributes. The
  window compositor drops the colour key and applies the constant alpha, so a
  shaped form is drawn as its artwork rather than as a rectangle of the key
  colour.

- Host: the system pointer stays visible until the guest has a display surface
  of its own. While it is still showing plain windows it draws no cursor, so
  hiding the host one left nothing to aim with.

- GDI: MaskBlt honours its mask bitmap instead of refusing every call that
  supplies one. A set mask bit takes the foreground raster operation and a
  clear one the background, which is how the VCL draws a transparent bitmap;
  refusing it lost whole window backgrounds, not single blits.

- Scheduler: `ExitProcess` on the main thread ends every other guest thread.
  No worker runs guest code again, so a host no longer crashes in one running
  on state the guest has already torn down; each is offered the baton once, to
  end, so a host's shutdown drive finishes instead of waiting out its bound.

- Auxiliary guest modules: game.toml `[modules.aux.<key>]` names a DLL the
  runtime maps beside the image at its preferred base (content-hashed, no
  relocation) with `[game] guest_size` growing the arena to hold it.
  `translate.py --module <key>` translates it into `gen/aux-<key>/` with
  prefixed, self-registering tables; the image's dispatch falls back to the
  module registry. LoadLibrary hands out the module's base and runs its
  entry point once, GetProcAddress answers from its export directory, and
  the bundle keeps the DLL regardless of `*.dll` exclusions.

- SEH: adopt registrations left installed by returning compiler helpers
  into a checkpoint in their live caller, including POP/JMP return helpers
  and helpers that fill caller-reserved stack records.

- DirectDraw: expose ANSI and Unicode legacy/extended device enumeration,
  reporting the primary display through callbacks with the correct ABI.

- Translator: recognize POP restores of FS:[0] through a proven zero
  register, including unlink helpers that do not establish their own frame.

- SEH: at verbose logging level, validate the guest chain at import and SEH
  boundaries and report its first invalid link together with the last valid
  observation. The diagnostic is bounded and does not alter guest memory.

- DirectDraw: expose DirectDrawCreateEx with an explicit unsupported result
  for IDirectDraw7, allowing callers to fall back to the legacy factory and
  its supported interfaces without receiving an incompatible vtable.

- Runtime: theme and desktop-composition probes report disabled visual
  styles and composition, allowing callers to use their classic window path.

- Runtime: buffered-paint initialization reports E_NOTIMPL through the
  uxtheme export, with safe cleanup for callers using ordinary GDI painting.

- Runtime: WTS session notification exports report an unavailable session
  service through their normal BOOL/error result instead of a missing DLL.

- Kernel32: GetNativeSystemInfo reports the same 32-bit guest system
  information as GetSystemInfo for delay-loaded platform probes.

- Kernel32: expose en-US thread, user and system preferred UI languages,
  with UTF-16 multi-string size queries and bounded writes for MUI callers.

- Runtime: GetModuleHandleA/W exposes registered DLLs before LoadLibrary,
  sharing stable pseudo-module handles with later loads and GetProcAddress.

- Runtime: configure the reported Windows version with
  `[game] windows_version = "major.minor[.build]"`. GetVersion,
  GetVersionExA/W and VerifyVersionInfoW share that identity. The default
  remains 4.10 (build 2222); 6.1 defaults to build 7601 and Service Pack 1.

- Build: refresh the generated directory's runtime header on incremental
  builds, without regenerating translated sources or touching unchanged files.

- Runtime: RET into a resolved import shim executes the target and its
  normal return, preserving Delphi delay-load calls on their first use.

- USER32: SetWindowPos sends WM_WINDOWPOSCHANGED synchronously, with
  WM_MOVE and WM_SIZE delivered by DefWindowProc, so successive dimension
  changes see the window procedure's updated bounds.

- Smoke mouse input follows visible, enabled windows in stacking order,
  delivers client coordinates and modifier flags, respects capture, and
  asks inactive windows to activate before a button press.

- Smoke: screen metrics, retained GDI captures and pointer bounds share the
  virtual desktop selected by RECOMP_SMOKE_DRAWABLE until DirectDraw sets
  a mode. Script dumps and pointer coordinates assert matching dimensions.

- GDI: DrawTextW and DrawTextExW now paint bitmap-font glyphs with the DC's
  colours, rectangle clipping and basic text layout. Measurement uses the
  selected font, including screen DCs without a backing surface.

- Translator: RET follows pushed interior continuations after cleanup, with
  shared Delphi finally blocks and their epilogues kept in the establishing
  body and exposed through alternate entries for exception dispatch.

- Translator: computed jumps to non-entry CALL continuations return to the
  pending host caller without dispatching again or popping the guest stack.

- Translator: recognize closed shutdown loops as nonreturning so recovery
  stops at their calls before decoding trailing data as instructions.

- Translator: follow adjacent `PUSH imm32; RET` continuations before returning
  to the host caller, while preserving direct entry at a shared `RET`.

- Merge integration: profile tests select the first generated entry and are
  omitted when none exists; ANSI and wide disk-space queries share the same
  virtual disk geometry.

- Build: default FFmpeg ON on Linux, import its major-version shared objects
  and package them beside the executable with an `$ORIGIN` rpath and notice.
  On Windows, detect MSYS2 bash/make and require a MinGW-compatible compiler;
  keep video OFF with a status message when prerequisites are missing or
  the compiler uses the MSVC ABI. Package enabled builds' DLLs and notice;
  remove staged video files on OFF without touching player files. Windows
  CI stays video OFF and Linux needs no new packages. macOS configure and
  fake-file staging checks pass; Linux/Windows builds and playback are unverified.

- Build: enable shared FFmpeg by default on iOS and Android. Cross-build
  arm64 iOS 17 dylibs with relative install names and use Xcode's Embed
  Frameworks phase to copy and sign them with the app's identity/team.
  Cross-build Android API-29 libraries and package the three unversioned
  `.so` files beside `libmain.so`; remove staged copies when video is OFF.
  Include the FFmpeg notice in mobile bundles and document all configure
  flags. Android stub/real APKs and the standalone iOS FFmpeg build pass;
  embedded iOS signatures and mobile device playback remain unverified.

- Bink: decode video through FFmpeg into the DirectDraw surface supplied by
  the game, converting YUV420P to RGB565, RGB555 or XRGB8888. Stream decoded
  audio through the shared mixer, pace frames with host time and return an
  empty error string on success. Pending host close ends the video loop;
  builds without FFmpeg keep the finished-video stub and Smacker stays refused.

- Build: fetch SHA-256-pinned FFmpeg 7.1.1 for macOS with only Bink/Smacker
  decoders and demuxers and file input. `RECOMP_VIDEO` defaults to ON on
  macOS and OFF elsewhere; OFF retains the build without FFmpeg. Link
  avformat, avcodec and avutil dynamically, bundle the three replaceable
  dylibs with relative install names and ad-hoc signatures, and ship the
  LGPL notice, source identity and build flags.

- Touch: taps press and release at the finger's position, including near a
  window edge, without a cursor nudge after release. Held fingers and drags
  retain edge snapping so holding an edge still scrolls; lifting an edge
  hold moves the cursor back inside to stop scrolling.

- Load saved host settings before symbol-table validation, so window mode and
  other profile settings survive relaunch even without a usable symbol table.
  Hide renderer and native Options rows from the fallback settings page until
  symbols are available; retain window mode, frame limit and performance overlay.

- SDL: confine a captured pointer in a plain window as well as borderless
  and fullscreen modes. Click inside to capture, hold Escape to release,
  and drag into the window's resize margin to release capture for resizing.

- Android reads game data from SDL's external files directory under `game/`,
  reads `switches.txt` beside it, and defaults to a writable `profile/` there.
  Missing data logs the expected executable and `adb push` command, then exits.
  Enable fullscreen touch/keypad behavior and background audio/presenter
  suspension; end the process after SDL teardown when the game exits.
  `--push-game` stages the configured install using `[bundle].exclude`, then
  pushes before launch, and fails clearly without a ready Android device.

- Build `--target android` through the NDK preset, then package its
  `libmain.so` with an SDLActivity subclass and the matching FetchContent
  Java sources. Add a Gradle 9.7.1 wrapper and AGP 9.1.1 templates for
  arm64-v8a, API 29 minimum, compile/target SDK 36 and required Vulkan 1.1.
  Install and launch on a ready adb device, stream logcat with `--console`,
  and skip device actions when none is attached. Stub APK packaging is
  verified on macOS; Android device execution remains unverified.

- Add `android` and `android-stub` NDK presets for arm64-v8a, API 29 and
  static libc++. Build the SDL host as `libmain.so` with static SDL3 and
  NDK Vulkan/log libraries, omit desktop tests and add an Android stub CI
  build. Read arm64 Linux/Android page-fault writes from the kernel's ESR
  signal-frame record. APK packaging and device execution are not implemented here.

- Package successful desktop app builds under `build/package`: a Linux
  folder and architecture-named tarball, or a Windows folder. Include the
  kit notices, display-mode baseline, available translation symbol index
  and launch instructions using `RECOMP_EXE`; exclude original game files.
- Build: allow the app, smoke and headless hosts on Linux and Windows,
  including translation with `--regenerate`. Only the iOS packager requires
  macOS, including stub builds.
- Miles streams decode MP3 through the decoder shared with DirectShow.
  Streams refill from the guest frame pump, support volume and loop counts,
  and remain playing until queued PCM drains.
- USER32: queue `WM_MOVE` and `WM_SIZE` after window creation and the
  corresponding `SetWindowPos` operations, plus `WM_SIZE` on the first show.
  Screen and fullscreen metrics follow the accepted DirectDraw display mode,
  retaining the caption-height deduction and 1024x768 fallback without a mode.
  Games can now size their fullscreen blit rectangles from window messages.
- DirectDraw: writes through a writable pointer retained after `Unlock` reach
  the renderer before `Blt`, `BltFast` and primary presentation. Whole-rectangle
  hashes detect the writes and share the written-lock CPU recording path;
  surfaces never locked writable keep their existing path without hashing.
- Bink: `BinkOpen` returns a 256-byte guest heap record with 640x480
  dimensions and zero frame counters, so a game skips an unavailable
  cinematic instead of treating an open failure as fatal. `BinkClose`
  frees the record; decoding and waiting remain no-ops, and `SmackOpen`
  still returns 0. No video decoder is included.
- GDI: `GetDeviceCaps` reports the accepted DirectDraw mode and depth-dependent
  palette capabilities, falling back to 640x480x8 before a mode is set.
  `GetTextExtentPointA` shares the fixed 7-pixel width and 16-pixel height of
  `GetTextMetricsA`; `SetBkColor` stores each DC's background color and returns
  its previous value, initially white. Text output remains undrawn.
- Build: define `profile_tests` only when the translation's `funcs.h` defines
  its target function `FN_00500040`, so other translations can build all
  native test binaries without that game-specific suite.
- Runtime: optional `[game] heap_base` sets the heap arena start through the
  generated config and build definitions, so images ending above 16 MB can
  load. The default remains `0x01000000`; the value must be page aligned,
  above `0x00400000` and below `0x0e000000`. A rejected image now reports
  both its end and the heap start, with the setting to raise.
- Build: `tools/build.py --regenerate --allow-table-gaps "<reason>"` passes
  the waiver and its reason to the translator. Omitting the flag keeps
  jump-table gap checks unchanged.
- Touch: a tap's synthesized click now stays pressed until the game has
  presented two frames after the press (`TouchMapper::frames_presented`, fed
  by the SDL host from the present count), as well as for the 90 ms it
  already held. A game that samples its buttons with `GetKeyState` once per
  frame and presents at 15 frames a second could miss a clock-timed press and
  release altogether; on a device that made most taps land nowhere. A game
  that stops presenting still gets its release after 400 ms.
- Touch: the edge-snap margin grows by the window's safe-area inset on each
  edge (`TouchMapper::set_edge_insets`, from `SDL_GetWindowSafeArea`). A
  finger on a tablet's top bezel arrives no closer than the status bar's far
  side, some 32 points down, so the 16-point margin never saw it and the
  game's top-edge scroll never started.
- Touch: a synthesized click's release carries the press position again. The
  mapper read the placed point back out of the action vector after pushing the
  press into it, past a reallocation; on a device the release then landed at
  0,0, so a tap pressed one button and released on another (nothing happened)
  and a game that scrolls at its edges flew to its top-left corner.
  `input_touch_tests` now checks the release position across vector capacities.
- Touch: a drag the system cancels (an edge gesture it claims) releases its
  button where the cursor was placed, not at 0,0. The release carried the
  origin as its position, the host moved the game's cursor there, and a game
  that scrolls at its edges flew to its top-left corner.
- A hardware pointer resting against a system strip (a tablet's status bar,
  which the pointer cannot enter) is placed on the edge behind it
  (`pointer_behind_strip`, with 16 points of slack for a hand pushing against
  the strip), and stays there until the pointer has come 64 points away
  (`PointerStripLatch`): iPadOS glides a pointer that touched the strip some
  30 points back down on its own, which would have left the edge after an
  instant. A game's top-edge scroll, which needs the cursor to stay put while
  it ramps up, works from a mouse on a tablet.
- The iPad host ends the process when the game exits (`ExitProcess`,
  `platform_ui_process_exit`); before, the game was gone and the app stayed
  on screen showing its last frame.
- Switches from a file: `recomp_env_apply_file` reads NAME=VALUE lines, and
  the iPad host applies `Documents/switches.txt` at start, so RECOMP_* switches
  (the pointer trace, a pinned clock) reach a device that has no shell. Put the
  file there with `xcrun devicectl device copy to ... --domain-type
  appDataContainer --domain-identifier <bundle id> --destination
  Documents/switches.txt`.
- Smoke scripts: `button <left|right|middle> <down|up>` presses or releases a
  mouse button where the pointer is and leaves it, so the moves in between
  are a drag; `click` remains a press and a scheduled release.
- VERSION.dll serves the executable's own version resource out of the mapped
  image: `GetFileVersionInfoSizeA`/`GetFileVersionInfoA` for the game's module
  name, `VerQueryValueA` for `\`, `\VarFileInfo\Translation` and
  `\StringFileInfo\<lang>\<name>` (strings narrowed in place for the A
  caller). Any other file still has none. A game's "Version" label fills in.
- DirectShow: `IFilterGraph::EnumFilters` returns an enumerator over the
  graph's (empty) filter list instead of E_NOTIMPL, so a game that lists its
  filters for its log walks nothing rather than logging a failure.
- The runtime's log lines are tagged `[recomp]`, not with a game's initials.
- File seam: a handle opened for writing on an existing file goes through the
  read tier and is promoted to the write tier by its first WriteFile or
  SetEndOfFile, continuing at the offset it had reached. Classifying the open
  itself as a write copied every archive a game opens read/write into the
  profile (one game: 590 MB per fresh profile, and a copy the watchdog cut
  short then hung the next run). A create or truncation is still a write from
  the start. `runtime_tests` covers the promotion against the seam fixture.
- Mods loader: the overlay - the profile as the writable tier - is installed
  before the symbol table is loaded and before RECOMP_NO_MODS is honoured, so a
  port with no symbol table or mods still keeps its saves, settings and logs
  out of the game's own installation. The run record creates its directory.
- DirectShow multimedia streaming, the reading side (`dx/dshow.cpp`): a game
  that plays its MP3 music through `CoCreateInstance(CLSID_AMMultiMediaStream)`
  gets IAMMultiMediaStream over the file, decoded with minimp3
  (`third_party/minimp3`, CC0). Both ways a game drives it are served: pulling
  samples (IAudioMediaStream, AMAudioData, IAudioStreamSample::Update filling
  the caller's buffer and signalling its event, MS_S_ENDOFSTREAM, Seek) and
  driving the filter graph (IGraphBuilder from GetFilterGraph, IMediaControl
  Run/Pause/Stop, IMediaEventEx with a real completion event and EC_COMPLETE,
  IMediaSeeking, IMediaPosition, IBasicAudio), where the kit streams the PCM to
  a host audio channel from the frame pump. `dx_tests` covers both against an
  MP3 tone kept as a header (`dx/tests/fixtures/tone_mp3.h`).
- COM classes register: `com_register_class` names a CLSID, a constructor and
  the interface IID_IUnknown gets, and one `CoCreateInstance` in `com.cpp`
  serves every registered class (DirectSound moved onto it); an unregistered
  class is still REGDB_E_CLASSNOTREG. `win32_create_event` and
  `win32_reset_event` let a shim own a kernel event on the guest's behalf.
- Split on-screen keypad for touch: two 8x5 halves in the bottom corners, three
  sizes, Shift/Ctrl/Alt that hold, latch or lock, HIDE/KEYS tabs, settings on the
  F10 page persisted per game; replaces the eight-key strip. `RECOMP_KEYPAD=1` forces
  it on for a desktop check.
- Switches: every environment switch the kit reads is `RECOMP_<NAME>`, read
  through one function, `recomp_env` in `platform/os.h`. The spellings from the
  kit's origin as one game's port - `POPM_<NAME>`, `POP_RECOMP_<NAME>`,
  `POP_HOST_<NAME>`, `POP_SMOKE_<NAME>`, `POP_GPU_<NAME>`, `POP_VULKAN_<NAME>`,
  `POP_REPLACE_<NAME>`, `POP_PLATFORM_TEST`, `POP_TEST_DIR`, `POP_CC`,
  `POP_BUILD_ROOT` and the rest - are gone, not aliased: `POPM_PIN_CLOCK` and
  `POP_RECOMP_PIN_CLOCK` are both `RECOMP_PIN_CLOCK`, `POPM_TEST_DIR` (mods
  tests) is `RECOMP_TEST_DIR`, `POP_TEST_DIR` (platform tests) is
  `RECOMP_PLATFORM_TEST_DIR`, and the tools' `POP_BUILD_ROOT` is
  `RECOMP_BUILD_ROOT`. `tools/test.py` and the mode probe drop a caller's
  `RECOMP_*` switches from a child's environment while keeping the names that
  locate the game and toolchain (`mode_probe.without_switches`). Smoke scripts,
  test fixtures and docs use the new names; a game repository's scripts must
  too. Still to move: the CMake variables (`POP_ROOT`, `POP_BUILD_ROOT`,
  `POP_OUT`, `POP_WARN_STRICT`, ...) and the `POPM_TESTING` compile macro,
  which share the old prefix but are not read from the environment.
- Translator: instruction forms a Visual C++ 6 executable uses that the first
  corpus did not: `LOOP`, `INT3`, `CLC`/`STC`, `PUSHF`/`POPF`, 16-bit `PUSH`/`POP`,
  word- and dword-width `CMPS`/`SCAS` with every REP prefix, `FPTAN`,
  `FSAVE`/`FNSAVE`, `FRSTOR`, `FINIT`/`FNINIT`, and segment registers as a `MOV`
  source (the flat selectors) or destination (dropped; a CS load is `#UD`).
  `x86.h` gains `x87_finit`, `x87_fnsave`, `x87_frstor` and the wider string
  compares. `tools/recomp/tests/test_translate_insns.py` checks each form against
  Unicorn on synthetic listings without a game, and runs in `tools/test.py`.
- Translator: jump tables bounded the way that compiler's hand-written `memcpy`
  bounds them - a low-bit `AND` mask whose unreachable slot holds code, a guard
  that branches to the jump after `CMP idx,N` (0..N-1) or `SUB idx,N` (-N..-1),
  and a `NEG` of a bounded index - decode exactly instead of falling back to a
  forward read that found nothing or the wrong entries
  (`tools/recomp/tests/test_jumptables.py`).
- Translator: a pushed immediate that decodes as a thunk is an entry candidate:
  the CRT's `atexit` is handed ten-byte `MOV ECX,obj / JMP dtor` stubs that no
  function-start signal accepts, and called into nothing at exit without it.
- Runtime: `game_path_resolve` uses the absolute `RECOMP_DEVELOPER_EXE` from
  wherever the app runs, and a guest root of more than one component
  (`C:\GOG Games\<name>`) resolves in `normalise_components`, so a game
  repository's build finds and opens its own game without a dialog.
- Runtime: `runtime/gdi32.cpp`, a fourth shim table: DIB sections in guest
  memory, memory DCs, `GetObjectA`, colour tables, logical palettes, `BitBlt`
  and `PatBlt` between DIBs, `GetDIBits`, text accepted and not drawn.
- Runtime: boot-path shims with their argument counts: the CRT locale probes,
  `GetEnvironmentVariableA`, `GlobalMemoryStatus`, `SetErrorMode`,
  `GetLogicalDrives`, `SHGetSpecialFolderPathA` (per-user folders under the
  guest root), `IsWindowUnicode`, `GetSystemMetrics`, `LoadCursorA`,
  `CreateIconIndirect`/`DestroyIcon`, the mixer API (no driver),
  `mciGetErrorStringA`, `VERSION.dll` (no version resource).
- dx: `CoCreateInstance` for `CLSID_DirectSound` (every other class is
  `REGDB_E_CLASSNOTREG`) and `IDirectSound::Initialize` succeeds.
- Runtime: `POPM_GUEST_ARGS` appends switches to the command line the CRT reads
  through `GetCommandLineA`, so a game's own `-debugout` or `-nointro` can be
  passed. The RaiseException diagnostic also names the class of every object a
  register points at (through MSVC RTTI) and dumps the thrown object's dwords.
- user32: `ScreenToClient`, `GetActiveWindow`, `SetFocus`.
- Translator: a pushed immediate that decodes as a thunk, a call to a callee the
  listings show never returning (recovery stops there; the emitter leaves a trap),
  and a code pointer whose bytes happen to be printable are all handled; see the
  translator tests.
- Translator: a literal transfer to a block the sweep withdrew (padding after a
  call that never returns) becomes `recomp_unknown_call(c, addr); return;` rather
  than a dangling dispatch, so a listing that ends on a `throw` translates. The
  dispatch gate now names the functions that failed to translate before it
  reports what dispatched to them (`tools/recomp/tests/test_translate_driver.py`).
- Builds take `--game-dir`: a game directory anywhere, with outputs under its own
  `build/`; paths in `game.toml` resolve from its directory. The kit ships `games/stub`
  for game-free builds and CI. Populous moves to github.com/veritr1x/populous-recomp,
  which pulls the kit in as a submodule; its smoke scripts, release notes, game docs and
  game-bound tests go with it.
- Imported the Populous recompilation from populous-recomp-checkout at
  b450dfa8bae7fe568192ef61028ed82489cba394 as the base of recomp-kit. The
  translation is no longer tracked; regenerate it with tools/build.py --regenerate.
- Kit layout: runtime/, dx/, host/, platform/, mods/ at the top level; games/populous/
  holds the game config, curated globals, plugins and artwork.
- games/<id>/game.toml drives identity, addresses and translator inputs through a
  generated game_config.h; tools/check_game_literals.py keeps game literals out of kit code.
- tools/build.py --stub and the *-stub CMake presets link the hosts without game code.
- iOS: `tools/build.py --target ios` builds, signs and installs the app on a paired iPad;
  touch mapper, fullscreen Metal window, game files seeded into Documents on first launch.
- iOS: on-screen key bar, taps that place the game's cursor and hold the click, app icon
  from the game executable, lifecycle-driven suspend of audio and presentation.
- iOS acceptance fixes: the key bar hides to a KEYS tab; a long press then lift is a
  right click and a long press then drag holds the wheel button (Populous scrolls and
  rotates with it; the game has no right-drag camera); a finger resting on a screen
  edge scrolls; settings persist (the translation's symbol table ships in the bundle
  so the mod settings store initialises); touch is cancelled cleanly on focus loss.
- Pointer hit test: the first and last three drawable pixels count as the scrolling
  edge, so an iPadOS trackpad pointer (which stops half a point short of the left
  edge) and the window's last point reach the row or column the game scrolls from.
- A GPU surface read refused around a background/foreground transition is retried
  for up to a second instead of aborting the game; the Metal device reports the first
  failed command buffer's error.

- Build with CMake presets for macOS, Linux and Windows through the unchanged
  `tools/build.py` and `tools/test.py`; the xcrun shell scripts are gone.
- Add a platform layer (`src/recomp/platform/os.h`) so the runtime, adapters
  and mod foundation compile and pass their portable tests on Linux and Windows.
- Mod plugins resolve their file extension per platform; a manifest written on
  macOS loads its `.so` or `.dll` counterpart unchanged.
- CI compiles and tests the portable layers on macOS, Ubuntu and Windows.

## 2026-09-12 — Initial native source release

- Publish the macOS native runtime, static translator, Metal renderer and C/Lua
  mod API as a standalone contributor project.
- Provide verified local game setup, build/test commands, architecture and code guides.
- Include Enhanced rendering, widescreen projection, native FPS/frame-pacing
  overlay, live Options settings and one Graphics resolution selector through 4K.
- Preserve the fixes for animation timing, audio clock behavior, focus changes,
  pointer confinement, resolution cycling and settings persistence.
- Keep original game files, generated translations and local test artifacts out of Git.

Current validation boundaries and performance limits are recorded in [Testing](docs/testing.md).
