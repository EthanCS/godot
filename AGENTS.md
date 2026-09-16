# Kiln engine development

- The engine base is Godot 4.7.2-stable at ed1daf0bf001b61586d9930840f2f1394092c079.
  Read kiln/upstream.json and kiln/README.md before implementation.
- Preserve upstream history, licensing, and existing Forward+/Mobile/Compatibility
  rendering. Keep changes focused on the new renderer and required integration.
- Implement a real engine G-buffer and deferred lighting path. A renamed Forward+
  renderer or compositor overlay does not satisfy this project.
- Record implemented, compiled, visually verified, and unsupported features
  separately. Never claim an untested platform or a performance target passed.
- Source game code is a read-only reference. The demo must be self-contained.
  Do not copy secrets, .env files, caches, saves, or unrelated game data. Preserve
  provenance and check redistribution terms before publishing imported assets.
- Use deterministic scene timelines and fair equal-quality Forward+ comparisons.
- Build affected engine targets and verify changed graphics on a real GPU. Do not
  count headless startup as visual validation. Keep captures/logs in temp storage.
