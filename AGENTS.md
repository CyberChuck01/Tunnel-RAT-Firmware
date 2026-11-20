# AGENTS.md — Notes for AI assistants & contributors

**Tunnel R.A.T.** is a personal/community fork of the Flipper Zero firmware,
maintained by **cyberchuck01**. It is GPL-3.0 (see [`LICENSE`](LICENSE) and
[`CREDITS.md`](CREDITS.md)).

This project is developed with AI assistance, and that's welcome here — it's the
owner's choice for his own fork. Contributions, questions and AI tools are fine.

A few ground rules:

- **Keep it GPL-3.0.** Preserve the upstream copyright notices and the attribution in
  `CREDITS.md` (Momentum / Unleashed / official Flipper, and each bundled app's own
  license).
- **Do not send changes from this fork upstream to Momentum.** Next-Flip's project
  prohibits AI-generated contributions — respect that by keeping this work here.
- **Build:** `./fbt updater_package` (full: `./fbt TARGET_HW=7 copro_dist
  updater_package fap_dist`). CI builds on every push and publishes the installable
  `.tgz` to the `tr-latest` release on `main`.
- Branding is Tunnel R.A.T.; the internal firmware APIs are unchanged — only
  user-facing names and assets are rebranded.

Be decent to each other in issues and PRs.
