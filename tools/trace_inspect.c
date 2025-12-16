/*
 * trace_inspect.c - ChampSim binary trace inspector (Phase 1)
 *
 * Usage: trace_inspect [--trace PATH] [--max N]
 *
 * Reads a raw binary trace file and prints human-readable dump of records.
 * Each record corresponds to struct input_instr from ChampSim's trace_instruction.h
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <getopt.h>

/*
 * ChampSim trace format (from inc/trace_instruction.h)
 * This must match the exact binary layout used by the Pin tracer.
 */
#define NUM_INSTR_DESTINATIONS 2
#define NUM_INSTR_SOURCES 4

struct input_instr {
    uint64_t ip;                                      /* instruction pointer */

    uint8_t  is_branch;                               /* branch flag */
    uint8_t  branch_taken;                            /* branch taken/not-taken */

    uint8_t  destination_registers[NUM_INSTR_DESTINATIONS]; /* dest reg IDs */
    uint8_t  source_registers[NUM_INSTR_SOURCES];           /* src reg IDs */

    uint64_t destination_memory[NUM_INSTR_DESTINATIONS];    /* dest mem addresses */
    uint64_t source_memory[NUM_INSTR_SOURCES];              /* src mem addresses */
};

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s --trace PATH [--max N]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --trace PATH   Path to raw binary trace file (required)\n");
    fprintf(stderr, "  --max N        Maximum number of records to display (default: 100)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Output format:\n");
    fprintf(stderr, "  idx=<record#> ip=<hex> src_mem=[...] dst_mem=[...]\n");
}

int main(int argc, char *argv[]) {
    const char *trace_path = NULL;
    uint64_t max_records = 100;

    /* Parse command line options */
    static struct option long_options[] = {
        {"trace", required_argument, 0, 't'},
        {"max",   required_argument, 0, 'm'},
        {"help",  no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "t:m:h", long_options, NULL)) != -1) {
        switch (opt) {
            case 't':
                trace_path = optarg;
                break;
            case 'm':
                max_records = strtoull(optarg, NULL, 10);
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }

    if (!trace_path) {
        fprintf(stderr, "Error: --trace is required\n\n");
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

    /* Print header info */
    printf("# Trace file: %s\n", trace_path);
    printf("# sizeof(input_instr) = %zu bytes\n", sizeof(struct input_instr));
    printf("# Displaying up to %lu records\n", (unsigned long)max_records);
    printf("#\n");

    /* Read and print records */
    struct input_instr rec;
    uint64_t idx = 0;

    while (idx < max_records && fread(&rec, sizeof(rec), 1, fp) == 1) {
        /* Build source memory list (non-zero only) */
        char src_buf[256] = "[";
        int src_first = 1;
        for (int i = 0; i < NUM_INSTR_SOURCES; i++) {
            if (rec.source_memory[i] != 0) {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "%s0x%lx",
                         src_first ? "" : ",",
                         (unsigned long)rec.source_memory[i]);
                strcat(src_buf, tmp);
                src_first = 0;
            }
        }
        strcat(src_buf, "]");

        /* Build destination memory list (non-zero only) */
        char dst_buf[128] = "[";
        int dst_first = 1;
        for (int i = 0; i < NUM_INSTR_DESTINATIONS; i++) {
            if (rec.destination_memory[i] != 0) {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "%s0x%lx",
                         dst_first ? "" : ",",
                         (unsigned long)rec.destination_memory[i]);
                strcat(dst_buf, tmp);
                dst_first = 0;
            }
        }
        strcat(dst_buf, "]");

        /* Print record */
        printf("idx=%lu ip=0x%lx src_mem=%s dst_mem=%s\n",
               (unsigned long)idx,
               (unsigned long)rec.ip,
               src_buf,
               dst_buf);

        idx++;
    }

    /* Summary */
    printf("#\n");
    printf("# Read %lu records\n", (unsigned long)idx);

    /* Check if we hit EOF or max */
    if (feof(fp)) {
        printf("# Reached end of file\n");
    } else if (idx >= max_records) {
        printf("# Stopped at --max limit\n");
    }

    fclose(fp);
    return 0;
}
