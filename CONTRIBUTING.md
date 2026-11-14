# Contributing

Thank you for your interest in hydra-native. Bug reports, sketches that run differently than in the web editor,
and pull requests are all welcome.

## Report a problem

Open an issue with:

- the system and version (for example Ubuntu 26.04, macOS 15 on Apple Silicon, Windows 11);
- the version of hydra-native (the release, or the commit of a build);
- the smallest sketch that shows the problem, or a hydra.ojack.xyz link to it;
- what you expected, what happened, and the text of the error bar or the terminal.

A sketch that works in the web editor and not here is a bug, unless README.md lists the feature under
"What hydra-native leaves out".

## Change the code

1. Build it as README.md describes ("Build"), and run `make check` before and after your change. The checks that
   render need a GPU; `make test` runs the others.
2. Keep each commit to one change, with a message in the form `type(scope): subject`, for example
   `fix(ui): keep the cursor on screen after a paste`. Types: feat, fix, refactor, perf, docs, test, build, ci,
   chore. The subject is imperative and at most 50 characters.
3. Add a test for a fix or a feature where one is possible: the unit tests are in test/, and test/check.js renders in
   a hidden window.
4. Keep the code in the style of the file you change: complete words as names, named constants, and comments only
   where the reason is not clear from the code.
5. Update docs/manual.md when the change is visible to users.

hydra-native stays small on purpose. Before you add a dependency or a large feature, open an issue to discuss it.

## License

hydra-native is under the GNU Affero General Public License 3.0 (LICENSE), because it contains hydra-synth and
code of the hydra web editor. By contributing, you agree that your contribution is under the same license.
