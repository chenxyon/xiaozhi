/**
 * @file     W25FlashDisk.cpp
 * @brief    W25Q128 SPI Flash 统一组件实现 — 双模式分支
 */

#include "W25FlashDisk.h"

// USB_MSC 模式下才需要 USB 相关头文件
#if __has_include("USB.h") && __has_include("USBMSC.h")
  #include "USB.h"
  #include "USBMSC.h"
  #define W25_HAS_USB 1
#else
  #define W25_HAS_USB 0
#endif

W25FlashDisk *W25FlashDisk::s_instance = nullptr;

// ============================================================
// ★ 构造 / 初始化
// ============================================================

W25FlashDisk::W25FlashDisk(const W25Q128Pins &pins, W25DiskMode mode,
                           SPIClass &spi, uint32_t freq)
    : _flash(pins, spi, freq),
      _mode(mode), _spi(spi), _freq(freq), _pins(pins)
{
}

bool W25FlashDisk::begin()
{
    // 1. SPI + JEDEC 检测 (所有模式都做)
    if (!_flash.begin()) {
        ESP_LOGE("W25FlashDisk", "W25Q128 未检出, 接线? JEDEC 读失败");
        return false;
    }

    // 2. 模式分支
    if (_mode == W25DiskMode::STANDALONE) {
        _started = true;
        return true;
    }

    // USB_MSC 模式: 启动 USB 设备 + 注册 MSC 回调
#if W25_HAS_USB
    if (!_usb_attached) {
        _msc = new USBMSC();
        s_instance = this;

        uint32_t blocks = _flash.capacity() / W25Q128::DISK_SECTOR_SIZE;

        _msc->onRead(readCb);
        _msc->onWrite(writeCb);
        _msc->onStartStop(startStopCb);
        _msc->isWritable(true);
        _msc->mediaPresent(true);
        _msc->begin(blocks, W25Q128::DISK_SECTOR_SIZE);

        USB.begin();
        _usb_attached = true;
    }
    _started = true;
    return true;
#else
    ESP_LOGE("W25FlashDisk", "当前框架没有 USB.h / USBMSC.h, 无法使用 USB_MSC 模式");
    return false;
#endif
}

// ============================================================
// ★ acquire / release / poll / firmwareOwns
// ============================================================

bool W25FlashDisk::firmwareOwns() const
{
    return (_mode == W25DiskMode::STANDALONE) || _ejected;
}

bool W25FlashDisk::acquire()
{
    if (_flash.isFsMounted()) return true;

    if (_mode == W25DiskMode::STANDALONE) {
        return _flash.mountFs();
    }

    // USB_MSC: 先通知电脑卸载 → 等 1.5s → mount
#if W25_HAS_USB
    if (_ejected) {
        // 电脑已安全弹出, 不需要再通知电脑
        return _flash.mountFs();
    }
    if (_msc) {
        _msc->mediaPresent(false);
        delay(1500);
    }
    if (!_flash.mountFs()) {
        if (_msc && !_ejected) _msc->mediaPresent(true);  // 失败了归还给电脑
        return false;
    }
    return true;
#else
    return false;
#endif
}

void W25FlashDisk::release()
{
    if (_flash.isFsMounted()) {
        _flash.unmountFs();
    }

    if (_mode == W25DiskMode::USB_MSC && !_ejected) {
#if W25_HAS_USB
        if (_msc) _msc->mediaPresent(true);
#endif
    }
}

void W25FlashDisk::poll()
{
    if (_mode != W25DiskMode::USB_MSC) return;   // STANDALONE 空操作
    if (!_remount_req) return;
    _remount_req = false;

    if (_flash.isFsMounted()) {
        _flash.unmountFs();
    }
#if W25_HAS_USB
    if (_msc) _msc->mediaPresent(true);
#endif
}

// ============================================================
// ★ 高层文件操作 (内部自动 acquire/release, 调用方一行搞定)
// ============================================================

bool W25FlashDisk::readFile(const char *path, char *buf, uint32_t maxLen, uint32_t &outLen)
{
    outLen = 0;
    if (!acquire()) return false;
    bool ok = _flash.readFile(path, buf, maxLen, outLen);
    if (!firmwareOwns()) release();
    return ok;
}

bool W25FlashDisk::writeFile(const char *path, const char *data, uint32_t len, bool append)
{
    if (!acquire()) return false;
    bool ok = _flash.writeFile(path, data, len, append);
    if (!firmwareOwns()) release();
    return ok;
}

// ============================================================
// ★ USB MSC 回调 (TinyUSB 任务上下文, 块级互斥锁由 W25Q128 内部保证)
// ============================================================

int32_t W25FlashDisk::readCb(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    if (!s_instance) return -1;
    return s_instance->_flash.read(lba * W25Q128::DISK_SECTOR_SIZE + offset,
                                   (uint8_t *)buffer, bufsize);
}

int32_t W25FlashDisk::writeCb(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    if (!s_instance) return -1;
    return s_instance->_flash.write(lba * W25Q128::DISK_SECTOR_SIZE + offset,
                                    buffer, bufsize);
}

bool W25FlashDisk::startStopCb(uint8_t power_condition, bool start, bool load_eject)
{
    (void)power_condition;
    if (!s_instance) return false;

    if (load_eject && !start) {
        // 电脑安全弹出 → 固件长期持有
        s_instance->_ejected = true;
    }
    if (load_eject && start) {
        // 电脑要求重新挂载 → poll() 里归还
        s_instance->_remount_req = true;
    }
    return true;
}
