/**
 * @file     W25Q128.cpp
 * @brief    W25Q128 (16MB) SPI NOR Flash 驱动实现（块级读写 + FATFS 支持）
 */

#include "W25Q128_drv.h"

// diskio 绑定的实例（单实例）
W25Q128 *W25Q128::s_diskio_owner = nullptr;

W25Q128::W25Q128(const W25Q128Pins &pins, SPIClass &spi, uint32_t freq)
    : _pins(pins), _spi(spi), _freq(freq)
{
}

bool W25Q128::begin()
{
    // 1. 块级互斥锁
    if (!_mutex) _mutex = xSemaphoreCreateMutex();

    // 2. 片选引脚（SPI 库不代管 CS）
    pinMode(_pins.cs, OUTPUT);
    csHigh();

    // 3. SPI 总线
    _spi.begin(_pins.sck, _pins.miso, _pins.mosi, -1);
    _spi.beginTransaction(SPISettings(_freq, MSBFIRST, SPI_MODE0));

    // 4. 检测芯片（Winbond 厂商 ID = 0xEF）
    uint8_t mfr, type, cap;
    _ok = readJedecId(mfr, type, cap);
    if (_ok) {
        _capacity = (1UL << cap);  // cap=0x18 → 2^24 = 16MB
    }
    return _ok;
}

bool W25Q128::readJedecId(uint8_t &mfr, uint8_t &type, uint8_t &cap)
{
    csLow();
    _spi.transfer(CMD_JEDEC_ID);
    mfr  = _spi.transfer(0);
    type = _spi.transfer(0);
    cap  = _spi.transfer(0);
    csHigh();
    return (mfr == 0xEF);  // Winbond
}

void W25Q128::writeEnable()
{
    csLow();
    _spi.transfer(CMD_WRITE_ENABLE);
    csHigh();
}

uint8_t W25Q128::readStatus()
{
    csLow();
    _spi.transfer(CMD_READ_STATUS);
    uint8_t s = _spi.transfer(0);
    csHigh();
    return s;
}

void W25Q128::waitBusy()
{
    while (readStatus() & 0x01) { /* busy */ }
}

// ============================ 块级访问 ============================
// 线程安全：USB MSC 回调与 FATFS diskio 并发调用时由互斥锁串行化

int32_t W25Q128::read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    lock();
    csLow();
    _spi.transfer(CMD_READ_DATA);
    _spi.transfer((addr >> 16) & 0xFF);
    _spi.transfer((addr >> 8)  & 0xFF);
    _spi.transfer( addr        & 0xFF);
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = _spi.transfer(0);
    }
    csHigh();
    unlock();
    return len;
}

bool W25Q128::eraseSector(uint32_t addr)
{
    addr &= ~(SECTOR_SIZE - 1);  // 对齐 4KB
    writeEnable();
    csLow();
    _spi.transfer(CMD_SECTOR_ERASE);
    _spi.transfer((addr >> 16) & 0xFF);
    _spi.transfer((addr >> 8)  & 0xFF);
    _spi.transfer( addr        & 0xFF);
    csHigh();
    waitBusy();
    return true;
}

int32_t W25Q128::pageProgram(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (len > PAGE_SIZE) len = PAGE_SIZE;
    writeEnable();
    csLow();
    _spi.transfer(CMD_PAGE_PROGRAM);
    _spi.transfer((addr >> 16) & 0xFF);
    _spi.transfer((addr >> 8)  & 0xFF);
    _spi.transfer( addr        & 0xFF);
    for (uint32_t i = 0; i < len; i++) {
        _spi.transfer(buf[i]);
    }
    csHigh();
    waitBusy();
    return len;
}

// read-modify-write 实现：
// NOR Flash 擦除以 4KB 扇区为单位，擦掉后只能整块重写。
// 若写入不足 4KB（如 FAT 表/目录项的 512B 更新），必须先读出整个
// 扇区 → 内存中合并新数据 → 擦除 → 整块写回，否则同扇区内其余数据会丢失。
int32_t W25Q128::write(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    static uint8_t secbuf[SECTOR_SIZE];  // 4KB 合并缓冲（互斥锁保证串行使用）
    uint32_t done = 0;

    lock();
    while (done < len) {
        uint32_t sec   = (addr + done) & ~(SECTOR_SIZE - 1);  // 扇区起始
        uint32_t off   = (addr + done) & (SECTOR_SIZE - 1);   // 扇区内偏移
        uint32_t chunk = SECTOR_SIZE - off;
        if (chunk > len - done) chunk = len - done;

        // 1. 读出整扇区现有内容
        csLow();
        _spi.transfer(CMD_READ_DATA);
        _spi.transfer((sec >> 16) & 0xFF);
        _spi.transfer((sec >> 8)  & 0xFF);
        _spi.transfer( sec        & 0xFF);
        for (uint32_t i = 0; i < SECTOR_SIZE; i++) {
            secbuf[i] = _spi.transfer(0);
        }
        csHigh();

        // 2. 内存中合并新数据
        memcpy(secbuf + off, buf + done, chunk);

        // 3. 擦除
        eraseSector(sec);

        // 4. 整块写回
        for (uint32_t p = 0; p < SECTOR_SIZE; p += PAGE_SIZE) {
            pageProgram(sec + p, secbuf + p, PAGE_SIZE);
        }
        done += chunk;
    }
    unlock();
    return len;
}

// ============================ FATFS 支持 ============================

// 首次调用时向 FatFs 注册本芯片的 diskio 回调
bool W25Q128::registerDiskio()
{
    if (_diskio_registered) return true;

    if (ff_diskio_get_drive(&_pdrv) != ESP_OK) return false;  // 无空闲驱动器号

    ff_diskio_impl_t impl;
    impl.init   = diskInit;
    impl.status = diskStatus;
    impl.read   = diskRead;
    impl.write  = diskWrite;
    impl.ioctl  = diskIoctl;
    ff_diskio_register(_pdrv, &impl);

    s_diskio_owner = this;
    snprintf(_fs_path, sizeof(_fs_path), "%d:", _pdrv);
    _diskio_registered = true;
    return true;
}

// 挂载 FATFS（立即挂载模式 opt=1，介质必须已包含有效 FAT 卷）
bool W25Q128::mountFs()
{
    if (_fs_mounted) return true;
    if (!registerDiskio()) return false;

    if (f_mount(&_fs, _fs_path, 1) != FR_OK) return false;  // 未格式化/读失败
    _fs_mounted = true;
    return true;
}

void W25Q128::unmountFs()
{
    if (!_fs_mounted) return;
    f_mount(NULL, _fs_path, 0);
    _fs_mounted = false;
}

// ---------- 便捷文件操作 ----------

bool W25Q128::readFile(const char *path, char *buf, uint32_t maxLen, uint32_t &outLen)
{
    outLen = 0;
    if (!_fs_mounted) return false;

    char full[64];
    snprintf(full, sizeof(full), "%s%s", _fs_path, path);

    FIL f;
    if (f_open(&f, full, FA_READ) != FR_OK) return false;

    UINT br = 0;
    f_read(&f, buf, maxLen, &br);
    f_close(&f);

    outLen = br;
    return true;
}

bool W25Q128::writeFile(const char *path, const char *data, uint32_t len, bool append)
{
    if (!_fs_mounted) return false;

    char full[64];
    snprintf(full, sizeof(full), "%s%s", _fs_path, path);

    FIL f;
    BYTE mode = FA_WRITE | (append ? FA_OPEN_APPEND : FA_CREATE_ALWAYS);
    if (f_open(&f, full, mode) != FR_OK) return false;

    UINT bw = 0;
    FRESULT res = f_write(&f, data, len, &bw);
    f_close(&f);

    return (res == FR_OK && bw == len);
}

// ---------- FATFS diskio 回调（FatFs 任务上下文 → 转发到实例，互斥锁保护） ----------

DSTATUS W25Q128::diskInit(BYTE pdrv)
{
    (void)pdrv;
    return s_diskio_owner && s_diskio_owner->detected() ? 0 : STA_NOINIT;
}

DSTATUS W25Q128::diskStatus(BYTE pdrv)
{
    (void)pdrv;
    return s_diskio_owner && s_diskio_owner->detected() ? 0 : STA_NOINIT;
}

DRESULT W25Q128::diskRead(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (!s_diskio_owner) return RES_NOTRDY;
    s_diskio_owner->read((uint32_t)sector * DISK_SECTOR_SIZE,
                         buff, (uint32_t)count * DISK_SECTOR_SIZE);
    return RES_OK;
}

DRESULT W25Q128::diskWrite(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    (void)pdrv;
    if (!s_diskio_owner) return RES_NOTRDY;
    s_diskio_owner->write((uint32_t)sector * DISK_SECTOR_SIZE,
                          const_cast<uint8_t *>(buff), (uint32_t)count * DISK_SECTOR_SIZE);
    return RES_OK;
}

DRESULT W25Q128::diskIoctl(BYTE pdrv, BYTE cmd, void *buff)
{
    (void)pdrv;
    W25Q128 *d = s_diskio_owner;
    if (!d) return RES_NOTRDY;

    switch (cmd) {
    case CTRL_SYNC:          // 无缓存，写直达 Flash
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = (LBA_t)(d->_capacity / DISK_SECTOR_SIZE);
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buff = (WORD)DISK_SECTOR_SIZE;
        return RES_OK;
    case GET_BLOCK_SIZE:     // 擦除块 4KB = 8 逻辑扇区
        *(DWORD *)buff = SECTOR_SIZE / DISK_SECTOR_SIZE;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}
