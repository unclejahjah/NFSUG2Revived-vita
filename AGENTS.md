# Agent notes

## Porting a PC mod option

To add, port or fix a PC mod feature (for example an NFSU2 Extra Options setting), follow
[.claude/skills/modding/SKILL.md](.claude/skills/modding/SKILL.md). In short:

- Identify the mod's ini, the mod's version and the options in scope.
- Make a table of every change the mod makes for those options, from its source and checked against its release.
- Map the changes onto the port. Writes into guest `.text` do nothing, so:
  - data patches go in `modrt::applyPatches`;
  - operand edits go in `MOD_SWITCHES`;
  - other code changes go in a native override.
- Check for conflicts with the cfg, the port's natives and other mods. If there are any, ask the user.
- Replicate the mod exactly, switched only by its ini key and editable in the Mods tab.
- Tag every change `// {Mod} {version} [{Section}] {Key}`.
