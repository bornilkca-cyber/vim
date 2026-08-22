# Copilot Language Server — Protocol Reference

Empirically derived against **`@github/copilot-language-server` 1.408.0** (native
`linux-x64` build) on 2026-08-22 by driving the server over stdio JSON-RPC.

> These `conversation/*` methods are **not** part of standard LSP and are not
> covered by the package's published `dist/api/types.d.ts` (which only documents
> the unrelated Context Provider API). Everything below was captured from live
> traffic and from the server's own JSON-Schema validator error messages.
> Treat as version-pinned to 1.408.0.

---

## 1. Transport

- Launch: `copilot-language-server --stdio`
- Framing: standard LSP `Content-Length: <n>\r\n\r\n<body>`.
- Vim already implements this framing natively via `CH_MODE_LSP`
  (`channel.c`), selected with `job_start()` / `channel_open()` using
  `in_mode`/`out_mode` = `lsp`. No new framing code is required.
- The native binary is **self-contained**: no Node runtime needed.

### Schema-validator introspection

Invalid params return `-32602` with a field-by-field report, e.g.

```
Schema validation failed with the following errors:
- /turns: Expected required property
- /turns: Expected array
```

This is the fastest way to re-derive schemas after a server upgrade.

---

## 2. Handshake

### `initialize` (request)

```jsonc
{
  "processId": 1234,
  "clientInfo": { "name": "Vim", "version": "9.2" },
  "rootUri": "file:///path/to/project",
  "capabilities": {
    "workspace": { "workspaceFolders": true },
    "window": { "showDocument": { "support": true }, "workDoneProgress": true }
  },
  "initializationOptions": {
    "editorInfo":       { "name": "Vim", "version": "9.2" },
    "editorPluginInfo": { "name": "vim-copilot-native", "version": "0.1" }
  },
  "workspaceFolders": [ { "uri": "file:///path", "name": "project" } ]
}
```

`editorInfo` and `editorPluginInfo` are **required**; the server rejects
conversation calls without them.

### `initialize` result (verbatim)

```jsonc
{
  "capabilities": {
    "textDocumentSync": { "openClose": true, "change": 2 },   // 2 = Incremental
    "notebookDocumentSync": { "notebookSelector": [ { "notebook": "*" } ] },
    "workspace": { "workspaceFolders": { "supported": true, "changeNotifications": true } },
    "executeCommandProvider": { "commands": [
      "github.copilot.finishDeviceFlow",
      "github.copilot.didAcceptCompletionItem",
      "github.copilot.didAcceptNextEditSuggestionItem",
      "github.copilot.didAcceptPanelCompletionItem"
    ] },
    "inlineCompletionProvider": {}
  },
  "serverInfo": { "name": "GitHub Copilot Language Server",
                  "version": "1.408.0", "nodeVersion": "22.20.0" }
}
```

Then send the `initialized` notification.

> The `openURL` client capability was **removed** in 1.408.0. The server logs:
> `[sdk] The openURL Copilot capability has been removed in favor of
> window/showDocument.` Clients must implement `window/showDocument`.

---

## 3. Authentication (device flow)

| Step | Direction | Method |
|---|---|---|
| 1 | → | `signIn` (no params) |
| 2 | ← | `PromptUserDeviceFlow` payload |
| 3 | — | user opens `verificationUri`, enters `userCode` |
| 4 | → | `workspace/executeCommand` `github.copilot.finishDeviceFlow`, or poll `checkStatus` |
| 5 | ← | `didChangeStatus` / `statusNotification` flip to `Normal` |

### `signIn` result

```jsonc
{
  "status": "PromptUserDeviceFlow",
  "userCode": "XXXX-XXXX",
  "verificationUri": "https://github.com/login/device",
  "expiresIn": 899,
  "interval": 5,
  "command": { "command": "github.copilot.finishDeviceFlow",
               "title": "Sign in with GitHub", "arguments": [] }
}
```

### `checkStatus`

Params `{ "options": {} }` (accepts `{"localChecksOnly": true}`).

- Signed out: `{ "status": "NotSignedIn" }`
- Signed in:  `{ "status": "OK", "user": "<login>" }`

`signOut` clears it. Tokens are persisted by the **server** under
`~/.config/github-copilot/`; the client never handles a credential.

### Auth-gated methods

Before sign-in these fail with `{"code": 1000, "message": "Not authenticated: NotSignedIn"}`:
`conversation/preconditions`, `conversation/agents`, `conversation/create`, `conversation/turn`.

**`conversation/templates` and `conversation/modes` work while signed out** —
useful for offline/CI tests.

---

## 4. Server → client notifications

| Method | Payload |
|---|---|
| `didChangeStatus` | `{ busy, kind, message? }` — `kind`: `Normal` \| `Error` \| `Warning` \| `Inactive` |
| `statusNotification` | as above plus `status` (legacy duplicate; handle one, ignore the other) |
| `window/logMessage` | `{ type, message }` — feed `:copilot log` |
| `$/progress` | streaming turn output, see §6 |

Server → client **requests** that must be answered or the flow stalls:
`window/showDocument`, `window/showMessageRequest`, `conversation/context`,
`conversation/invokeClientToolConfirmation`, `conversation/invokeClientTool`.

---

## 5. Conversation lifecycle

### `conversation/create`

Required: `turns` (array, `minItems: 1`), each turn requires `request` (string).

```jsonc
{
  "workDoneToken": "vim-create-1",          // required for streaming
  "turns": [ { "request": "Explain this function." } ],
  "capabilities": { "skills": [], "allSkills": true },
  "source": "panel",                        // "panel" | "inline"
  "chatMode": "Ask"                         // "Ask" | "Edit" | "Agent"
}
```

Result:

```jsonc
{
  "conversationId": "8a96022c-…",
  "turnId": "6cceb1ee-…",
  "modelName": "GPT-4o",
  "billingMultiplier": 1
}
```

Note: `create` **immediately executes the first turn** — it is not a passive
constructor. The reply streams over the `workDoneToken` given here.

### `conversation/turn`

Required: `workDoneToken` **and** `message`.

```jsonc
{
  "workDoneToken": "vim-turn-7",
  "conversationId": "8a96022c-…",
  "message": "Now add error handling."
}
```

Result: same shape as `create` (new `turnId`).

### Other methods

`conversation/destroy`, `conversation/turnDelete`, `conversation/rating`,
`conversation/copyCode`, `conversation/insertCode`, `conversation/documentDiff`,
`conversation/notifyCodeAcceptance`, `conversation/persistence`,
`conversation/registerTools`, `conversation/unregisterTools`,
`conversation/invokeClientTool`, `conversation/invokeClientToolConfirmation`,
`conversation/updateToolsStatus`, `conversation/inspectPrompt`,
`conversation/inspectFetchResult`, `conversation/preconditions`.

### `conversation/templates` (works signed out)

| id | shortDescription | scopes |
|---|---|---|
| `tests` | Generate Tests | chat-panel, agent-panel, editor |
| `simplify` | Simplify This | editor, chat-panel, agent-panel, inline |
| `fix` | Fix This | editor, chat-panel, agent-panel, inline |
| `explain` | Explain This | editor, chat-panel, agent-panel, inline |
| `doc` | Generate Docs | editor, chat-panel, agent-panel, inline |
| `feedback` | Feedback | chat-panel, agent-panel |
| `help` | Help | chat-panel, agent-panel |

### `conversation/modes` (works signed out)

`Ask` (general chat) · `Edit` (code editing) · `Agent` (tools + capabilities).
All `isBuiltIn: true`.

### `conversation/agents` (requires auth)

`github`, `project`, `github-copilot-coding-agent` — addressed as `@slug`.

---

## 6. Streaming: `$/progress`

All turn output arrives as `$/progress` notifications keyed by the
`workDoneToken` supplied in `create`/`turn`.

### `kind: "begin"`
```jsonc
{ "kind": "begin", "title": "Conversation <id> Turn <id>",
  "conversationId": "…", "turnId": "…" }
```

### `kind: "report"` — two distinct variants

**(a) Step/status updates** — drive a spinner or status line:
```jsonc
{ "kind": "report", "conversationId": "…", "turnId": "…",
  "steps": [ { "id": "collect-context", "title": "Collecting context",
               "status": "running" } ] }
```
`status` ∈ `running` | `completed`. Observed step ids: `collect-context`,
`recent-files`, `git-metadata`, `problems-in-active-document`, `generate-response`.

**(b) Text chunks** — the actual answer:
```jsonc
{ "kind": "report", "conversationId": "…", "turnId": "…",
  "reply": "```python\n", "annotations": [], "references": [],
  "hideText": false, "notifications": [] }
```

> ### ⚠ `reply` chunks are DELTAS, not cumulative snapshots.
> Verified: a reply of `` ```python\nfor i in range(1, 41):\n    print(i)\n``` ``
> arrived as 4 chunks of length 10, 23, 13, 3. Concatenation = 49 chars = the
> full text. The final chunk is only `` ``` ``.
> **The client must append each chunk.** Replacing would leave only ``` ``` ```.

### `kind: "end"`
```jsonc
{ "kind": "end", "conversationId": "…", "turnId": "…",
  "followUp": { "message": "…", "id": "…", "type": "Follow-up from model" },
  "suggestedTitle": "Simple Response Interaction",
  "skillResolutions": [ { "skillId": "references", "resolution": "unresolvable",
                          "files": [], "resolutionTimeMs": 0 } ],
  "updatedDocuments": [] }
```

`suggestedTitle` → chat window title. `followUp.message` → offer as a
one-key next prompt. `updatedDocuments` → buffer edits in Edit/Agent mode.

---

## 7. Implementation notes for the Vim port

1. **Positions are UTF-16 code units.** Vim's byte columns must be converted
   on every `textDocument/*` payload. Highest-risk correctness area.
2. **Deltas must be appended**, and may split mid-word or mid-fence — buffer
   until a newline (or a short flush timer) before touching the buffer to avoid
   redraw thrash.
3. **One `workDoneToken` per turn**, mapped to the target buffer/append line;
   tokens are the only way to demultiplex concurrent turns.
4. `create` fires a turn — do not send an extra `turn` for the first message.
5. Answer server→client requests promptly; an unanswered
   `conversation/context` or `invokeClientToolConfirmation` stalls the turn.
6. `didChangeStatus` and `statusNotification` are duplicates — pick one.
7. Re-run the validator probes after any server version bump.
