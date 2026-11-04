# Paused development snapshot — 2026-11-04

This snapshot preserves the current Windows-focused Switch 2 work, existing
Android source, diagnostic bridge, and backend regression tools.

- Kirby Air Riders black-screen correctness is still unresolved; counters/FPS
  are not proof of correct pixels or playability.
- The per-operation F32-to-F16 RN/FmzNone rounding repair and LEA/local-memory
  tests passed their documented backend checks. HMMA/SR/CSM contracts and full
  resource-to-consumer replay remain open.
- Development/testing is paused. Cleanup removes only reproducible build and
  historical experiment binaries; the runnable deployment and user data stay.
- Windows was the verification focus. Android source is retained; a current
  Android build/playability certification is not claimed.
- Personal project guidance and historical source originals are archived in
  a separate private research repository. ROM, keys, firmware, saves, and large
  runtime dumps are not included in source-control uploads.
