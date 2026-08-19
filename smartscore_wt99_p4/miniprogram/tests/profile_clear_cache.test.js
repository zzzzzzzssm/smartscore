const assert = require('assert');
const path = require('path');

const storage = new Map([
  ['apiBaseUrl', 'http://smart-score.local'],
  ['bluetoothDevice', { deviceId: 'ble-1' }],
  ['deviceState', { connected: true }],
  ['scores', [{ id: 'score-1' }]],
  ['currentScoreId', 'score-1'],
  ['currentExternalScore', { id: 'external-1' }],
  ['practiceRecords', [{ id: 'record-1', adviceText: '保留建议' }]],
  ['scoreExamplesRemovedV1', true]
]);
const removed = [];
const app = { globalData: { apiBaseUrl: 'http://smart-score.local' } };

global.wx = {
  getStorageSync(key) { return storage.get(key); },
  removeStorageSync(key) {
    removed.push(key);
    storage.delete(key);
  },
  showModal(options) { options.success({ confirm: true }); },
  showToast() {}
};
global.getApp = () => app;

let definition;
global.Page = (value) => { definition = value; };
const pagePath = path.resolve(__dirname, '../pages/profile/profile.js');
delete require.cache[require.resolve(pagePath)];
require(pagePath);

definition.clearCache();
assert.deepStrictEqual(removed.sort(), ['apiBaseUrl', 'bluetoothDevice', 'deviceState']);
assert.strictEqual(app.globalData.apiBaseUrl, '');
assert.deepStrictEqual(storage.get('scores'), [{ id: 'score-1' }]);
assert.strictEqual(storage.get('currentScoreId'), 'score-1');
assert.deepStrictEqual(storage.get('currentExternalScore'), { id: 'external-1' });
assert.strictEqual(storage.get('practiceRecords')[0].adviceText, '保留建议');
assert.strictEqual(storage.get('scoreExamplesRemovedV1'), true);

console.log('profile clear cache tests passed');
