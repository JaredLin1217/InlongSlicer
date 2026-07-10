# InlongSlicer

InlongSlicer is a fork and downstream development branch based on
[OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer).

The current purpose of this repository is focused and conservative:

- bug fixes on top of OrcaSlicer
- Inlong-specific maintenance and secondary development
- compatibility work needed by the InlongSlicer builds
- preservation of upstream open-source licensing and attribution

This repository is not presented as a separate official upstream slicer project.
For general slicer documentation and feature background, refer to the upstream
OrcaSlicer project and its documentation.

## Upstream Basis

InlongSlicer is based on OrcaSlicer, which itself is part of the open-source
slicer lineage that includes Slic3r, PrusaSlicer, Bambu Studio, SuperSlicer,
and related community work.

InlongSlicer keeps that lineage and attribution intact. Changes in this
repository should be understood as downstream bug fixes, maintenance work, and
Inlong-specific secondary development unless a change is explicitly documented
otherwise.

## Development Notes

For repository-specific development workflow, start with:

- [AGENTS.md](AGENTS.md)
- [CLAUDE.md](CLAUDE.md)

Current Agents workflow version: `2.8.0` (`precision-efficiency`).

When separating Inlong-specific changes from upstream behavior, use the clean
upstream comparison branch documented in the project workflow.

## Build

Build instructions currently follow the upstream OrcaSlicer build process.
This repository may also contain Inlong-specific build scripts and CI workflows
for local packaging and validation.

On Windows, the current local build entry point is:

```shell
build_release_vs.bat slicer
```

## License

The license file in this repository is the **GNU Affero General Public License
version 3 (AGPL-3.0)**. See [LICENSE.txt](LICENSE.txt).

InlongSlicer follows the same open-source licensing obligations inherited from
OrcaSlicer and the upstream projects it is based on. If you distribute modified
builds, or provide network access to modified versions, keep the AGPL-3.0
license notice and provide the corresponding source code as required by the
license.

Some included components or derived files may carry additional compatible
notices, such as GPLv3, LGPL, MIT, Boost, Apache, or other component-level
licenses. Keep those notices intact when modifying or redistributing this
software.

This is a practical licensing summary, not legal advice. The authoritative
license text is [LICENSE.txt](LICENSE.txt) together with the notices preserved
in the source tree.
