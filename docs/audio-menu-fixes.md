# Audio lifetime and checkpoint menu fixes

The SDL2 port now preserves the original sound API's volume-ratio semantics and
one-shot lifetime. Checkpoint labels use the correct Linux character conversion.

## Changes

- Stop active weapon firing effects before deselection, including single-player
  AI death. Stop attached sounds before clearing an overflowing entity sound list.
- Cancel pending audio decodes on Silence as well as active voices. Keep sounds
  alive across callbacks that can release them or modify the registry.
- Treat Play's ratio and SetRatio as the same volume gain; pitch remains separate.
  Preserve fades applied during background decoding. Repeated loop requests keep
  their playback position; rapid one-shot requests keep the original 200 ms guard.
- Retain non-looping sounds until playback completes, stops or fails, matching
  the original auto-stop ownership. Short-lived particle emitters can release
  their references while explosion tails finish. Loops retain owner-based cleanup.
- Steal exactly one voice at capacity, and call Stop and its listeners after
  releasing the audio lock. This avoids nested locking and a second voice removal.
- Convert narrow menu fragments with the correct glibc wide-printf format and
  directly append already narrow text. Existing checkpoint date/time labels are
  repaired at display time; save contents and format are unchanged.

## Validation

Desktop Linux and Bootlin GCC 8.4/glibc 2.30 ARM builds passed. Run from the source
root:

```sh
python3 port/tests/sound_lifecycle_regression.py --prove-previous-bug
python3 port/tests/sound_ratio_regression.py
python3 port/tests/sound_oneshot_regression.py
python3 port/tests/menu_text_regression.py
```

These tests compile production methods with controlled fixtures. They cover AI
stop-before-detach, pending playback cancellation, callback lifetimes, attachment
overflow, helicopter fades, pitch, delayed decode, one-shot owner release and
cleanup, voice saturation, and timestamp localization. The one-shot test uses
the engine smart pointer. The lifecycle test includes old-behavior negative
controls; the other tests accept `--backend` or `--source` for earlier code.
Restoring the corresponding previous behavior was checked to fail each new
regression, including the nested lock at voice saturation.

Desktop Training smoke tests used offscreen SDL video and dummy audio. Real
terrain/air grenade effects loaded GRENADE1_MONO.wav (4927 ms) and reached normal
completion. These checks establish lifecycle behavior, not audible sound balance
or handheld performance. Menu conversion has unit coverage; the final guarded
menu probe exited normally without yielding the intended label diagnostics.

On H700, the user confirmed the second helicopter is audible and existing
checkpoint labels now display correctly. Weapon-loop recurrence has not been
reported during subsequent testing. Explosion loudness/tails and voice saturation
still need audible handheld acceptance; source and host checks do not establish
that all explosion complaints are resolved. The existing 700 ms late one-shot
threshold remains unchanged.

The installed combined ARM build on 2026-11-14 has SHA-256:
`7af6d51ef8b133bf19664502063f42e1de6d6c96ac1f31706356c7be4f008e8a`.
