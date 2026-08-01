# Contributing

## Workflow

1. Open an issue describing the bug or proposed behavior.
2. Create a focused branch from `main`.
3. Add tests for behavior changes.
4. Build, test, and run `./scripts/audit-release.sh`.
5. Open a pull request with the motivation, implementation summary, validation, and screenshots for
   UI changes.

Keep networking asynchronous, avoid blocking the OBS/Qt UI thread, and preserve independent output
control. API code must remain UI-independent; caption providers must remain output-independent.

## Style

- C++20, four-space conceptual indentation as established by the existing files
- Clear ownership and RAII for OBS/Qt resources
- No exceptions across OBS C callbacks
- Friendly UI errors without credentials or raw sensitive responses
- Synthetic fixtures only (`example.test`, fake entry IDs, and fake credentials)

## Security and privacy

Never commit KS values, partner/user details, stream URLs with tokens, stream keys, OBS logs, HAR
captures, local models, signing certificates, or `.env` files. Do not paste them into public issues
or pull requests. Use GitHub private vulnerability reporting for suspected security issues.

## Pull-request checklist

- [ ] Build succeeds without new warnings from project code.
- [ ] All tests pass.
- [ ] Release audit passes.
- [ ] UI remains responsive and windows remain resizable/scrollable.
- [ ] Logging contains no sessions, stream credentials, or sensitive response bodies.
- [ ] Documentation is updated for user-visible behavior.
