/*
 * Small, deliberately boring PCI BAR access utility for the pcie-vip demo.
 *
 * Usage:
 *   pcie-vip-pcimem BDF read  OFFSET WIDTH
 *   pcie-vip-pcimem BDF write OFFSET WIDTH VALUE
 *
 * WIDTH is 8, 16, 32, or 64 bits.  The utility uses the kernel's sysfs
 * resource file, so it exercises the same BAR mapping a userspace UIO tool
 * would use without relying on /dev/mem or host physical addresses.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(const char *name)
{
    fprintf(stderr,
            "usage: %s BDF read OFFSET WIDTH\n"
            "       %s BDF write OFFSET WIDTH VALUE\n"
            "  BDF examples: 0000:01:00.0 or 01:00.0\n"
            "  WIDTH: 8, 16, 32, or 64 bits\n", name, name);
}

static int parse_u64(const char *text, uint64_t *value)
{
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 0);
    if (errno || !end || *end != '\0') {
        return -1;
    }
    *value = (uint64_t)parsed;
    return 0;
}

static int bar_size(const char *bdf, uint64_t *size)
{
    char path[256];
    FILE *fp;
    unsigned long long start, end, flags;

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource", bdf);
    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    if (fscanf(fp, "%llx %llx %llx", &start, &end, &flags) != 3 || end < start) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    *size = end - start + 1;
    return 0;
}

int main(int argc, char **argv)
{
    char path[256];
    uint64_t offset, width, value = 0, size;
    size_t bytes;
    int fd, prot, flags = MAP_SHARED;
    long page_size;
    uint64_t page_offset;
    void *mapping;
    volatile uint8_t *address;

    if (argc != 5 && argc != 6) {
        usage(argv[0]);
        return 2;
    }
    if (strcmp(argv[2], "read") != 0 && strcmp(argv[2], "write") != 0) {
        usage(argv[0]);
        return 2;
    }
    if (parse_u64(argv[3], &offset) || parse_u64(argv[4], &width) ||
        (argc == 6 && parse_u64(argv[5], &value))) {
        fprintf(stderr, "invalid numeric argument\n");
        return 2;
    }
    if (width != 8 && width != 16 && width != 32 && width != 64) {
        fprintf(stderr, "width must be 8, 16, 32, or 64\n");
        return 2;
    }
    bytes = (size_t)(width / 8);
    if (offset % bytes) {
        fprintf(stderr, "offset 0x%" PRIx64 " is not aligned to %zu bytes\n",
                offset, bytes);
        return 2;
    }
    if (bar_size(argv[1], &size) || offset > size || bytes > size - offset) {
        fprintf(stderr, "BAR0 range check failed for %s offset 0x%" PRIx64 "\n",
                argv[1], offset);
        return 1;
    }
    if (argc == 5 && strcmp(argv[2], "read") != 0) {
        usage(argv[0]);
        return 2;
    }
    if (argc == 6 && strcmp(argv[2], "write") != 0) {
        usage(argv[0]);
        return 2;
    }

    snprintf(path, sizeof(path), "/sys/bus/pci/devices/%s/resource0", argv[1]);
    prot = PROT_READ | (strcmp(argv[2], "write") == 0 ? PROT_WRITE : 0);
    fd = open(path, O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) {
        perror(path);
        return 1;
    }
    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        fprintf(stderr, "cannot determine page size\n");
        close(fd);
        return 1;
    }
    page_offset = offset & ~((uint64_t)page_size - 1);
    mapping = mmap(NULL, (size_t)(page_size + offset - page_offset), prot,
                   flags, fd, (off_t)page_offset);
    if (mapping == MAP_FAILED) {
        perror("mmap resource0");
        close(fd);
        return 1;
    }
    address = (volatile uint8_t *)mapping + (offset - page_offset);

    if (strcmp(argv[2], "read") == 0) {
        /* Use volatile typed loads so the compiler cannot turn a BAR access
         * into an ordinary cached memcpy. PCI MMIO is little-endian on the
         * x86 guest used by this project. */
        switch (bytes) {
        case 1:
            value = *(volatile const uint8_t *)address;
            break;
        case 2:
            value = *(volatile const uint16_t *)address;
            break;
        case 4:
            value = *(volatile const uint32_t *)address;
            break;
        case 8:
            value = *(volatile const uint64_t *)address;
            break;
        }
        printf("0x%0*" PRIx64 "\n", (int)(bytes * 2), value);
    } else {
        memcpy((void *)address, &value, bytes);
    }

    munmap(mapping, (size_t)(page_size + offset - page_offset));
    close(fd);
    return 0;
}
