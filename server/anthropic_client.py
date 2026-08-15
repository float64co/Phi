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
# Was 1024, then 4096 -- still too tight for a real generated mesh (a
# house-scale shape's positions/indices arrays can run well past a few
# thousand tokens). A response that hits the cap mid-tool-call truncates
# with stop_reason='max_tokens' and no usable text or complete tool_use
# block -- see run_tool_loop's explicit handling of that below for what
# used to happen instead (silently returned ''). Raised again here; if a
# genuinely huge single-call mesh still gets cut off, the model should be
# told (via the system prompt) to build it in smaller pieces across
# multiple create_mesh_object/set_mesh_vertices calls rather than pushing
# this value past what the API accepts without an extended-output beta
# header (not added here, since nothing has needed it yet).
MAX_TOKENS = 8192

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
        # Was 30s -- too tight now that MAX_TOKENS is 8192; a real,
        # legitimately-in-progress large tool call (generated mesh geometry
        # especially) can take meaningfully longer than that to finish
        # generating, and a network-level timeout here looks identical to
        # "the model is stuck" from the caller's side (URLError, not a
        # truncation this module can detect/explain the way it now does
        # for stop_reason='max_tokens').
        with urllib.request.urlopen(req, timeout=90) as resp:
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
    log.info(f'tool loop start: user_message={user_message[:200]!r}')
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
            log.warning(f'round {_round}: API returned an error: {resp["error"]!r}')
            raise AnthropicError(resp['error'].get('message', str(resp['error'])))

        content = resp.get('content', [])
        stop_reason = resp.get('stop_reason')
        usage = resp.get('usage', {})
        block_types = [b.get('type') for b in content]
        log.info(f'round {_round}: stop_reason={stop_reason} blocks={block_types} '
                  f'usage={usage.get("input_tokens")}in/{usage.get("output_tokens")}out')
        messages.append({'role': 'assistant', 'content': content})

        # A truncated response (hit MAX_TOKENS before the model finished)
        # is NOT the same thing as a normal, deliberate empty reply -- the
        # content in hand may be a half-written tool_use block (so no
        # complete tool call to dispatch) and/or no text block at all.
        # Previously this fell into the `stop_reason != 'tool_use'` branch
        # below and silently returned '', which is exactly what looked
        # like "Claude said nothing and did nothing" from the Chat panel
        # with zero indication of why. Surface it for real instead, both
        # to the server log and (briefly) to the user.
        if stop_reason == 'max_tokens':
            text = ''.join(b.get('text', '') for b in content if b.get('type') == 'text')
            log.warning(f'round {_round}: hit MAX_TOKENS ({MAX_TOKENS}) before finishing -- '
                        f'response truncated, {len(text)} chars of text recovered')
            if text:
                return text + '\n\n(cut off -- hit the reply length limit)'
            return ('(the reply was cut off by the token limit before producing any text -- '
                    'likely mid-way through a large tool call, e.g. a big generated mesh; '
                    'try asking for something smaller or in fewer steps)')

        if stop_reason != 'tool_use':
            text = ''.join(b.get('text', '') for b in content if b.get('type') == 'text')
            if not text:
                log.warning(f'round {_round}: stop_reason={stop_reason} but no text block in the '
                            f'response (blocks={block_types}) -- returning an empty reply')
            return text

        tool_results = []
        for block in content:
            if block.get('type') != 'tool_use':
                continue
            name = block.get('name')
            tool_input = block.get('input', {})
            fn = dispatch.get(name)
            if fn is None:
                result_text = f'Unknown tool: {name}'
                log.warning(f'round {_round}: model called unknown tool {name!r}')
            else:
                try:
                    result_text = fn(tool_input)
                except Exception as e:
                    result_text = f'Tool {name} raised: {e}'
                    log.warning(f'round {_round}: tool {name} raised: {e!r}')
            log.info(f'round {_round}: tool {name}({tool_input!r}) -> {str(result_text)[:200]!r}')
            tool_results.append({
                'type': 'tool_result',
                'tool_use_id': block.get('id'),
                'content': result_text,
            })
        messages.append({'role': 'user', 'content': tool_results})

    log.warning(f'tool loop: hit MAX_TOOL_ROUNDS ({MAX_TOOL_ROUNDS}) without a final answer')
    return '(stopped after reaching the tool-call round limit without a final answer)'
