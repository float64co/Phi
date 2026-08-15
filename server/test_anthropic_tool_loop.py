#!/usr/bin/env python3
"""
Standalone, no-network test for run_tool_loop's response-handling branches
(server/anthropic_client.py). Unlike test_chat_protocol.py (which makes
REAL Anthropic API calls and needs a live server.py + API key), this mocks
only the actual HTTP boundary (_post) -- the real seam between "network
call" and "everything this module does with the result" -- so it runs
anywhere, for free, and exercises the exact logic that produced a real bug:
a tool call carrying enough generated geometry to hit MAX_TOKENS mid-
response used to silently return '' with no indication anything went
wrong (see anthropic_client.py's own comment on stop_reason=='max_tokens'
for the fix this guards).

Usage: python3 test_anthropic_tool_loop.py
"""
import sys

import anthropic_client as ac
from anthropic_client import run_tool_loop, AnthropicError

_fail = False


def check(cond, msg):
    global _fail
    print(f"  {'PASS' if cond else 'FAIL'}: {msg}")
    if not cond:
        _fail = True


def _block(type_, **kw):
    return {'type': type_, **kw}


def with_mocked_post(responses):
    """responses: list of dicts, one per expected _post() call, returned
    in order. Returns the mock and a list capturing every request body
    _post was actually called with (so a test can assert on max_tokens/
    messages/tools sent, not just what came back)."""
    calls = []
    it = iter(responses)

    def fake_post(body):
        # Snapshot 'messages' as a NEW list at call time -- run_tool_loop
        # keeps mutating the same list object via .append() for the rest
        # of the loop, so storing the bare reference would make every
        # earlier call's recorded state silently reflect LATER rounds too.
        calls.append({**body, 'messages': list(body['messages'])})
        return next(it)

    return fake_post, calls


def test_normal_completion(monkeypatch_post):
    print("=== 1: normal end_turn completion, no tools involved ===")
    fake_post, calls = with_mocked_post([
        {'content': [_block('text', text='hello there')], 'stop_reason': 'end_turn'},
    ])
    ac._post = fake_post
    result = run_tool_loop('sys', 'hi', [], {})
    check(result == 'hello there', f"returns the plain text block verbatim, got {result!r}")
    check(len(calls) == 1, "exactly one API call for a single-turn reply")


def test_tool_round_trip():
    print("=== 2: a real tool_use round trip dispatches and returns the follow-up text ===")
    seen_input = {}

    def tool_fn(input_):
        seen_input.update(input_)
        return 'the answer is 42'

    fake_post, calls = with_mocked_post([
        {'content': [_block('tool_use', id='t1', name='get_thing', input={'x': 1})], 'stop_reason': 'tool_use'},
        {'content': [_block('text', text='it is 42')], 'stop_reason': 'end_turn'},
    ])
    ac._post = fake_post
    result = run_tool_loop('sys', 'what is it', [{'name': 'get_thing'}], {'get_thing': tool_fn})
    check(result == 'it is 42', f"returns the SECOND round's text after the tool round-trips, got {result!r}")
    check(seen_input == {'x': 1}, "the dispatched tool actually received the model's real tool input")
    check(len(calls) == 2, "two real API calls: the tool-use round, then the follow-up")
    # The tool_result must reference the exact tool_use_id the model sent --
    # a mismatch here is invisible until a real multi-tool-call turn breaks.
    second_call_msgs = calls[1]['messages']
    tool_result_msg = second_call_msgs[-1]['content'][0]
    check(tool_result_msg['tool_use_id'] == 't1', "the tool_result block echoes back the model's own tool_use_id")
    check(tool_result_msg['content'] == 'the answer is 42', "the tool_result content is exactly what the dispatched function returned")


def test_tool_raises_is_reported_not_fatal():
    print("=== 3: a dispatch function raising doesn't abort the whole reply ===")

    def bad_tool(_input):
        raise ValueError('boom')

    fake_post, calls = with_mocked_post([
        {'content': [_block('tool_use', id='t1', name='bad', input={})], 'stop_reason': 'tool_use'},
        {'content': [_block('text', text='handled it')], 'stop_reason': 'end_turn'},
    ])
    ac._post = fake_post
    result = run_tool_loop('sys', 'do it', [{'name': 'bad'}], {'bad': bad_tool})
    check(result == 'handled it', "the loop continues to a real final answer even though the tool raised")
    tool_result_content = calls[1]['messages'][-1]['content'][0]['content']
    check('boom' in tool_result_content, "the exception message is reported back to the model as the tool's result, not swallowed")


def test_max_tokens_with_partial_text():
    print("=== 4: stop_reason=max_tokens WITH some text recovered -- returned, clearly marked as cut off ===")
    fake_post, calls = with_mocked_post([
        {'content': [_block('text', text='building a house, starting with')], 'stop_reason': 'max_tokens',
         'usage': {'input_tokens': 50, 'output_tokens': 4096}},
    ])
    ac._post = fake_post
    result = run_tool_loop('sys', 'build a house', [], {})
    check('building a house, starting with' in result, "the partial text that WAS generated is not thrown away")
    check('cut off' in result.lower(), "the truncation is disclosed to the user, not presented as a complete answer")


def test_max_tokens_with_no_text_the_original_bug():
    print("=== 5: stop_reason=max_tokens with ZERO text (all budget spent on a truncated tool call) ===")
    print("    -- this is the exact shape of response that used to silently return '' (the reported bug)")
    fake_post, calls = with_mocked_post([
        {'content': [_block('tool_use', id='t1', name='create_mesh_object', input={'positions': [1, 2, 3]})],
         'stop_reason': 'max_tokens', 'usage': {'input_tokens': 50, 'output_tokens': 4096}},
    ])
    ac._post = fake_post
    result = run_tool_loop('sys', 'turn this into a house', [{'name': 'create_mesh_object'}], {})
    check(result != '', "no longer silently empty -- this is the actual regression check")
    check('cut off' in result.lower() or 'token limit' in result.lower(),
          f"the empty-text-because-truncated case is explained, not silent, got {result!r}")


def test_error_response_raises():
    print("=== 6: an API-level error is raised as AnthropicError, not swallowed into a text reply ===")
    fake_post, _ = with_mocked_post([
        {'error': {'message': 'overloaded_error: try again'}},
    ])
    ac._post = fake_post
    try:
        run_tool_loop('sys', 'hi', [], {})
        check(False, "expected AnthropicError to be raised")
    except AnthropicError as e:
        check('overloaded' in str(e), f"the real API error message is preserved, got {e}")


def test_round_limit_exhaustion():
    print("=== 7: repeated tool_use forever hits MAX_TOOL_ROUNDS and returns a real explanation, not an infinite loop ===")
    responses = [
        {'content': [_block('tool_use', id=f't{i}', name='noop', input={})], 'stop_reason': 'tool_use'}
        for i in range(ac.MAX_TOOL_ROUNDS)
    ]
    fake_post, calls = with_mocked_post(responses)
    ac._post = fake_post
    result = run_tool_loop('sys', 'loop forever', [{'name': 'noop'}], {'noop': lambda _i: 'ok'})
    check('round limit' in result.lower() or 'without a final answer' in result.lower(),
          f"a bounded, explained stop rather than hanging, got {result!r}")
    check(len(calls) == ac.MAX_TOOL_ROUNDS, f"exactly MAX_TOOL_ROUNDS ({ac.MAX_TOOL_ROUNDS}) calls were made, not more")


def main():
    real_post = ac._post
    try:
        test_normal_completion(None)
        test_tool_round_trip()
        test_tool_raises_is_reported_not_fatal()
        test_max_tokens_with_partial_text()
        test_max_tokens_with_no_text_the_original_bug()
        test_error_response_raises()
        test_round_limit_exhaustion()
    finally:
        ac._post = real_post

    print("\n[test_anthropic_tool_loop] RESULT:", "FAIL (see above)" if _fail else "PASS (all checks passed)")
    return 1 if _fail else 0


if __name__ == "__main__":
    sys.exit(main())
