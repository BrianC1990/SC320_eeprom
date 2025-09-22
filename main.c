/*******************************************************************
 * project: eeprom pro
 * 版本更新记录:
 *   - v1.0.0 (2025-04-27)
 *     - 初始版本，实现基础功能,单路烧录，需配和 v4l2-ctl 使用
 *   - v1.0.1
 *     - 新增版本号，makefile   
 *   - v1.1.0 (2025-04-27)
 *     -  新增4通道烧录
 *     -  
 *******************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <stdint.h>
#include <string.h>

#include "init.h"

// 通过编译器注入的宏
#ifndef VERSION
#define VERSION "unknown"
#endif

#ifndef BUILD_TIME
#define BUILD_TIME __DATE__ " " __TIME__
#endif

#define EE_WRITW   1

#define CHANNEL_N  4

#define SAVE_ON 1
#define SAVE_FILE "read_file.bin"

#define SC320_OP 1
#define SC320_ADDR 0x30

#define I2C_DEV "/dev/i2c-2"    // I2C 设备文件（根据实际情况修改）
#define EEPROM_ADDR 0x50        // P24C64C 的 I2C 地址（默认 0x50）
#define PAGE_SIZE 32            // P24C64C 页写入大小（最大 32 字节）
#define MAX_SIZE 8192           // P24C64C 总容量 8KB (64Kbit)

struct reg_sequence {
    unsigned int reg;
    unsigned int def;
    // unsigned int delay_us;
};

int isProcessRunning(const char *processName) {
    char command[256];
    // 使用ps和grep命令查找进程
    sprintf(command, "ps aux | grep \"%s\" | grep -v grep > /dev/null", processName);
    
    // system返回0表示grep找到了进程(进程存在)
    return system(command) == 0;
}

// 读取二进制文件
int read_bin_file(const char *filename, uint8_t *buf, size_t max_size) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("Failed to open file");
        return -1;
    }
    size_t read_bytes = fread(buf, 1, max_size, file);
    fclose(file);
    return read_bytes;
}

int write_page(int i2c_fd, uint16_t addr, uint8_t *data, size_t len) {
    uint8_t buf[PAGE_SIZE + 2];
    if (len > PAGE_SIZE) len = PAGE_SIZE;
    
    buf[0] = addr >> 8;
    buf[1] = addr & 0xFF;
    memcpy(&buf[2], data, len);
    
    if (write(i2c_fd, buf, len + 2) != len + 2) {
        perror("Write failed");
        return -1;
    }
    usleep(5000); // P24C64写入周期
    return 0;
}

static int sc320_multi_reg_write(int i2c_fd, const struct reg_sequence* regs, int num_regs)
{
    for (int i = 0; i < num_regs; i++) {
        write_page(i2c_fd, regs[i].reg, (uint8_t *)&regs[i].def, 1);
        // printf("reg write : i=%d, 0x%02x, 0x%02x\n", i, regs[i].reg, regs[i].def);
        // if (regs[i].delay_us > 0) {
        //     usleep(regs[i].delay_us);
        // }
        usleep(3000);
    }
    return 0;
}

void erase_full(int i2c_fd) {
    uint8_t blank_page[PAGE_SIZE];
    uint8_t read_page[PAGE_SIZE];

    memset(blank_page, 0xFF, PAGE_SIZE); // 填充0xFF
    printf("Erasing EEPROM...\n");
    for (uint16_t addr = 0; addr < MAX_SIZE; addr += PAGE_SIZE) {
        if (write_page(i2c_fd, addr, blank_page, PAGE_SIZE) != 0) {
            fprintf(stderr, "Erase failed at address 0x%04X\n", addr);
            break;
        }
        // 进度显示
        if (addr % 1024 == 0) printf(".");
    }
    printf("\nErase complete!\n");

    // 校验所有字节是否为0xFF
    for (uint16_t addr = 0; addr < MAX_SIZE; addr += PAGE_SIZE) {
        // 设置读取地址
        uint8_t addr_buf[2] = {addr >> 8, addr & 0xFF};
        if (write(i2c_fd, addr_buf, sizeof(addr_buf)) != sizeof(addr_buf)) {
            perror("Failed to set read address");
            return;
        }
        
        // 读取一页数据
        if (read(i2c_fd, read_page, PAGE_SIZE) != PAGE_SIZE) {
            perror("Failed to read verification data");
            return;
        }
        
        // 检查每个字节
        for (int i = 0; i < PAGE_SIZE; i++) {
            if (read_page[i] != 0xFF) {
                fprintf(stderr, "Verification failed at 0x%04X! Found 0x%02X\n", 
                        addr + i, read_page[i]);
                return;
            }
        }
        
        // 进度显示
        if (addr % 1024 == 0) printf(".");
    }
    printf("\nEEPROM fully erased and verified!\n");
}

// 写入 EEPROM（按页写入）
int write_eeprom(int i2c_fd, uint16_t addr, uint8_t *data, size_t len) {
    uint8_t buf[PAGE_SIZE + 2]; // 2字节地址 + 数据
    size_t remaining = len;
    size_t offset = 0;

    while (remaining > 0) {
        size_t chunk = (remaining > PAGE_SIZE) ? PAGE_SIZE : remaining;
        buf[0] = (addr + offset) >> 8;    // 高字节地址
        buf[1] = (addr + offset) & 0xFF;  // 低字节地址
        memcpy(&buf[2], data + offset, chunk);

        if (write(i2c_fd, buf, chunk + 2) != chunk + 2) {
            perror("Failed to write EEPROM");
            return -1;
        }
        usleep(5000); // P24C64C 写入周期约 5ms
        remaining -= chunk;
        offset += chunk;
    }
    return 0;
}

int read_eeprom(int i2c_fd, uint16_t addr, uint8_t *data, size_t len) {
    uint8_t addr_buf[2];
    size_t remaining = len;
    size_t offset = 0;

    while (remaining > 0) {
        // 1. 设置读取起始地址
        addr_buf[0] = (addr + offset) >> 8;   // 高字节地址
        addr_buf[1] = (addr + offset) & 0xFF; // 低字节地址
        
        if (write(i2c_fd, addr_buf, sizeof(addr_buf)) != sizeof(addr_buf)) {
            perror("Failed to set EEPROM read address");
            return -1;
        }

        // 2. 计算本次读取长度(不超过页边界)
        size_t chunk = (remaining > PAGE_SIZE) ? PAGE_SIZE : remaining;
        
        // 3. 执行读取
        if (read(i2c_fd, data + offset, chunk) != chunk) {
            perror("Failed to read EEPROM data");
            return -1;
        }

        remaining -= chunk;
        offset += chunk;
        
        // 4. 小延迟防止总线冲突(读取通常不需要等待，但保持稳健性)
        usleep(5000);
    }
    return 0;
}

// 读取 EEPROM 数据校验
int verify_eeprom(int i2c_fd, uint16_t addr, uint8_t *data, size_t len) {
    int ret =0;
    uint8_t buf[2];
    uint8_t read_buf[MAX_SIZE];

    ret = read_eeprom(i2c_fd, addr, read_buf, len);
    if (ret)
    {
        perror("Failed to read");
        return -1;
    }

#if SAVE_ON
    mode_t fileMode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;  
    
    // 创建一个新文件并设置权限
    int read_fd = open(SAVE_FILE, O_CREAT | O_WRONLY, fileMode);
    if (read_fd == -1) {
        perror("文件打开失败");
        return -1;
    }

    ret = write(read_fd, read_buf, len);
    if (ret != len)
    {
        perror("write save fiile failed");
    }
    close(read_fd);
#endif

    return memcmp(data, read_buf, len) == 0 ? 0 : -1;
}

int falsh_pro(int fd, uint8_t *data, uint32_t len)
{
    int i2c_fd = fd;
    #if SC320_OP
    if (ioctl(i2c_fd, I2C_SLAVE, SC320_ADDR) < 0) {
        perror("Failed to set SC320 slave address");
        close(i2c_fd);
        return 1;
    }

    sc320_multi_reg_write(i2c_fd, (struct reg_sequence *)SC320AT_CFG, 5);

#endif

    if (ioctl(i2c_fd, I2C_SLAVE, EEPROM_ADDR) < 0) {
        perror("Failed to set I2C slave address");
        close(i2c_fd);
        return 1;
    }

    erase_full(i2c_fd);

#if EE_WRITW
    printf("Writing to EEPROM...\n");
    if (write_eeprom(i2c_fd, 0, data, len) != 0) {
        close(i2c_fd);
        return 1;
    }

    printf("Verifying EEPROM...\n");
    if (verify_eeprom(i2c_fd, 0, data, len) != 0) {
        printf("EEPROM verification failed!\n");
        close(i2c_fd);
        return 1;
    }


    printf("EEPROM write and verify success!\n");
#endif
    return 0;

}

int main(int argc, char *argv[]) {
    int ret;

#if EE_WRITE
    if (argc != 2) {
        printf("Usage: %s <filename.bin>\n", argv[0]);
        return 1;
    }
#endif

    if (argc > 1 && strcmp(argv[1], "--version") == 0) {
        printf("程序版本: %s\n编译时间: %s\n", VERSION, BUILD_TIME);
        return 0;
    }

    system("v4l2-ctl -d /dev/video0 --stream-mmap > /dev/null 2>&1 &");
    sleep(8);

    uint8_t data[MAX_SIZE];
    int file_size = read_bin_file(argv[1], data, MAX_SIZE);
    printf("file: %s , size: %d \n", argv[1], file_size);

    if (file_size <= 0) {
        return 1;
    }

    int i2c_fd = open(I2C_DEV, O_RDWR);
    if (i2c_fd < 0) {
        perror("Failed to open I2C device");
        return 1;
    }

    for (int i = 0; i < CHANNEL_N; i++)
    {
        char cmd[32];
        sprintf(cmd, "v4l2-ctl --set-ctrl=gain=%d\n",i);
        printf("flash chl %d\n", i);
        // printf(cmd);

        system(cmd);

        ret = falsh_pro(i2c_fd, data, file_size);
        if (ret)
        {
            printf("flash chl %d failed\n", i);
            continue;
        }
        printf("flash chl %d success\n\n\n", i);

        sleep(5);

    }

    close(i2c_fd);

    system("killall v4l2-ctl");
    printf("flash done!!!\n");

    return 0;
}