# Task: Documentation Audit — More-Phi v3.3.0

## Goal
Audit and update all project documentation files to accurately reflect the current state of the codebase.

## Scope
- Project-owned `.md` files at root, docs/, specs/, and config/ directories
- Exclude build artifacts, third-party dependencies (build/, _bmad/, .github/, .kilocode/, etc.)
- Focus on technical accuracy: correct paths, class names, API signatures, build commands, architecture descriptions

## Non-goals
- Do NOT create new documentation files
- Do NOT restructure documentation organization
- Do NOT edit source code
- Do NOT edit third-party/workflow template files (_bmad/, .github/, .kilocode/, .specify/, .claude/, .agents/)

## Success Criteria
1. Every project doc references correct class names and file paths that exist in the codebase
2. Build commands in docs match CMakeLists.txt and build-ninja.bat exactly
3. Version numbers are consistent across all docs (v3.3.0)
4. Architecture descriptions match the actual source file layout
5. Dependencies and their versions are accurate
6. No references to removed/deprecated features without clear deprecation notes

## Allowed Operations
- Read any file for analysis
- Edit .md files within the project scope
- Run shell commands for verification (e.g., checking file existence)

## Files to Audit (by priority)
### Tier 1 — Primary agent-facing docs
- AGENTS.md (root)
- CLAUDE.md (root)
- README.md (root)
- CHANGELOG.md (root)

### Tier 2 — Technical documentation
- docs/ARCHITECTURE.md
- docs/API_REFERENCE.md
- docs/DEVELOPER_GUIDE.md
- docs/BUILD_STABILITY_GUIDE.md
- docs/FEATURE_REFERENCE.md
- docs/TECHNICAL_DOCUMENTATION.md

### Tier 3 — User-facing docs
- docs/USER_GUIDE.md
- docs/USER_MANUAL.md
- docs/RELEASE_CHECKLIST.md

### Tier 4 — Audit/spec reports
- AUDIT_REPORT.md (root)
- DSP_AUDIT_REPORT.md (root)
- More-Phi_Technical_Review_Report.md (root)
- VST3_TECHNICAL_AUDIT_AND_MARKET_ANALYSIS.md (root)
- All docs/audit/*.md
- All docs/validation/*.md

### Tier 5 — Specs & plans
- specs/001-004/*.md
- docs/superpowers/**/*.md
- docs/plans/*.md
- docs/design/*.md

### Tier 6 — Misc documentation
- docs/AI_* (all)
- docs/DATASET_GENERATION.md
- docs/ECOSYSTEM.md
- docs/LEARN_MODE_GUIDE.md
- docs/PRODUCT_POSITIONING.md
- docs/PYTHON_MCP_SERVER.md
- plan.md (root)
- indie_dance_production_guide.md (root)
- melodic_techno_production_guide.md (root)
