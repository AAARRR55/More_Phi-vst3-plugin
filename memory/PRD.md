# PRD — VST3 License Key Management Specification

## Original Problem Statement
Create a complete technical specification and implementation guide for a VST3 plugin license key management system covering key generation/validation, multiple license types, secure storage, offline activation, anti-tamper protection, VST3 integration, activation UX, compliance, pseudocode, C++ examples, configuration guidance, and third-party licensing recommendations.

## Architecture Decisions
- Deliverable is a Markdown technical guide integrated into the existing More-Phi documentation workspace.
- Target implementation assumes JUCE 8 / C++20 VST3/AU plugin architecture already used by More-Phi.
- Recommended licensing model uses signed activation certificates with Ed25519 public-key verification, server-held private keys, local secure storage, background validation, and audio-thread-safe atomic license state.
- License support covers trial, perpetual, subscription, node-locked, and floating/team lease workflows.

## Implemented
- Added `/app/docs/VST3_LICENSE_KEY_MANAGEMENT.md` with complete architecture, workflows, key formats, cryptographic approach, secure storage guidance, validation pseudocode, C++ implementation patterns, UI/UX, compliance, testing, and roadmap.
- Linked the guide from `/app/README.md` under Documentation.
- Validated the guide contains all requested topic areas and README link.

## Prioritized Backlog
### P0
- Implement `src/Licensing` module skeleton in C++ when source code is ready for modification.
- Add unit tests for key normalization, checksum, certificate verification, expiry, machine mismatch, and grace-period transitions.

### P1
- Build activation UI panel in JUCE with online/offline activation, status messaging, and deactivation flow.
- Implement mocked activation server contract tests.

### P2
- Add production licensing backend or integrate selected third-party licensing provider.
- Add audit dashboard, privacy policy language, and floating/team management workflows.

## Next Tasks
- Choose custom backend vs third-party provider.
- Select production cryptography library for Ed25519 verification.
- Convert the specification into implementation tickets for C++ plugin work.
