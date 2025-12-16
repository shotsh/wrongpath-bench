/*
 * find_b_accesses.c - Find array B accesses in ChampSim trace (Phase 2)
 *
 * Usage: find_b_accesses --trace PATH --b-base 0x... --b-size N [--max-hits M]
 *
 * Scans a binary trace file and reports all memory accesses that fall
 * within the address range [b_base, b_base + b_size).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <getopt.h>

/*
 * ChampSim trace format (from inc/trace_instruction.h)
 */
#define NUM_INSTR_DESTINATIONS 2
#define NUM_INSTR_SOURCES 4

struct input_instr {
    uint64_t ip;
    uint8_t  is_branch;
    uint8_t  branch_taken;
    uint8_t  destination_registers[NUM_INSTR_DESTINATIONS];
    uint8_t  source_registers[NUM_INSTR_SOURCES];
    uint64_t destination_memory[NUM_INSTR_DESTINATIONS];
    uint64_t source_memory[NUM_INSTR_SOURCES];
};

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s --trace PATH --b-base 0x... --b-size N [--max-hits M]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --trace PATH     Path to raw binary trace file (required)\n");
    fprintf(stderr, "  --b-base ADDR    Base address of array B in hex (required)\n");
    fprintf(stderr, "  --b-size BYTES   Size of array B in bytes (required)\n");
    fprintf(stderr, "  --max-hits N     Maximum number of B accesses to report (default: unlimited)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Output format (CSV):\n");
    fprintf(stderr, "  idx,kind,ip,addr,offset\n");
}

int main(int argc, char *argv[]) {
    const char *trace_path = NULL;
    uint64_t b_base = 0;
    uint64_t b_size = 0;
    uint64_t max_hits = 0;  /* 0 = unlimited */
    int have_b_base = 0;
    int have_b_size = 0;

    /* Parse command line options */
    static struct option long_options[] = {
        {"trace",    required_argument, 0, 't'},
        {"b-base",   required_argument, 0, 'b'},
        {"b-size",   required_argument, 0, 's'},
        {"max-hits", required_argument, 0, 'm'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:b:s:m:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 't':
                trace_path = optarg;
                break;
            case 'b':
                b_base = strtoull(optarg, NULL, 0);
                have_b_base = 1;
                break;
            case 's':
                b_size = strtoull(optarg, NULL, 0);
                have_b_size = 1;
                break;
            case 'm':
                max_hits = strtoull(optarg, NULL, 10);
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }

    if (!trace_path || !have_b_base || !have_b_size) {
        fprintf(stderr, "Error: --trace, --b-base, and --b-size are required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Open trace file */
    FILE *fp = fopen(trace_path, "rb");
    if (!fp) {
        perror("fopen");
        fprintf(stderr, "Error: Cannot open trace file: %s\n", trace_path);
        return 1;
    }

    /* Print header info to stderr */
    fprintf(stderr, "# Trace file: %s\n", trace_path);
    fprintf(stderr, "# B range: [0x%lx, 0x%lx) (%lu bytes)\n",
            (unsigned long)b_base,
            (unsigned long)(b_base + b_size),
            (unsigned long)b_size);
    if (max_hits > 0) {
        fprintf(stderr, "# Max hits: %lu\n", (unsigned long)max_hits);
    }
    fprintf(stderr, "#\n");

    /* Print CSV header to stdout */
    printf("idx,kind,ip,addr,offset\n");

    /* Scan trace */
    struct input_instr rec;
    uint64_t idx = 0;
    uint64_t hit_count = 0;
    uint64_t total_records = 0;

    while (fread(&rec, sizeof(rec), 1, fp) == 1) {
        total_records++;

        /* Check source_memory (loads) */
        for (int i = 0; i < NUM_INSTR_SOURCES; i++) {
            uint64_t addr = rec.source_memory[i];
            if (addr != 0 && addr >= b_base && addr < b_base + b_size) {
                uint64_t offset = addr - b_base;
                printf("%lu,load,0x%lx,0x%lx,0x%lx\n",
                       (unsigned long)idx,
                       (unsigned long)rec.ip,
                       (unsigned long)addr,
                       (unsigned long)offset);
                hit_count++;
                if (max_hits > 0 && hit_count >= max_hits) {
                    goto done;
                }
            }
        }

        /* Check destination_memory (stores) */
        for (int i = 0; i < NUM_INSTR_DESTINATIONS; i++) {
            uint64_t addr = rec.destination_memory[i];
            if (addr != 0 && addr >= b_base && addr < b_base + b_size) {
                uint64_t offset = addr - b_base;
                printf("%lu,store,0x%lx,0x%lx,0x%lx\n",
                       (unsigned long)idx,
                       (unsigned long)rec.ip,
                       (unsigned long)addr,
                       (unsigned long)offset);
                hit_count++;
                if (max_hits > 0 && hit_count >= max_hits) {
                    goto done;
                }
            }
        }

        idx++;
    }

done:
    /* Summary to stderr */
    fprintf(stderr, "#\n");
    fprintf(stderr, "# Scanned %lu records\n", (unsigned long)total_records);
    fprintf(stderr, "# Found %lu B accesses\n", (unsigned long)hit_count);

    fclose(fp);
    return 0;
}
