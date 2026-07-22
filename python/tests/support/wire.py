"""Raw wire payload builders for delivering scripted inbound messages in unit tests."""

from __future__ import annotations


def req(command_id: str, payload_json: str = "{}") -> bytes:
    return f'{{"v":1,"t":"req","id":"{command_id}","p":{payload_json}}}'.encode()


def hb(command_id: str) -> bytes:
    return f'{{"v":1,"t":"hb","id":"{command_id}"}}'.encode()


def prg(command_id: str, payload_json: str) -> bytes:
    return f'{{"v":1,"t":"prg","id":"{command_id}","p":{payload_json}}}'.encode()


def res_ok(command_id: str, payload_json: str) -> bytes:
    return f'{{"v":1,"t":"res","id":"{command_id}","ok":true,"p":{payload_json}}}'.encode()


def res_err(command_id: str, code: str) -> bytes:
    return f'{{"v":1,"t":"res","id":"{command_id}","ok":false,"e":"{code}"}}'.encode()


def rak(command_id: str) -> bytes:
    return f'{{"v":1,"t":"rak","id":"{command_id}"}}'.encode()


def cnl(command_id: str) -> bytes:
    return f'{{"v":1,"t":"cnl","id":"{command_id}"}}'.encode()


def raw(text: str) -> bytes:
    return text.encode("utf-8")
