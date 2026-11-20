/**
 * @file     W25Q128.h
 * @brief    W25Q128 (16MB) SPI NOR Flash 驱动组件（块级读写 + FATFS 文件系统）
 *
 * 用法：
 *   W25Q128Pins pins = { .cs = 10, .sck = 12, .mosi = 11, .miso = 13 };
 *   W25Q128 flash(pins);
 *   flash.begin();                    // SPI 初始化 + 芯片检测
 *
 *   // 块级访问（线程安全：内部互斥锁，可直接被 USB MSC 回调 / FATFS 共用）
 *   flash.read(addr, buf, len);
 *   flash.write(addr, buf, len);      // read-modify-write，安全支持子扇区写
 *
 *   // FATFS 文件系统（挂载后用 f_open/f_read 等 FatFs API 访问）
 *   flash.mountFs();                  // 注册 diskio + 挂载
 *   f_open(&f, "0:/test.txt", FA_READ);
 *   flash.unmountFs();
 *
 * 注意：
 *   - 芯片供电必须 3.3V (⚠️ 别接 5V)
 *   - write() 为 read-modify-write 实现，不会破坏同 4KB 扇区内的其他数据
 *   - FATFS diskio 仅支持单实例（首个调用 mountFs() 的对象生效）
 */

#pragma once

#include <Arduino.h>
#include <SPI.h>
#include "ff.h"            // FatFs：FATFS / f_mount / diskio 类型
#include "diskio_impl.h"   // ff_diskio_register / ff_diskio_get_drive

// ============================================================
// ★ W25Q128 引脚配置（初始化时传入）
// ============================================================
struct W25Q128Pins {
    int8_t cs;    // 片选 /CS
    int8_t sck;   // 时钟 CLK
    int8_t mosi;  // 主出从入 SI
    int8_t miso;  // 主入从出 SO
};

// ============================================================
// ★ W25Q128 驱动类
// ============================================================
class W25Q128 {
public:
    static constexpr uint32_t PAGE_SIZE       = 256;   // 页编程大小
    static constexpr uint32_t SECTOR_SIZE     = 4096;  // 擦除扇区大小
    static constexpr uint32_t DISK_SECTOR_SIZE = 512;  // FATFS / USB MSC 逻辑扇区

    // pins: 引脚配置；spi: SPI 总线（默认 SPI）；freq: SPI 频率
    W25Q128(const W25Q128Pins &pins, SPIClass &spi = SPI, uint32_t freq = 4000000);

    // 初始化 SPI 并检测芯片（JEDEC ID 厂商 = Winbond 0xEF）
    bool begin();

    // begin() 是否检测到有效芯片
    bool detected() const { return _ok; }

    // 芯片容量（字节）；未检测到时为 0
    uint32_t capacity() const { return _capacity; }

    // 读取 JEDEC ID (MFR / 类型 / 容量)
    bool readJedecId(uint8_t &mfr, uint8_t &type, uint8_t &cap);

    // ---------- 块级访问（线程安全：内部互斥锁保护） ----------
    // USB MSC 回调 (TinyUSB 任务) 与 FATFS diskio 可并发调用

    // 从指定地址读指定长度，返回读取字节数
    int32_t read(uint32_t addr, uint8_t *buf, uint32_t len);

    // 往指定地址写指定长度（read-modify-write，安全支持子扇区写）
    int32_t write(uint32_t addr, const uint8_t *buf, uint32_t len);

    // 擦除 4KB 扇区（地址内部自动对齐 4KB）
    bool eraseSector(uint32_t addr);

    // ---------- FATFS 文件系统支持 ----------

    // 注册 diskio（首次调用时）并挂载 FATFS；成功后可用 f_open 等 API
    bool mountFs();

    // 卸载 FATFS
    void unmountFs();

    // FATFS 是否挂载中
    bool isFsMounted() const { return _fs_mounted; }

    // FATFS 驱动器路径（如 "0:"，mountFs 成功后有效）
    const char *fsPath() const { return _fs_path; }

    // ---------- 便捷文件操作（mountFs 窗口内使用） ----------
    // path 为卷内绝对路径，如 "/test.txt"

    // 读整个文件到 buf（最多 maxLen 字节），outLen 返回实际读取长度
    // 文件不存在 / 未挂载返回 false
    bool readFile(const char *path, char *buf, uint32_t maxLen, uint32_t &outLen);

    // 写文件（append=false 覆盖整个文件，true 在末尾追加）
    bool writeFile(const char *path, const char *data, uint32_t len, bool append = false);

private:
    // W25Q128 SPI 指令集
    static constexpr uint8_t CMD_WRITE_ENABLE  = 0x06;
    static constexpr uint8_t CMD_WRITE_DISABLE = 0x04;
    static constexpr uint8_t CMD_READ_STATUS   = 0x05;
    static constexpr uint8_t CMD_READ_DATA     = 0x03;
    static constexpr uint8_t CMD_PAGE_PROGRAM  = 0x02;
    static constexpr uint8_t CMD_SECTOR_ERASE  = 0x20;
    static constexpr uint8_t CMD_JEDEC_ID      = 0x9F;

    inline void csLow()  { digitalWrite(_pins.cs, LOW);  }
    inline void csHigh() { digitalWrite(_pins.cs, HIGH); }
    inline void lock()   { xSemaphoreTake(_mutex, portMAX_DELAY); }
    inline void unlock() { xSemaphoreGive(_mutex); }

    void writeEnable();
    uint8_t readStatus();
    void waitBusy();
    int32_t pageProgram(uint32_t addr, const uint8_t *buf, uint32_t len);

    // FATFS diskio 适配（static 函数指针 → 转发到注册实例）
    static W25Q128 *s_diskio_owner;   // 注册 diskio 的实例（单实例）
    bool registerDiskio();            // 获取驱动器号并注册回调
    static DSTATUS diskInit(BYTE pdrv);
    static DSTATUS diskStatus(BYTE pdrv);
    static DRESULT diskRead(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count);
    static DRESULT diskWrite(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count);
    static DRESULT diskIoctl(BYTE pdrv, BYTE cmd, void *buff);

    W25Q128Pins      _pins;
    SPIClass        &_spi;
    uint32_t         _freq;
    bool             _ok = false;
    uint32_t         _capacity = 0;

    SemaphoreHandle_t _mutex = nullptr;      // 块级互斥锁
    FATFS            _fs;                    // FatFs 文件系统对象
    bool             _fs_mounted = false;
    bool             _diskio_registered = false;
    BYTE             _pdrv = 0xFF;           // FatFs 驱动器号
    char             _fs_path[8];            // 如 "0:"
};
