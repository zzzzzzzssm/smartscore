const utf8 = require('./utf8');

const SERVICE_UUID = '7A6E0001-5C5A-4B11-9A4A-53534D415254';
const RX_UUID = '7A6E0002-5C5A-4B11-9A4A-53534D415254';
const TX_UUID = '7A6E0003-5C5A-4B11-9A4A-53534D415254';
const DEVICE_PREFIX = 'SmartScore-WT99-';
const MAX_MESSAGE_BYTES = 512;
const CHUNK_BYTES = 20;

function normalizeUuid(uuid) {
  const value = String(uuid || '').replace(/[{}]/g, '').toUpperCase();
  if (/^[0-9A-F]{4}$/.test(value)) {
    return `0000${value}-0000-1000-8000-00805F9B34FB`;
  }
  if (/^[0-9A-F]{8}$/.test(value)) {
    return `${value}-0000-1000-8000-00805F9B34FB`;
  }
  return value;
}

function encodeMessage(message) {
  const bytes = utf8.encode(`${JSON.stringify(message)}\n`);
  if (bytes.length > MAX_MESSAGE_BYTES + 1) {
    throw new Error('消息超过 512 字节');
  }
  return bytes;
}

function splitChunks(bytes) {
  const chunks = [];
  for (let offset = 0; offset < bytes.length; offset += CHUNK_BYTES) {
    chunks.push(bytes.slice(offset, Math.min(offset + CHUNK_BYTES, bytes.length)));
  }
  return chunks;
}

function createReceiver(onMessage, onError) {
  let pending = [];
  let dropping = false;
  return {
    push(buffer) {
      const bytes = new Uint8Array(buffer || new ArrayBuffer(0));
      for (let i = 0; i < bytes.length; i += 1) {
        const byte = bytes[i];
        if (dropping) {
          if (byte === 0x0a) dropping = false;
          continue;
        }
        if (byte === 0x0a) {
          if (pending.length) {
            try {
              onMessage(JSON.parse(utf8.decode(new Uint8Array(pending))));
            } catch (error) {
              onError(`通知解析失败：${error.message}`);
            }
          }
          pending = [];
        } else if (pending.length >= MAX_MESSAGE_BYTES) {
          pending = [];
          dropping = true;
          onError('设备通知超过 512 字节，已丢弃');
        } else {
          pending.push(byte);
        }
      }
    },
    reset() {
      pending = [];
      dropping = false;
    }
  };
}

module.exports = {
  SERVICE_UUID,
  RX_UUID,
  TX_UUID,
  DEVICE_PREFIX,
  CHUNK_BYTES,
  normalizeUuid,
  encodeMessage,
  splitChunks,
  createReceiver
};
