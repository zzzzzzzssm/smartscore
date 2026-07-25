const api = require('../../utils/api');
const scoreLibrary = require('../../utils/score_library');
const protocol = require('../../utils/ble_protocol');
const utf8 = require('../../utils/utf8');

// ESP32-C5 dual-band scanning through ESP-Hosted takes about 20 seconds on
// the WT99 board.  Leave enough room for the scan plus BLE result framing.
const WIFI_SCAN_TIMEOUT_MS = 45000;

function defaultDeviceState() {
  return {
    connected: false,
    name: '谱伴智能乐谱设备',
    ip: ''
  };
}

function delay(milliseconds) {
  return new Promise((resolve) => setTimeout(resolve, milliseconds));
}

function isSmartScoreDevice(item) {
  const name = String(item && (item.name || item.localName) || '');
  return name.indexOf(protocol.DEVICE_PREFIX) === 0;
}

function callWx(method, options = {}) {
  return new Promise((resolve, reject) => {
    wx[method](Object.assign({}, options, { success: resolve, fail: reject }));
  });
}

Page({
  data: {
    device: defaultDeviceState(),
    baseUrl: '',
    bluetooth: {
      available: false,
      scanning: false,
      connected: false,
      connectedName: '',
      connectedDeviceId: '',
      serviceId: '',
      writeCharacteristicId: '',
      notifyCharacteristicId: '',
      writeType: 'write',
      devices: [],
      statusText: '蓝牙未初始化'
    },
    wifiProvision: {
      ssid: '',
      password: '',
      networks: [],
      scanning: false,
      sending: false,
      statusText: '先搜索 Wi-Fi，再通过蓝牙发送配置；开放网络密码可留空'
    },
    connection: {
      step: 1,
      canReach: false,
      serviceOk: false,
      statusText: '先连接蓝牙设备',
      state: '未连接',
      phase: '-',
      progress: '0 / 0',
      lastChecked: '-'
    }
  },

  onLoad() {
    this.requestId = 1;
    this.receiver = protocol.createReceiver(
      (message) => this.handleBluetoothNotifyMessage(message),
      (message) => this.setBluetoothStatus(message)
    );
  },

  onShow() {
    const storedDevice = wx.getStorageSync('deviceState') || defaultDeviceState();
    const storedBluetooth = wx.getStorageSync('bluetoothDevice') || {};
    this.setData({
      device: storedDevice,
      baseUrl: api.getBaseUrl() || '',
      'bluetooth.connectedName': storedBluetooth.name || '',
      'bluetooth.connectedDeviceId': storedBluetooth.deviceId || ''
    });
    this.updateConnectionStep();
    this.startDeviceStatusPolling();
  },

  onHide() {
    this.stopDeviceStatusPolling();
  },

  onUnload() {
    this.pageUnloading = true;
    this.stopDeviceStatusPolling();
    clearTimeout(this.wifiScanTimer);
    this.cleanupBluetooth();
  },

  onBaseUrlInput(event) {
    this.setData({ baseUrl: event.detail.value });
  },

  saveBaseUrl() {
    const baseUrl = api.setBaseUrl(this.data.baseUrl);
    this.setData({ baseUrl });
    this.updateConnectionStep();
    this.refreshDeviceStatus(baseUrl);
    wx.showToast({ title: '已保存地址', icon: 'success' });
  },

  connectDevice() {
    const device = Object.assign({}, this.data.device, { connected: true });
    wx.setStorageSync('deviceState', device);
    this.setData({ device });
    wx.showToast({ title: '已记录设备地址', icon: 'success' });
  },

  disconnectDevice() {
    const device = Object.assign({}, this.data.device, { connected: false });
    wx.setStorageSync('deviceState', device);
    this.setData({ device });
    wx.showToast({ title: '已断开', icon: 'success' });
  },

  syncScore() {
    const current = scoreLibrary.getCurrentScore();
    if (!current) {
      wx.showToast({ title: '请先添加乐谱', icon: 'none' });
      return;
    }

    api.uploadScore(current)
      .then(() => wx.showToast({ title: '同步成功', icon: 'success' }))
      .catch((err) => wx.showModal({
        title: '同步失败',
        content: `${api.errorMessage(err, '同步失败')}\n乐谱仍保存在手机本地，请检查设备地址后重试。`,
        showCancel: false
      }));
  },

  fetchResult() {
    api.getResult()
      .then((data) => wx.showModal({
        title: '练习结果',
        content: JSON.stringify(data, null, 2).slice(0, 600),
        showCancel: false
      }))
      .catch(() => {
        const records = wx.getStorageSync('practiceRecords') || [];
        const latest = records[0];
        if (!latest) {
          wx.showToast({ title: '暂无结果', icon: 'none' });
          return;
        }
        wx.showModal({
          title: '最近练习结果',
          content: `${latest.scoreTitle}\n总分：${latest.totalScore}\n音准：${latest.pitchScore}\n节奏：${latest.rhythmScore}\n完整度：${latest.completeScore}`,
          showCancel: false
        });
      });
  },

  setBluetoothStatus(statusText, extra = {}) {
    this.setData(Object.assign({
      'bluetooth.statusText': statusText
    }, extra));
    this.updateConnectionStep();
  },

  startDeviceStatusPolling() {
    this.stopDeviceStatusPolling();
    this.refreshDeviceStatus();
    this.deviceStatusTimer = setInterval(() => this.refreshDeviceStatus(), 3000);
  },

  stopDeviceStatusPolling() {
    if (this.deviceStatusTimer) {
      clearInterval(this.deviceStatusTimer);
      this.deviceStatusTimer = null;
    }
  },

  formatTimeNow() {
    const now = new Date();
    const pad = (value) => String(value).padStart(2, '0');
    return `${pad(now.getHours())}:${pad(now.getMinutes())}:${pad(now.getSeconds())}`;
  },

  phaseLabel(phase) {
    if (phase === 'CALIBRATING') return '环境音评估';
    if (phase === 'WAITING') return '等待演奏';
    if (phase === 'RECORDING') return '正在记录';
    return '空闲';
  },

  updateConnectionStep() {
    const hasBle = !!(this.data.bluetooth && this.data.bluetooth.connected);
    const canReach = !!(this.data.connection && this.data.connection.canReach);
    const step = !hasBle ? 1 : (canReach ? 3 : 2);
    if (this.data.connection.step !== step) {
      this.setData({ 'connection.step': step });
    }
  },

  refreshDeviceStatus(baseUrl) {
    const explicitUrl = typeof baseUrl === 'string' ? baseUrl : '';
    const url = explicitUrl || this.data.baseUrl || api.getBaseUrl();
    if (!url) {
      const device = Object.assign({}, this.data.device, { connected: false });
      this.setData({
        device,
        'connection.canReach': false,
        'connection.serviceOk': false,
        'connection.statusText': '完成 Wi-Fi 配网后会自动检测设备',
        'connection.state': '未配置地址',
        'connection.phase': '-',
        'connection.progress': '0 / 0',
        'connection.lastChecked': '-'
      });
      wx.setStorageSync('deviceState', device);
      this.updateConnectionStep();
      return;
    }

    api.request('/api/status', { baseUrl: url, timeout: 5000 })
      .then((status) => {
        const state = status.state || 'UNKNOWN';
        const connected = status.ok === true || state === 'WIFI_CONNECTED';
        const device = Object.assign({}, this.data.device, {
          connected,
          name: status.device || this.data.device.name
        });
        this.setData({
          device,
          baseUrl: url,
          'connection.step': this.data.bluetooth.connected ? 3 : 1,
          'connection.canReach': true,
          'connection.serviceOk': connected,
          'connection.statusText': connected
            ? `WT99 网络正常，IP ${status.ip || url.replace(/^https?:\/\//, '')}`
            : `WT99 状态：${state}`,
          'connection.state': state,
          'connection.phase': connected ? '网络正常' : '-',
          'connection.progress': `重试 ${Number(status.retry_count || 0)} 次`,
          'connection.lastChecked': this.formatTimeNow()
        });
        wx.setStorageSync('deviceState', device);
        if (connected) {
          api.request('/api/ping', { baseUrl: url, timeout: 5000 })
            .catch(() => {});
        }
      })
      .catch((err) => {
        const device = Object.assign({}, this.data.device, { connected: false });
        this.setData({
          device,
          'connection.canReach': false,
          'connection.serviceOk': false,
          'connection.statusText': api.errorMessage(err, '无法连接到设备'),
          'connection.state': '离线',
          'connection.phase': '-',
          'connection.progress': '0 / 0',
          'connection.lastChecked': this.formatTimeNow()
        });
        wx.setStorageSync('deviceState', device);
        this.updateConnectionStep();
      });
  },

  ensureBluetoothReady() {
    if (this.data.bluetooth.available) {
      return Promise.resolve();
    }
    return new Promise((resolve, reject) => {
      wx.openBluetoothAdapter({
        mode: 'central',
        success: () => {
          this.bindBluetoothEvents();
          this.setBluetoothStatus('蓝牙已就绪', {
            'bluetooth.available': true
          });
          resolve();
        },
        fail: (err) => {
          const message = err.errCode === 10001
            ? '请先打开手机蓝牙'
            : api.errorMessage(err, '蓝牙初始化失败');
          this.setBluetoothStatus(message, {
            'bluetooth.available': false
          });
          reject(new Error(message));
        }
      });
    });
  },

  bindBluetoothEvents() {
    if (this.bluetoothEventsBound) return;
    this.bluetoothEventsBound = true;

    this.onBluetoothAdapterStateChange = (res) => {
      if (this.pageUnloading) return;
      this.setData({
        'bluetooth.available': !!res.available,
        'bluetooth.scanning': !!res.discovering
      });
      if (!res.available) {
        this.setBluetoothStatus('手机蓝牙已关闭', {
          'bluetooth.connected': false
        });
      }
    };

    this.onBluetoothConnectionStateChange = (res) => {
      if (this.pageUnloading) return;
      if (res.deviceId !== this.data.bluetooth.connectedDeviceId) return;
      if (!res.connected) {
        if (this.receiver) this.receiver.reset();
        clearTimeout(this.wifiScanTimer);
        this.wifiScanRequestId = null;
        this.setBluetoothStatus('蓝牙连接已断开', {
          'bluetooth.connected': false,
          'bluetooth.serviceId': '',
          'bluetooth.writeCharacteristicId': '',
          'bluetooth.notifyCharacteristicId': '',
          'bluetooth.writeType': 'write',
          'wifiProvision.scanning': false
        });
      }
    };

    this.onBluetoothDeviceFound = (res) => {
      if (this.pageUnloading) return;
      this.mergeBluetoothDevices(res.devices || []);
    };

    this.onBluetoothValueChange = (res) => {
      if (this.pageUnloading) return;
      if (res.deviceId !== this.data.bluetooth.connectedDeviceId) return;
      if (protocol.normalizeUuid(res.characteristicId) !==
          protocol.normalizeUuid(protocol.TX_UUID)) return;
      if (this.receiver) this.receiver.push(res.value);
    };

    wx.onBluetoothAdapterStateChange(this.onBluetoothAdapterStateChange);
    wx.onBLEConnectionStateChange(this.onBluetoothConnectionStateChange);
    wx.onBluetoothDeviceFound(this.onBluetoothDeviceFound);
    wx.onBLECharacteristicValueChange(this.onBluetoothValueChange);
  },

  mergeBluetoothDevices(foundDevices) {
    const visible = foundDevices
      .filter((item) => item && item.deviceId)
      .map((item) => Object.assign({}, item, {
        name: item.name || item.localName || ''
      }))
      .filter((item) => item.name && isSmartScoreDevice(item));
    if (!visible.length) return;

    const map = {};
    this.data.bluetooth.devices.forEach((item) => {
      map[item.deviceId] = item;
    });
    visible.forEach((item) => {
      map[item.deviceId] = Object.assign({}, map[item.deviceId] || {}, item);
    });

    const devices = Object.keys(map)
      .map((key) => map[key])
      .sort((a, b) => {
        return Number(b.RSSI || -100) - Number(a.RSSI || -100);
      })
      .slice(0, 20);
    this.setData({ 'bluetooth.devices': devices });
  },

  startBluetoothScan() {
    this.ensureBluetoothReady()
      .then(() => new Promise((resolve, reject) => {
        this.setBluetoothStatus('正在扫描附近 BLE 设备...', {
          'bluetooth.devices': [],
          'bluetooth.scanning': true
        });
        wx.startBluetoothDevicesDiscovery({
          allowDuplicatesKey: false,
          interval: 0,
          success: resolve,
          fail: (err) => reject(new Error(api.errorMessage(err, '蓝牙扫描失败')))
        });
      }))
      .then(() => {
        clearTimeout(this.bluetoothScanTimer);
        this.bluetoothScanTimer = setTimeout(() => this.stopBluetoothScan(), 12000);
      })
      .catch((err) => wx.showToast({ title: api.errorMessage(err, '蓝牙扫描失败'), icon: 'none' }));
  },

  async stopBluetoothScan(silent = false) {
    clearTimeout(this.bluetoothScanTimer);
    if (!this.data.bluetooth.scanning) return;
    try {
      await callWx('stopBluetoothDevicesDiscovery');
    } catch (err) {
      // The adapter can stop discovery before this page receives the callback.
    }
    if (!silent) {
      this.setBluetoothStatus(
        this.data.bluetooth.connected ? '蓝牙设备已连接' : '扫描已停止',
        { 'bluetooth.scanning': false }
      );
    }
  },

  async connectBluetoothDevice(event) {
    const deviceId = event.currentTarget.dataset.id;
    const target = this.data.bluetooth.devices.find((item) => item.deviceId === deviceId);
    if (!deviceId) return;

    await this.stopBluetoothScan();
    wx.showLoading({ title: '连接蓝牙中' });
    try {
      if (this.data.bluetooth.connectedDeviceId) {
        await this.disconnectBluetooth();
      }
      await callWx('createBLEConnection', { deviceId, timeout: 12000 });
      const name = target && (target.name || target.localName) || 'SmartScore-WT99';
      wx.setStorageSync('bluetoothDevice', { deviceId, name });
      this.setBluetoothStatus('正在发现 WT99 服务...', {
        'bluetooth.connected': true,
        'connection.step': 2,
        'connection.canReach': false,
        'connection.serviceOk': false,
        'connection.statusText': '蓝牙已连接，请配置 Wi-Fi',
        'connection.state': '等待配网',
        'connection.phase': '-',
        'connection.progress': '0 / 0',
        'bluetooth.connectedDeviceId': deviceId,
        'bluetooth.connectedName': name
      });
      if (wx.setBLEMTU) {
        wx.setBLEMTU({ deviceId, mtu: 128, fail: () => {} });
      }
      await this.discoverBluetoothProfile(deviceId);
      wx.showToast({ title: '蓝牙已连接', icon: 'success' });
    } catch (err) {
      if (this.receiver) this.receiver.reset();
      wx.showModal({
        title: '蓝牙连接失败',
        content: api.errorMessage(err, '请靠近设备后重试'),
        showCancel: false
      });
    } finally {
      wx.hideLoading();
    }
  },

  async discoverBluetoothProfile(deviceId) {
    const serviceResult = await callWx('getBLEDeviceServices', { deviceId });
    const service = (serviceResult.services || []).find((item) =>
      protocol.normalizeUuid(item.uuid) === protocol.normalizeUuid(protocol.SERVICE_UUID));
    if (!service) {
      throw new Error('未发现 SmartScore WT99 BLE 服务');
    }
    await this.discoverBluetoothCharacteristics(deviceId, service.uuid);
  },

  async discoverBluetoothCharacteristics(deviceId, serviceId) {
    const result = await callWx('getBLEDeviceCharacteristics', {
      deviceId,
      serviceId
    });
    const characteristics = result.characteristics || [];
    const writable = characteristics.find((item) =>
      protocol.normalizeUuid(item.uuid) === protocol.normalizeUuid(protocol.RX_UUID) &&
      (item.properties.write || item.properties.writeNoResponse));
    const notify = characteristics.find((item) =>
      protocol.normalizeUuid(item.uuid) === protocol.normalizeUuid(protocol.TX_UUID) &&
      (item.properties.notify || item.properties.indicate));
    if (!writable || !notify) {
      throw new Error('WT99 RX/TX 特征不完整');
    }

    this.setBluetoothStatus('WT99 服务已发现，正在开启通知...', {
      'bluetooth.serviceId': serviceId,
      'bluetooth.writeCharacteristicId': writable.uuid,
      'bluetooth.notifyCharacteristicId': notify.uuid,
      'bluetooth.writeType': writable.properties.write ? 'write' : 'writeNoResponse'
    });
    await callWx('notifyBLECharacteristicValueChange', {
      deviceId,
      serviceId,
      characteristicId: notify.uuid,
      state: true
    });
    this.setBluetoothStatus('蓝牙设备已连接，可接收配网状态');
    await delay(100);
    await this.sendBluetoothMessage({ id: this.nextRequestId(), cmd: 'get_device_info' });
    await this.sendBluetoothMessage({ id: this.nextRequestId(), cmd: 'get_status' });
  },

  async disconnectBluetooth(silent = false) {
    const deviceId = this.data.bluetooth.connectedDeviceId;
    if (!deviceId) return;
    try {
      await callWx('closeBLEConnection', { deviceId });
    } catch (err) {
      // The connection can already be closed by the system.
    }
    if (this.receiver) this.receiver.reset();
    clearTimeout(this.wifiScanTimer);
    this.wifiScanRequestId = null;
    wx.removeStorageSync('bluetoothDevice');
    if (silent) return;
    this.setBluetoothStatus('蓝牙已断开', {
      'bluetooth.connected': false,
      'bluetooth.connectedName': '',
      'bluetooth.connectedDeviceId': '',
      'bluetooth.serviceId': '',
      'bluetooth.writeCharacteristicId': '',
      'bluetooth.notifyCharacteristicId': '',
      'bluetooth.writeType': 'write',
      'connection.step': 1,
      'connection.canReach': false,
      'connection.serviceOk': false,
      'connection.statusText': '先连接蓝牙设备',
      'connection.state': '未连接',
      'connection.phase': '-',
      'connection.progress': '0 / 0',
      'wifiProvision.scanning': false,
      'wifiProvision.password': ''
    });
  },

  async cleanupBluetooth() {
    clearTimeout(this.bluetoothScanTimer);
    await this.stopBluetoothScan(true);
    await this.disconnectBluetooth(true);
    if (this.bluetoothEventsBound) {
      if (wx.offBluetoothAdapterStateChange) {
        wx.offBluetoothAdapterStateChange(this.onBluetoothAdapterStateChange);
      }
      if (wx.offBLEConnectionStateChange) {
        wx.offBLEConnectionStateChange(this.onBluetoothConnectionStateChange);
      }
      if (wx.offBluetoothDeviceFound) {
        wx.offBluetoothDeviceFound(this.onBluetoothDeviceFound);
      }
      if (wx.offBLECharacteristicValueChange) {
        wx.offBLECharacteristicValueChange(this.onBluetoothValueChange);
      }
      this.bluetoothEventsBound = false;
    }
    if (this.data.bluetooth.available && wx.closeBluetoothAdapter) {
      wx.closeBluetoothAdapter({ complete: () => {} });
    }
  },

  getWifiFailureText(reason) {
    const map = {
      AUTH_FAIL: 'Wi-Fi 密码错误或认证失败',
      NO_AP_FOUND: '没有找到该 Wi-Fi，请检查名称和 2.4GHz 信号',
      ASSOC_FAIL: '路由器拒绝连接，请检查加密方式',
      TIMEOUT: '连接超时，请检查密码和信号',
      CONNECT_FAILED: 'Wi-Fi 连接失败，请重试',
      START_FAILED: '设备启动 Wi-Fi 失败',
      RETRY_FAILED: '达到最大重试次数',
      NVS_SAVE_FAILED: 'Wi-Fi 已连接，但保存配置失败',
      MESSAGE_TOO_LARGE: '配置消息超过 512 字节',
      wifi_scan_busy: '设备正在连接或扫描 Wi-Fi，请稍后重试',
      wifi_scan_queue_full: '设备命令队列繁忙，请稍后重试',
      wifi_scan_no_memory: '设备扫描内存不足，请稍后重试',
      wifi_scan_failed: '设备扫描 Wi-Fi 失败，请重试',
      wifi_not_ready: '设备 Wi-Fi 模块尚未准备好'
    };
    return map[reason] || api.messageText(reason, '请检查网络');
  },

  handleBluetoothNotifyMessage(data) {
    if (!data || typeof data !== 'object') return;
    let statusText = '已收到设备状态';

    const isScanMessage = data.event === 'wifi_scan' ||
      data.event === 'wifi_network' ||
      data.status === 'wifi_scan_start' ||
      data.status === 'wifi_network' ||
      data.status === 'wifi_scan_done';
    if (isScanMessage &&
        (this.wifiScanRequestId == null ||
         Number(data.id) !== Number(this.wifiScanRequestId))) {
      return;
    }

    if (data.status === 'wifi_scan_start') {
      statusText = '设备正在搜索附近 Wi-Fi...';
      this.armWifiScanTimeout(this.wifiScanRequestId);
      this.setData({
        'wifiProvision.scanning': true,
        'wifiProvision.networks': [],
        'wifiProvision.statusText': statusText
      });
    } else if (data.status === 'wifi_network' || data.event === 'wifi_network') {
      const networks = this.addWifiProvisionNetworks([{
        ssid: String(data.ssid || '').trim(),
        secure: !data.open,
        signalStrength: Number(data.rssi || 0),
        channel: Number(data.channel || 0)
      }]);
      statusText = `设备已发现 ${networks.length} 个 Wi-Fi...`;
      this.setData({
        'wifiProvision.scanning': true,
        'wifiProvision.statusText': statusText
      });
    } else if (data.status === 'wifi_scan_done') {
      clearTimeout(this.wifiScanTimer);
      this.wifiScanRequestId = null;
      const count = this.data.wifiProvision.networks.length;
      statusText = count
        ? `设备找到 ${count} 个 Wi-Fi，请选择网络`
        : '设备没有搜到可见 Wi-Fi，可以手动输入名称';
      this.setData({
        'wifiProvision.scanning': false,
        'wifiProvision.statusText': statusText
      });
    } else if (data.event === 'wifi_scan' && data.status === 'failed') {
      clearTimeout(this.wifiScanTimer);
      this.wifiScanRequestId = null;
      statusText = `Wi-Fi 搜索失败：${this.getWifiFailureText(data.reason)}`;
      this.setData({
        'wifiProvision.scanning': false,
        'wifiProvision.statusText': statusText
      });
    } else if (data.event === 'wifi_state') {
      if (data.state === 'connecting') {
        statusText = '正在连接 Wi-Fi';
        api.setBaseUrl('');
        this.setData({
          baseUrl: '',
          'connection.step': 2,
          'connection.canReach': false,
          'connection.serviceOk': false,
          'connection.state': '正在连接',
          'connection.statusText': statusText
        });
      } else if (data.state === 'connected' && data.ip) {
        const url = `http://${data.ip}`;
        api.setBaseUrl(url);
        statusText = `Wi-Fi 已连接，IP ${data.ip}`;
        this.setData({
          baseUrl: url,
          'wifiProvision.password': '',
          'wifiProvision.sending': false,
          'connection.step': 3,
          'connection.state': 'WIFI_CONNECTED',
          'connection.statusText': 'Wi-Fi 已连接，正在检测 WT99 网络接口'
        });
        setTimeout(() => this.refreshDeviceStatus(url), 1200);
      } else if (data.state === 'failed') {
        statusText = `Wi-Fi 连接失败：${this.getWifiFailureText(data.reason)}`;
        api.setBaseUrl('');
        this.setData({
          baseUrl: '',
          'wifiProvision.password': '',
          'wifiProvision.sending': false,
          'connection.step': 2,
          'connection.canReach': false,
          'connection.serviceOk': false,
          'connection.state': '连接失败',
          'connection.statusText': statusText
        });
      } else if (data.state === 'waiting_credentials') {
        statusText = '设备正在等待 Wi-Fi 配置';
        api.setBaseUrl('');
        this.setData({
          baseUrl: '',
          'wifiProvision.password': '',
          'wifiProvision.sending': false,
          'connection.step': 2,
          'connection.canReach': false,
          'connection.serviceOk': false,
          'connection.state': '等待配网',
          'connection.statusText': statusText
        });
      } else {
        statusText = `Wi-Fi 状态：${data.state || '未知'}`;
      }
    } else if (data.event === 'device_info') {
      statusText = `WT99 信息已确认：BLE ${data.ble ? '可用' : '不可用'}，Wi-Fi ${data.wifi ? '可用' : '不可用'}`;
    } else if (data.event === 'error') {
      statusText = `设备返回错误：${this.getWifiFailureText(data.error)}`;
    }

    this.setData({ 'wifiProvision.statusText': statusText });
    this.setBluetoothStatus(statusText);
  },

  onBleWifiSsidInput(event) {
    this.setData({ 'wifiProvision.ssid': event.detail.value });
  },

  onBleWifiPasswordInput(event) {
    this.setData({ 'wifiProvision.password': event.detail.value });
  },

  armWifiScanTimeout(requestId) {
    clearTimeout(this.wifiScanTimer);
    this.wifiScanTimer = setTimeout(() => {
      if (this.wifiScanRequestId !== requestId) return;
      // Keep the request ID so a delayed Hosted scan result is still accepted.
      // Starting another scan naturally replaces the ID and rejects old data.
      this.setData({
        'wifiProvision.scanning': false,
        'wifiProvision.statusText': '设备搜索等待超时，可以重试；原结果稍后返回时仍会自动显示'
      });
    }, WIFI_SCAN_TIMEOUT_MS);
  },

  async startWifiScan() {
    const ble = this.data.bluetooth;
    if (!ble.connected || !ble.serviceId || !ble.writeCharacteristicId) {
      this.setData({
        'wifiProvision.statusText': '请先连接 SmartScore WT99 蓝牙，再搜索 Wi-Fi'
      });
      wx.showToast({ title: '请先连接蓝牙', icon: 'none' });
      return;
    }

    if (this.data.wifiProvision.scanning) {
      return;
    }
    if (this.data.wifiProvision.sending ||
        this.data.connection.state === '正在连接' ||
        this.data.connection.state === 'WIFI_CONNECTED') {
      this.setData({
        'wifiProvision.statusText': '设备正在连接或已经连接 Wi-Fi，无需重新搜索'
      });
      return;
    }

    clearTimeout(this.wifiScanTimer);
    const requestId = this.nextRequestId();
    this.wifiScanRequestId = requestId;
    this.setData({
      'wifiProvision.scanning': true,
      'wifiProvision.networks': [],
      'wifiProvision.statusText': '正在请求 WT99 设备搜索附近 Wi-Fi...'
    });
    try {
      await this.sendBluetoothMessage({ id: requestId, cmd: 'scan_wifi' });
      this.armWifiScanTimeout(requestId);
    } catch (err) {
      this.wifiScanRequestId = null;
      this.setData({
        'wifiProvision.scanning': false,
        'wifiProvision.statusText': api.errorMessage(err, 'BLE 扫描命令发送失败')
      });
    }
  },

  addWifiProvisionNetworks(nextNetworks) {
    const map = {};
    this.data.wifiProvision.networks.concat(nextNetworks || []).forEach((item) => {
      const ssid = String(item.ssid || '').trim();
      if (!ssid) return;
      const signalStrength = Number(item.signalStrength || 0);
      if (!map[ssid] || signalStrength > map[ssid].signalStrength) {
        map[ssid] = {
          ssid,
          secure: !!item.secure,
          signalStrength,
          channel: item.channel || 0
        };
      }
    });

    const networks = Object.keys(map)
      .map((key) => map[key])
      .sort((a, b) => b.signalStrength - a.signalStrength)
      .slice(0, 15);
    this.setData({ 'wifiProvision.networks': networks });
    return networks;
  },

  selectWifiNetwork(event) {
    const ssid = event.currentTarget.dataset.ssid || '';
    const secure = event.currentTarget.dataset.secure === true ||
      event.currentTarget.dataset.secure === 'true';
    this.setData({
      'wifiProvision.ssid': ssid,
      'wifiProvision.password': secure ? this.data.wifiProvision.password : '',
      'wifiProvision.statusText': secure
        ? `已选择 ${ssid}，请输入 Wi-Fi 密码`
        : `已选择开放网络 ${ssid}，可直接发送`
    });
  },

  async sendBluetoothWifiConfig() {
    const ssid = String(this.data.wifiProvision.ssid || '').trim();
    if (!ssid) {
      wx.showToast({ title: '请输入 Wi-Fi 名称', icon: 'none' });
      return;
    }

    this.setData({
      'wifiProvision.sending': true,
      'wifiProvision.statusText': '正在通过蓝牙发送 Wi-Fi 配置...'
    });
    try {
      await this.sendBluetoothMessage({
        id: this.nextRequestId(),
        cmd: 'set_wifi',
        ssid,
        password: this.data.wifiProvision.password || ''
      });
      this.setBluetoothStatus('Wi-Fi 配置已发送，正在等待设备连接...');
      this.setData({
        'wifiProvision.sending': false,
        'wifiProvision.statusText': 'Wi-Fi 配置已发送，正在等待设备连接...'
      });
      wx.showToast({ title: 'Wi-Fi 配置已发送', icon: 'success' });
    } catch (err) {
      const message = api.errorMessage(err, '蓝牙发送失败');
      this.setData({
        'wifiProvision.sending': false,
        'wifiProvision.statusText': message
      });
      this.setBluetoothStatus(message);
      wx.showToast({ title: message, icon: 'none' });
    }
  },

  nextRequestId() {
    const id = Number(this.requestId || 1);
    this.requestId = id >= 2147483647 ? 1 : id + 1;
    return id;
  },

  async sendBluetoothMessage(message) {
    const ble = this.data.bluetooth;
    if (!ble.connected || !ble.serviceId || !ble.writeCharacteristicId) {
      throw new Error('未发现 WT99 可写蓝牙特征');
    }

    const chunks = protocol.splitChunks(protocol.encodeMessage(message));
    for (let index = 0; index < chunks.length; index += 1) {
      let lastError;
      for (let attempt = 0; attempt < 3; attempt += 1) {
        try {
          await callWx('writeBLECharacteristicValue', {
            deviceId: ble.connectedDeviceId,
            serviceId: ble.serviceId,
            characteristicId: ble.writeCharacteristicId,
            value: utf8.toArrayBuffer(chunks[index]),
            writeType: ble.writeType || 'write'
          });
          lastError = null;
          break;
        } catch (err) {
          lastError = err;
          if (attempt < 2) await delay(150);
        }
      }
      if (lastError) throw lastError;
      if (index + 1 < chunks.length) await delay(60);
    }
  },

  async sendBluetoothPing() {
    try {
      await this.sendBluetoothMessage({ id: this.nextRequestId(), cmd: 'get_status' });
      wx.showToast({ title: '状态查询已发送', icon: 'success' });
    } catch (err) {
      wx.showToast({ title: api.errorMessage(err, '蓝牙测试失败'), icon: 'none' });
    }
  },

  openAudioCenter() {
    wx.navigateTo({
      url: '/pages/audio/audio',
      fail: (err) => wx.showToast({ title: api.errorMessage(err, '跳转失败'), icon: 'none' })
    });
  }
});
