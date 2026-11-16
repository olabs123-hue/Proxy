# Upstream open-source contribution plan — Cloudflare Pingora

**Status: prepared only.** No issue, pull request or code contribution has been submitted to Cloudflare by this project.

Cloudflare's [Pingora contribution guidelines](https://github.com/cloudflare/pingora/blob/main/.github/CONTRIBUTING.md) recommend filing an issue before nontrivial changes. Simple typo and most documentation fixes can be proposed without an issue.

## Small first contribution candidate

At the time of review, the public [Pingora load-balancer quick-start guide](https://github.com/cloudflare/pingora/blob/main/docs/quick_start.md) contained a Markdown link with an extra closing parenthesis around the peer guide:

```text
[`Peer`](user_guide/peer.md))
```

A small proposed correction is:

```text
[`Peer`](user_guide/peer.md)
```

This is only a **candidate**. Recheck the upstream `main` branch and open PRs immediately before contributing so you do not submit a duplicate or unnecessary change.

## Submission checklist

1. Read the current contribution guidelines and existing open PRs/issues.
2. Confirm the line remains incorrect on the latest upstream branch.
3. Fork `cloudflare/pingora` into your own account and create a dedicated branch.
4. Make only the relevant documentation correction; inspect the Markdown preview.
5. Open a concise PR with a description such as “docs: correct peer guide Markdown link in quick start.”
6. Respond to maintainers' feedback. **Do not claim an accepted contribution unless maintainers accept/merge it.**

Do not submit PRs solely to inflate a CV; focus on accurate and useful improvements. More substantive follow-up contributions could involve reproducible integration tests or clarity improvements backed by your own measured observations, but require upstream discussion first.
