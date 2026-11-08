# foolish to cbase cutover kit

This kit moves foolish onto the cbase submodule, mounted at cbase/.
It was proven green against foolish 48af777d3 and cbase c1813f2e, and the tag v0.1.0 adds nothing that it consumes, except lib/wasm returning to foolish.
The cutover is deferred by owner decision (2026-11-08); re-prove it against the cbase tag in use when it is picked up.

- cbx_cutover.sh: the scripted cutover, which builds a scratch copy of foolish with cbase mounted at cbase/.
- cbx_foolish_patch.py: the product-side edits (admin_json shapes, the app config and panels, rows families and key templates, the knobs read under their old env names).
- cbx_mk_admin_json.py: generates foolish/server/src/app/admin_json.{c,h} from the shapes that left cbase core.
- foolish_overlay/: whole files that replace foolish's (foolish/server/Makefile on cbase.mk).
- to_foolish/, to_foolish_root/: every file that moves into foolish, at its destination path.
- cbx_names.diff, cbx_cutover_names.txt: the test-name diff against the baseline (91 present, 0 removed, 5 added).

This is foolish product code, so it belongs in the foolish repo (on a side branch until the cutover), never in cbase.
