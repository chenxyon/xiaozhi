/**
 * @file     W25FlashDisk.h
 * @brief    W25Q128 SPI NOR Flash 统一组件 — 双模式合一
 *
 * 两种模式通过构造时传参选择，对外 API 完全一致：
 *
 *   STANDALONE — 纯固件独占：mountFs 直接可用，不启动 USB
 *   USB_MSC    — U 盘模式：USB 枚举为 FAT 卷，电脑可读写，固件 acquire/release 窗口访问
 *
 * 用法（一个类搞定两种模式）：
 *
 *   W25FlashDisk disk(W25Q128Pins{.cs=39, .sck=40, .mosi=41, .miso=42},
 *                     W25DiskMode::USB_MSC);   // 或 STANDALONE
 *
 *   setup:
 *     disk.begin();
 *
 *   loop:
 *     disk.poll();                              // USB_MSC 需要轮询, STANDALONE 空操作
 *     disk.writeFile("/log.txt", data, len);    // 直接调, 内部自动 acquire/release
 *     disk.readFile("/cfg.json", buf, 512, outLen);
 *
 *   底层块级（高级用户）:
 *     disk.read(addr, buf, len);
 *     disk.write(addr, buf, len);               // read-modify-write 安全
 */

#pragma once

#include <Arduino.h>
#include "W25Q128_drv.h"

// USB.h / USBMSC.h 仅 USB_MSC 模式需要, 这里前置声明
class USB;
class USBMSC;

// ============================================================
// ★ 工作模式（构造时选择）
// ============================================================
enum class W25DiskMode {
    STANDALONE,  // 纯固件独占 (不启动 USB)
    USB_MSC      // U 盘模式 (USB 枚举, 电脑可读写)
};

// ============================================================
// ★ 统一组件
// ============================================================
class W25FlashDisk {
public:
    /**
     * @brief  构造
     * @param pins  4 根 SPI 引脚 (cs, sck, mosi, miso)
     * @param mode  工作模式 (默认 USB_MSC)
     * @param spi   SPI 总线引用 (默认 SPI)
     * @param freq  SPI 频率 (默认 4MHz)
     */
    W25FlashDisk(const W25Q128Pins &pins,
                 W25DiskMode mode = W25DiskMode::USB_MSC,
                 SPIClass &spi = SPI,
                 uint32_t freq = 4000000);

    // 当前模式
    W25DiskMode mode() const { return _mode; }
    bool detected() const { return _flash.detected(); }
    uint32_t capacity() const { return _flash.capacity(); }

    /**
     * @brief  初始化
     *         STANDALONE: 只做 SPI + JEDEC 检测
     *         USB_MSC:    SPI 检测 + USB.begin() + 注册 MSC 回调
     */
    bool begin();

    /**
     * @brief  固件接管 Flash (内部挂载 FATFS)
     *         STANDALONE: 直接 mountFs() 就返回
     *         USB_MSC:    先 mediaPresent(false) 通知电脑卸载 → 等 1.5s → mountFs()
     */
    bool acquire();

    /**
     * @brief  归还 Flash (内部卸载 FATFS)
     *         STANDALONE: unmountFs()
     *         USB_MSC:    unmountFs() → mediaPresent(true) 归还电脑
     */
    void release();

    /**
     * @brief  loop 里轮询 (USB_MSC 需要, STANDALONE 空操作)
     */
    void poll();

    /**
     * @brief  固件长期持有
     *         STANDALONE: 恒为 true
     *         USB_MSC:    电脑「安全弹出」后为 true, release() 不再归还
     */
    bool firmwareOwns() const;

    // ---------- 高层文件操作 (内部自动 acquire/release, 调用方一行搞定) ----------

    /**
     * @brief  读整个文件
     */
    bool readFile(const char *path, char *buf, uint32_t maxLen, uint32_t &outLen);

    /**
     * @brief  写文件
     * @param append true=末尾追加, false=覆盖整个文件
     */
    bool writeFile(const char *path, const char *data, uint32_t len, bool append = false);

    // ---------- 底层块级 (高级用户, 线程安全) ----------

    int32_t read(uint32_t addr, uint8_t *buf, uint32_t len) { return _flash.read(addr, buf, len); }
    int32_t write(uint32_t addr, const uint8_t *buf, uint32_t len) { return _flash.write(addr, buf, len); }
    bool eraseSector(uint32_t addr) { return _flash.eraseSector(addr); }

    // ---------- 直接 FATFS 访问 (已 acquire 窗口内) ----------

    bool mountFs() { return _flash.mountFs(); }
    void unmountFs() { _flash.unmountFs(); }
    bool isFsMounted() const { return _flash.isFsMounted(); }
    const char *fsPath() const { return _flash.fsPath(); }

private:
    // USB MSC 静态回调 → 转发到实例
    static W25FlashDisk *s_instance;
    static int32_t readCb(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
    static int32_t writeCb(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize);
    static bool startStopCb(uint8_t power_condition, bool start, bool load_eject);

    // 内部成员
    W25Q128       _flash;        // 内嵌, 用户不需要知道这个类
    W25DiskMode   _mode;
    SPIClass     &_spi;
    uint32_t      _freq;
    W25Q128Pins   _pins;

    // USB_MSC 专用状态
    bool          _usb_attached = false;   // USB 设备是否已初始化
    volatile bool _ejected      = false;   // 电脑已安全弹出
    volatile bool _remount_req  = false;   // 电脑要求重新挂载
    bool          _started      = false;

    // USBMSC 实例 (仅 USB_MSC 模式下用到, 懒初始化)
    USBMSC       *_msc          = nullptr;
};
