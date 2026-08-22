#!/usr/bin/env python3
# Mock copilot-language-server for test_copilot.vim.
# Speaks LSP framing on stdin/stdout and answers the subset of the protocol
# that the ":copilot" commands use.  No network, no account.

import json
import select
import sys
import time

DEVICE_CODE = "TEST-CODE"


def read_message():
    length = 0
    while True:
        line = sys.stdin.buffer.readline()
        if not line:
            return None
        line = line.strip()
        if not line:
            break
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":")[1].strip())
    if length == 0:
        return None
    return json.loads(sys.stdin.buffer.read(length))


def write_message(obj):
    body = json.dumps(obj).encode()
    sys.stdout.buffer.write(b"Content-Length: %d\r\n\r\n" % len(body) + body)
    sys.stdout.buffer.flush()


def reply(msg_id, result):
    write_message({"jsonrpc": "2.0", "id": msg_id, "result": result})


def error(msg_id, code, message):
    write_message({"jsonrpc": "2.0", "id": msg_id,
                   "error": {"code": code, "message": message}})


def progress(token, value):
    write_message({"jsonrpc": "2.0", "method": "$/progress",
                   "params": {"token": token, "value": value}})


def run_turn(msg_id, params, conv_id):
    """Stream a reply, then answer the request -- the real server sends the
    progress "end" BEFORE the response, so do the same here."""
    token = params.get("workDoneToken")
    turn_id = "turn-1"
    if token is not None:
        progress(token, {"kind": "begin", "conversationId": conv_id,
                         "turnId": turn_id})
        progress(token, {"kind": "report", "conversationId": conv_id,
                         "turnId": turn_id,
                         "steps": [{"id": "generate-response",
                                    "title": "Generating response",
                                    "status": "running"}]})
        # Deltas, deliberately split mid-word and across lines.
        for chunk in ["Hello", " world", "\n```py", "thon\n", "x = 1\n", "```"]:
            progress(token, {"kind": "report", "conversationId": conv_id,
                             "turnId": turn_id, "reply": chunk})
        progress(token, {"kind": "end", "conversationId": conv_id,
                         "turnId": turn_id, "suggestedTitle": "Test chat"})
    reply(msg_id, {"conversationId": conv_id, "turnId": turn_id,
                   "modelName": "MockModel", "billingMultiplier": 1})


def run_cancellable_turn(msg_id, params, conv_id):
    """Wait for Vim to cancel this request, then send late progress."""
    token = params.get("workDoneToken")
    turn_id = "turn-1"
    progress(token, {"kind": "begin", "conversationId": conv_id,
                     "turnId": turn_id})
    progress(token, {"kind": "report", "conversationId": conv_id,
                     "turnId": turn_id, "reply": "Before cancel"})

    deadline = time.monotonic() + 0.2
    cancel = None
    while time.monotonic() < deadline:
        ready, _, _ = select.select([sys.stdin], [], [], 0.01)
        if ready:
            cancel = read_message()
            break
        # Give Vim's event loop a chance to process the scheduled CTRL-C.
        progress(token, {"kind": "report", "conversationId": conv_id,
                         "turnId": turn_id,
                         "steps": [{"id": "generate-response",
                                    "title": "Generating response",
                                    "status": "running"}]})
    if cancel is None:
        error(msg_id, -32000, "Timed out waiting for cancellation")
        return
    if (cancel.get("method") != "$/cancelRequest"
            or cancel.get("params", {}).get("id") != msg_id):
        error(msg_id, -32000, "Expected matching cancellation request")
        return

    progress(token, {"kind": "report", "conversationId": conv_id,
                     "turnId": turn_id, "reply": "After cancel"})
    progress(token, {"kind": "end", "conversationId": conv_id,
                     "turnId": turn_id})
    reply(msg_id, {"conversationId": conv_id, "turnId": turn_id,
                   "modelName": "MockModel", "billingMultiplier": 1})


def main():
    signed_in = True
    conv_seq = 0

    while True:
        msg = read_message()
        if msg is None:
            return
        method = msg.get("method")
        msg_id = msg.get("id")
        params = msg.get("params") or {}

        if method is None or msg_id is None and method.startswith("$/"):
            continue

        if method == "initialize":
            reply(msg_id, {
                "capabilities": {
                    "textDocumentSync": {"openClose": True, "change": 2},
                    "inlineCompletionProvider": {},
                },
                "serverInfo": {"name": "Mock Copilot", "version": "0.0.1"},
            })
        elif method == "checkStatus":
            reply(msg_id, {"status": "OK", "user": "testuser"} if signed_in
                  else {"status": "NotSignedIn"})
        elif method == "signIn":
            if signed_in:
                reply(msg_id, {"status": "AlreadySignedIn", "user": "testuser"})
            else:
                reply(msg_id, {"status": "PromptUserDeviceFlow",
                               "userCode": DEVICE_CODE,
                               "verificationUri": "https://example.invalid/dev",
                               "expiresIn": 900, "interval": 1})
        elif method == "signOut":
            signed_in = False
            reply(msg_id, {"status": "NotSignedIn"})
        elif method == "conversation/registerTools":
            reply(msg_id, [])
        elif method == "conversation/create":
            conv_seq += 1
            if params.get("turns", [{}])[0].get("request") == "cancel":
                run_cancellable_turn(msg_id, params, "conv-%d" % conv_seq)
            else:
                run_turn(msg_id, params, "conv-%d" % conv_seq)
        elif method == "conversation/turn":
            run_turn(msg_id, params, params.get("conversationId", "conv-1"))
        elif method == "textDocument/inlineCompletion":
            pos = params.get("position", {})
            reply(msg_id, {"items": [{
                "insertText": "MOCK ONE\nMOCK TWO",
                "range": {"start": {"line": pos.get("line", 0),
                                    "character": 0},
                          "end": {"line": pos.get("line", 0),
                                  "character": pos.get("character", 0)}},
            }]})
        elif msg_id is not None:
            # Unknown request: answer so the client never hangs.
            error(msg_id, -32601, "Method not found: %s" % method)


if __name__ == "__main__":
    main()
