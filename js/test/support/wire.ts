const encoder = new TextEncoder();

/** Builds raw wire payloads for delivering scripted inbound messages in unit tests. */
export const wire = {
  request(id: string, payloadJson = '{}'): Uint8Array {
    return encoder.encode(`{"v":1,"t":"req","id":"${id}","p":${payloadJson}}`);
  },
  heartbeat(id: string): Uint8Array {
    return encoder.encode(`{"v":1,"t":"hb","id":"${id}"}`);
  },
  progress(id: string, payloadJson: string): Uint8Array {
    return encoder.encode(`{"v":1,"t":"prg","id":"${id}","p":${payloadJson}}`);
  },
  resultOk(id: string, payloadJson: string): Uint8Array {
    return encoder.encode(`{"v":1,"t":"res","id":"${id}","ok":true,"p":${payloadJson}}`);
  },
  resultError(id: string, code: string): Uint8Array {
    return encoder.encode(`{"v":1,"t":"res","id":"${id}","ok":false,"e":"${code}"}`);
  },
  resultAck(id: string): Uint8Array {
    return encoder.encode(`{"v":1,"t":"rak","id":"${id}"}`);
  },
  cancel(id: string): Uint8Array {
    return encoder.encode(`{"v":1,"t":"cnl","id":"${id}"}`);
  },
  raw(text: string): Uint8Array {
    return encoder.encode(text);
  },
  json(value: unknown): Uint8Array {
    return encoder.encode(JSON.stringify(value));
  },
};
