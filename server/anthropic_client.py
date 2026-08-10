"""
Minimal, stdlib-only Anthropic Messages API client with a real tool-use
loop. No `anthropic` package -- server.py has a hard "no third-party
dependencies" constraint (see its own module docstring), so this speaks
the Messages API's plain HTTPS/JSON wire format directly via
urllib.request (TLS is stdlib via the ssl module, nothing extra to
install).

The API key is read from the ANTHROPIC_API_KEY environment variable only
-- never accepted from a client, never logged -- matching phi.md's Hard
Architectural Decision "Anthropic API key lives server-side only".
"""

import json
import logging
import os
import urllib.error
import urllib.request

log = logging.getLogger('anthropic_client')

API_URL = 'https://api.anthropic.com/v1/messages'
API_VERSION = '2023-06-01'
DEFAULT_MODEL = os.environ.get('PHI_ANTHROPIC_MODEL', 'claude-sonnet-5')
MAX_TOKENS = 1024

# Human-readable labels for the Chat panel's "Name: ..." username prefix
# (see client/ui.c's draw_panel_chat -- it bolds whatever's before the
# first ": " on a message-start line) -- "claude-sonnet-5" isn't
# something a user should have to read in a chat transcript. Falls back
# to the raw model id for anything not listed here (e.g. a dated/pinned
# id set via PHI_ANTHROPIC_MODEL) rather than raising, since an unlabeled
# id is still a correct, honest thing to show.
MODEL_DISPLAY_NAMES = {
    'claude-sonnet-5': 'Sonnet 5',
    'claude-opus-5': 'Opus 5',
    'claude-fable-5': 'Fable 5',
    'claude-haiku-4-5-20251001': 'Haiku 4.5',
}


def model_display_name(model_id: str) -> str:
    return MODEL_DISPLAY_NAMES.get(model_id, model_id)
# Hard cap on tool-call rounds so a misbehaving loop (or a tool that keeps
# getting called with bad input) can't hang a chat-handling thread forever
# -- matches this project's "raise/report a bound rather than spin
# forever" convention at every other C boundary this session touched.
MAX_TOOL_ROUNDS = 6


class AnthropicError(Exception):
    pass


def _api_key():
    return os.environ.get('ANTHROPIC_API_KEY')


def _post(body: dict) -> dict:
    key = _api_key()
    if not key:
        raise AnthropicError('ANTHROPIC_API_KEY is not set in the server environment')
    data = json.dumps(body).encode('utf-8')
    req = urllib.request.Request(
        API_URL, data=data, method='POST',
        headers={
            'x-api-key': key,
            'anthropic-version': API_VERSION,
            'content-type': 'application/json',
        },
    )
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            return json.loads(resp.read().decode('utf-8'))
    except urllib.error.HTTPError as e:
        detail = e.read().decode(errors='replace')
        raise AnthropicError(f'Anthropic API HTTP {e.code}: {detail[:500]}') from e
    except urllib.error.URLError as e:
        raise AnthropicError(f'Anthropic API unreachable: {e.reason}') from e


def run_tool_loop(system: str, user_message: str, tools: list[dict], dispatch: dict) -> str:
    """Runs a real Anthropic tool-use conversation to completion and
    returns the final assistant text.

    tools: Anthropic tool-schema list (name/description/input_schema).
    dispatch: {tool_name: callable(input_dict) -> str}. Each callable is
    expected to catch its own errors and return a description of the
    failure as plain text rather than raise -- an uncaught exception here
    is still caught (see below) but aborts the WHOLE reply rather than
    just that one tool call, so callers should prefer the former.
    """
    messages = [{'role': 'user', 'content': user_message}]

    for _round in range(MAX_TOOL_ROUNDS):
        resp = _post({
            'model': DEFAULT_MODEL,
            'max_tokens': MAX_TOKENS,
            'system': system,
            'messages': messages,
            'tools': tools,
        })

        if 'error' in resp:
            raise AnthropicError(resp['error'].get('message', str(resp['error'])))

        content = resp.get('content', [])
        stop_reason = resp.get('stop_reason')
        messages.append({'role': 'assistant', 'content': content})

        if stop_reason != 'tool_use':
            return ''.join(b.get('text', '') for b in content if b.get('type') == 'text')

        tool_results = []
        for block in content:
            if block.get('type') != 'tool_use':
                continue
            name = block.get('name')
            tool_input = block.get('input', {})
            fn = dispatch.get(name)
            if fn is None:
                result_text = f'Unknown tool: {name}'
            else:
                try:
                    result_text = fn(tool_input)
                except Exception as e:
                    result_text = f'Tool {name} raised: {e}'
            tool_results.append({
                'type': 'tool_result',
                'tool_use_id': block.get('id'),
                'content': result_text,
            })
        messages.append({'role': 'user', 'content': tool_results})

    return '(stopped after reaching the tool-call round limit without a final answer)'
