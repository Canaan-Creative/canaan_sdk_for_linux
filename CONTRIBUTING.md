# Contributing

## Before review

- `test/scripts/host_tests.sh` passes ([docs/TESTING.md](docs/TESTING.md)).
- Code style:
  - kernel and U-Boot patches: Linux kernel coding style, checked with `checkpatch.pl`
  - C in `apps/`: `apps/.clang-format`
  - shell: `test/scripts/shellcheck.sh`
- Kernel, U-Boot and OpenSBI patches live under `product/k230/`.
- A change to one side of a rule in [docs/CROSS-LAYER-RULES.md](docs/CROSS-LAYER-RULES.md) changes
  every side and the document.

## Commit messages

```
<type>(<scope>): <subject>

Machine:
Customer:
Detailed description:
Tested:
```

| Field | Content |
|---|---|
| `type` | `feat`, `fix`, `refactor`, `optimize`, `bringup`, `config`, `build`, `cleanup`, `docs` |
| `scope` | the affected module (`wdog`, `pool`, `kernel`, `bootloader`, `yocto`, `product`, …) or `all` |
| `subject` | English, lowercase start, no trailing period |
| `Machine`, `Customer` | affected models or `common`; affected customers or `NA` |
| `Detailed description` | why and what changed |
| `Tested` | test environment and result |

## Licensing of contributions

Contributions are accepted under the license of the file they modify ("inbound
= outbound"). Any contribution you submit for inclusion is licensed under the
MIT License, or, for a file under another license, under that file's license;
a patch carries the license of the files it modifies and keeps their copyright and
license notices.
You keep the copyright in your contribution. A new file carries an
`SPDX-License-Identifier` line on its first line (second after a shebang, `<!DOCTYPE html>` or
`# syntax=` line), followed by a copyright line naming its author, except Markdown documents,
license texts (`LICENSE`, `NOTICE`, `third-party-license/`), ignore files, patches, JSON, PEM,
VERSION, configuration data, test fixtures, binaries and upstream third-party files.
