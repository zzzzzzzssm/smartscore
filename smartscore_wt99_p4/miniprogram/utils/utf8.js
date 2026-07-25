function encode(text) {
  const value = String(text || '');
  const bytes = [];
  for (let i = 0; i < value.length; i += 1) {
    let code = value.charCodeAt(i);
    if (code >= 0xd800 && code <= 0xdbff && i + 1 < value.length) {
      const low = value.charCodeAt(i + 1);
      if (low >= 0xdc00 && low <= 0xdfff) {
        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
        i += 1;
      }
    }
    if (code < 0x80) {
      bytes.push(code);
    } else if (code < 0x800) {
      bytes.push(0xc0 | (code >> 6), 0x80 | (code & 0x3f));
    } else if (code < 0x10000) {
      bytes.push(0xe0 | (code >> 12),
        0x80 | ((code >> 6) & 0x3f), 0x80 | (code & 0x3f));
    } else {
      bytes.push(0xf0 | (code >> 18),
        0x80 | ((code >> 12) & 0x3f),
        0x80 | ((code >> 6) & 0x3f),
        0x80 | (code & 0x3f));
    }
  }
  return new Uint8Array(bytes);
}

function decode(input) {
  const bytes = input instanceof Uint8Array ? input : new Uint8Array(input || []);
  let output = '';
  for (let i = 0; i < bytes.length;) {
    const first = bytes[i++];
    let code;
    if (first < 0x80) {
      code = first;
    } else if ((first & 0xe0) === 0xc0 && i < bytes.length) {
      code = ((first & 0x1f) << 6) | (bytes[i++] & 0x3f);
    } else if ((first & 0xf0) === 0xe0 && i + 1 < bytes.length) {
      code = ((first & 0x0f) << 12) |
        ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f);
    } else if ((first & 0xf8) === 0xf0 && i + 2 < bytes.length) {
      code = ((first & 0x07) << 18) |
        ((bytes[i++] & 0x3f) << 12) |
        ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f);
    } else {
      output += '\ufffd';
      continue;
    }
    if (code <= 0xffff) {
      output += String.fromCharCode(code);
    } else {
      code -= 0x10000;
      output += String.fromCharCode(0xd800 + (code >> 10),
        0xdc00 + (code & 0x3ff));
    }
  }
  return output;
}

function toArrayBuffer(bytes) {
  const view = bytes instanceof Uint8Array ? bytes : new Uint8Array(bytes || []);
  const output = new Uint8Array(view.length);
  output.set(view);
  return output.buffer;
}

module.exports = { encode, decode, toArrayBuffer };
