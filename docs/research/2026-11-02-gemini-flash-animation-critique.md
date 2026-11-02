# Independent Gemini animation critique

Review performed 2026-11-02 using the actual `agy` CLI and the user-selected
`gemini-3.8-flash-high` model. The user explicitly approved sharing the supplied media and
relevant source/trace excerpts with Gemini. No Codex subagents were created or renamed.
The application was not changed during this review.

**Later implementation outcome — USER-REVIEWED 2026-11-02:** following this critique, Gemini
implemented the initial patch through `agy`; parent review corrected the guard lifetime and
positioning path before deployment. The user described the running result as "generally great!".
The critique below remains a historical record, including its rejected explanations; the positive
verdict does not retrospectively establish those explanations or exact native parity. See
[IMPLEMENTATION.md section 9.1](../../IMPLEMENTATION.md#91-panel-window-focus-and-click-away-dismissal)
for the reviewed mechanism.

## What the reviewer actually inspected

The CLI's tool events record reads of the comparison, app entrance, Windows entrance, late-position,
and both overview frame sheets, plus all three user-supplied screenshots. Gemini also read
line-numbered current source/trace excerpts and surrounding sections of `IdleDimmer.cpp` directly.
It did not decode or play the MP4s itself; its visual review used extracted frames and screenshots.
This is a visual/code critique, not a test-based certification of appearance.

The review conversation is `95415260-2120-4286-ad49-5eaeeeed171d`. The first run finished image reads
but its shell tool was denied. The same conversation continued using the permitted file reader
and source excerpts; a later run timed out during further source inspection. An explicit request
to finish without additional tools produced the written critique. These incomplete attempts are
not counted as separate reviews. The CLI reports the selected model as `gemini-3.8-flash-high`.
Temporary prompts and raw CLI events remain under ignored
`build-tools/animation-comparison-2026-11-02/`.

## Conclusions and local cross-check

| Topic | Gemini's critique | Assessment against local evidence |
|---|---|---|
| Empty frame and interrupted entrance | Confirms the empty shell, disappearance, and later populated reveal from the images. | Supported by the original-video frame extraction. |
| Reentrant opening | Identifies nested `ShowPanel` and visible renderer teardown/recreation as the strongest startup defect. | Supported by trace/source. The exact triggering notification codes and reentry boundary remain unlogged; they must not be presented as directly measured. |
| Taskbar overlap | Identifies the moving topmost HWND and absent occlusion as the app's visible difference. | Supported by pixels and source. The reference's private shell implementation is not established. |
| First-frame readiness | Recommends coordinating reveal with actual composition readiness. | A useful requirement, not a demonstrated new mechanism. Existing upload/commit checks already exist and have not prevented the flash. Remove duplicate opening before attributing everything to a missing wait. |
| Late position jump | Initially treats the screenshot crops as a confounder and suggests a one-pixel end-rounding correction. | That does not explain the original-video measurements: a 22-pixel upward shift and a separate 9-pixel downward shift. Their causes remain unresolved. |

The original written critique is preserved as
[reviewer output](assets/animation-2026-11-02/gemini-original-critique.txt), including claims disputed
here. It is evidence of the external review, not an endorsed description of every mechanism.

## Claims that need correction

**MEASURED 2026-11-02 from the original app video:** the sampled dark body holds at y=106–859
through presentation time 3.833333 seconds, then moves to y=84–837 at 3.866667 seconds. Its sampled
height remains 754 pixels. Differently cropped clipboard screenshots cannot explain that
22-pixel translation within the original recording. The independent critic's initial treatment
of a multi-pixel jump as unproven is therefore too broad: the displacement is established;
its cause and relation to user input/animation phase are not.

The one-pixel rounding explanation also lacks support. `OnPostUpdate` uses integer `finalY` when
done, issues `SWP_ASYNCWINDOWPOS`, and then logs the current HWND rectangle. A log showing the
old position before the queued move applies is not proof of interpolation rounding. The extra
completion-handler write remains worth tracing, but neither it nor a rounding change is a
demonstrated explanation for the larger shift.

Gemini's account of the shell animating composition visuals rather than a topmost HWND is also
an inference. The supplied Windows clip establishes the desired behind-taskbar appearance;
it does not identify the shell's internal technique.

The reviewer was asked to reassess these points against the measured video coordinates and
the explicit `done ? finalY` code, with no additional tools or edits. Its
[written reassessment](assets/animation-2026-11-02/gemini-reassessment.txt) accepts the measured
displacements, retracts the rounding explanation and private-shell claim, and distinguishes
the recorded nested opening from the unlogged triggering notification codes.

The reassessment suggests a taskbar-margin/anchor instability as a new candidate. That is
**UNVERIFIED**, not a new diagnosis: no measured margin matches the displacement, no anchor
change has been established, and the visible-only timer does not call `AnchorPanel`. Its timing
instead makes repaint/presentation worth correlating as well. Preserve the demonstrated geometry
and open/replacement fault; do not replace a rejected rounding guess with an unmeasured offset
guess. The priority below follows evidence rather than adopting every reviewer recommendation.

## Priority for the next implementation session

1. Eliminate overlapping opening work: accept/bookkeep a gesture before reentrant operations
   and establish a preparing/opening state before any call that could reenter.
2. Recheck the first populated presentation after that fix; do not assume a successful commit
   or an initial pre-`CreateBrush` Pending reading establishes the final visible result.
3. Give the entrance the reference's taskbar occlusion, with deliberate clipping or ordering.
4. Correlate requested/applied HWND positions, completion-handler writes, and repaints with
   one user reproduction of the remaining late shift. Do not hide it with an arbitrary offset
   or a rounding patch based on an asynchronous observation.

The [main comparison](2026-11-02-flyout-animation-comparison.md) contains the measured table,
retained frames, source evidence, and limitations. No fix is certified by this review; the
running UI remains subject to the user's audit after an eventual code change.
