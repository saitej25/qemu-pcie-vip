// SPDX-License-Identifier: MIT
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define PCIE_VIP_IOC_RUN _IO('V', 0)

int main(void)
{
    int fd = open("/dev/pcie_vip0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/pcie_vip0");
        return 1;
    }
    if (ioctl(fd, PCIE_VIP_IOC_RUN) < 0) {
        perror("PCIE_VIP_IOC_RUN");
        close(fd);
        return 1;
    }
    close(fd);
    puts("PCIE-VIP DMA/MSI-X PASS");
    return 0;
}
