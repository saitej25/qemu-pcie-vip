// SPDX-License-Identifier: MIT
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "pcie_vip_uapi.h"

static void usage(const char *name)
{
    printf("Usage: %s [options]\n\n"
           "  -l, --length BYTES       payload length (1..4096, default 64)\n"
           "  -n, --iterations COUNT   repeat count (1..1024, default 1)\n"
           "  -i, --interactive        prompt for length and repeat count\n"
           "  -v, --verbose            print timing, throughput, and completions\n"
           "  -h, --help               show this help\n", name);
}

static int read_u32(const char *prompt, unsigned int *value,
                    unsigned int min, unsigned int max)
{
    unsigned long parsed;
    char line[64];
    char *end;

    printf("%s [%u..%u]: ", prompt, min, max);
    fflush(stdout);
    if (!fgets(line, sizeof(line), stdin))
        return -1;
    errno = 0;
    parsed = strtoul(line, &end, 0);
    if (errno || end == line || parsed < min || parsed > max)
        return -1;
    *value = (unsigned int)parsed;
    return 0;
}

int main(int argc, char **argv)
{
    unsigned int length = 64, iterations = 1;
    int interactive = 0, verbose = 0, opt;
    int fd, ret;
    struct pcie_vip_run_config cfg;
    struct timespec wall_start, wall_end;
    static const struct option options[] = {
        {"length", required_argument, NULL, 'l'},
        {"iterations", required_argument, NULL, 'n'},
        {"interactive", no_argument, NULL, 'i'},
        {"verbose", no_argument, NULL, 'v'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0}
    };

    while ((opt = getopt_long(argc, argv, "l:n:ivh", options, NULL)) != -1) {
        char *end;
        unsigned long parsed;

        switch (opt) {
        case 'l':
            errno = 0;
            parsed = strtoul(optarg, &end, 0);
            if (errno || end == optarg || *end || parsed < 1 || parsed > 4096) {
                fprintf(stderr, "invalid length: %s\n", optarg);
                return 2;
            }
            length = (unsigned int)parsed;
            verbose = 1;
            break;
        case 'n':
            errno = 0;
            parsed = strtoul(optarg, &end, 0);
            if (errno || end == optarg || *end || parsed < 1 || parsed > 1024) {
                fprintf(stderr, "invalid iterations: %s\n", optarg);
                return 2;
            }
            iterations = (unsigned int)parsed;
            verbose = 1;
            break;
        case 'i': interactive = 1; verbose = 1; break;
        case 'v': verbose = 1; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 2;
        }
    }

    if (interactive) {
        if (read_u32("Transfer length", &length, 1, 4096) ||
            read_u32("Iterations", &iterations, 1, 1024)) {
            fprintf(stderr, "invalid interactive value\n");
            return 2;
        }
    }

    fd = open("/dev/pcie_vip0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        perror("open /dev/pcie_vip0");
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &wall_start);
    if (!verbose && length == 64 && iterations == 1) {
        ret = ioctl(fd, PCIE_VIP_IOC_RUN);
    } else {
        cfg.length = length;
        cfg.iterations = iterations;
        cfg.completed = cfg.last_status = 0;
        cfg.elapsed_ns = 0;
        ret = ioctl(fd, PCIE_VIP_IOC_RUN_CONFIG, &cfg);
    }
    clock_gettime(CLOCK_MONOTONIC, &wall_end);
    close(fd);

    if (ret < 0) {
        perror("PCIE_VIP_DMA");
        return 1;
    }
    if (!verbose) {
        puts("PCIE-VIP DMA/MSI-X PASS");
        return 0;
    }

    {
        uint64_t wall_ns = (uint64_t)(wall_end.tv_sec - wall_start.tv_sec) * 1000000000ULL +
                           (uint64_t)wall_end.tv_nsec - wall_start.tv_nsec;
        uint64_t elapsed_ns = cfg.elapsed_ns ? cfg.elapsed_ns : wall_ns;
        double seconds = (double)elapsed_ns / 1e9;
        double mib_s = seconds > 0.0 ?
            (double)(2ULL * length * iterations) / (1024.0 * 1024.0 * seconds) : 0.0;

        printf("PCIE-VIP DMA/MSI-X PASS\n"
               "  transfer length : %u bytes\n"
               "  iterations      : %u\n"
               "  completions     : %u\n"
               "  elapsed         : %" PRIu64 " ns (%.3f ms)\n"
               "  round-trip data : %" PRIu64 " bytes\n"
               "  throughput      : %.3f MiB/s\n",
               length, iterations, cfg.completed, elapsed_ns,
               (double)elapsed_ns / 1e6,
               (uint64_t)2 * length * iterations, mib_s);
    }
    return 0;
}
