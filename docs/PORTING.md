# Porting between the private and public lines

The public repository (`origin`, `Kenja-mc/nioh4p`) carries a curated history
that shares no commits with the private line. **Never merge across the two:**
a merge would publish the private research history. The pre-cleanup backup
bundle and the private history still contain the owner's identifiers that the
public rewrite removed, so a merge reintroduces them.

To move content:

1. Commit on the working line (`feature/optional-gameplay-addons`).
2. In a scratch worktree of `public-master`, cherry-pick the commits.
   Content-identical files apply cleanly. Resolve drift against the modular
   tree - never hand-edit the public side away from it.
3. Run `tools/publish-scan.sh public-master --history` before pushing. Fix
   hits on the private line first, then re-port.
4. Push `public-master:master` and any release tag together.

Release binaries are always built and packaged from the modular line's source
(`tools/package.sh` writes `dist/` in the modular worktree), so the public
tree must always equal the modular tree at the release tag.
