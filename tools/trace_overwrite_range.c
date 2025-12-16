/*
 * trace_overwrite_range.c - Overwrite a range of trace records (Phase 3)
 *
 * Usage: trace_overwrite_range --in PATH --out PATH --src-begin I --src-end J --dst-begin K [--dry-run]
 *
 * Copies records from [src_begin, src_end) to [dst_begin, dst_begin + (src_end - src_begin))
 * The total trace length remains unchanged (overwrite, not insert).
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
    fprintf(stderr, "Usage: %s --in PATH --out PATH --src-begin I --src-end J --dst-begin K [--dry-run]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --in PATH        Input trace file (required)\n");
    fprintf(stderr, "  --out PATH       Output trace file (required, unless --dry-run)\n");
    fprintf(stderr, "  --src-begin I    Source range start index, inclusive (required)\n");
    fprintf(stderr, "  --src-end J      Source range end index, exclusive (required)\n");
    fprintf(stderr, "  --dst-begin K    Destination start index (required)\n");
    fprintf(stderr, "  --dry-run        Validate ranges without writing output\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Behavior:\n");
    fprintf(stderr, "  Copies records [src_begin, src_end) to [dst_begin, dst_begin + len)\n");
    fprintf(stderr, "  where len = src_end - src_begin.\n");
    fprintf(stderr, "  Total trace length is unchanged (overwrite mode).\n");
}

int main(int argc, char *argv[]) {
    const char *in_path = NULL;
    const char *out_path = NULL;
    int64_t src_begin = -1;
    int64_t src_end = -1;
    int64_t dst_begin = -1;
    int dry_run = 0;

    /* Parse command line options */
    static struct option long_options[] = {
        {"in",        required_argument, 0, 'i'},
        {"out",       required_argument, 0, 'o'},
        {"src-begin", required_argument, 0, 's'},
        {"src-end",   required_argument, 0, 'e'},
        {"dst-begin", required_argument, 0, 'd'},
        {"dry-run",   no_argument,       0, 'r'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "i:o:s:e:d:rh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i':
                in_path = optarg;
                break;
            case 'o':
                out_path = optarg;
                break;
            case 's':
                src_begin = strtoll(optarg, NULL, 10);
                break;
            case 'e':
                src_end = strtoll(optarg, NULL, 10);
                break;
            case 'd':
                dst_begin = strtoll(optarg, NULL, 10);
                break;
            case 'r':
                dry_run = 1;
                break;
            case 'h':
            default:
                print_usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }

    /* Validate required arguments */
    if (!in_path) {
        fprintf(stderr, "Error: --in is required\n\n");
        print_usage(argv[0]);
        return 1;
    }
    if (!out_path && !dry_run) {
        fprintf(stderr, "Error: --out is required (or use --dry-run)\n\n");
        print_usage(argv[0]);
        return 1;
    }
    if (src_begin < 0 || src_end < 0 || dst_begin < 0) {
        fprintf(stderr, "Error: --src-begin, --src-end, and --dst-begin are required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Validate source range */
    if (src_begin >= src_end) {
        fprintf(stderr, "Error: src_begin (%ld) must be less than src_end (%ld)\n",
                (long)src_begin, (long)src_end);
        return 1;
    }

    int64_t copy_len = src_end - src_begin;

    /* Open input file and get total records */
    FILE *fp_in = fopen(in_path, "rb");
    if (!fp_in) {
        perror("fopen");
        fprintf(stderr, "Error: Cannot open input file: %s\n", in_path);
        return 1;
    }

    fseek(fp_in, 0, SEEK_END);
    long filesize = ftell(fp_in);
    fseek(fp_in, 0, SEEK_SET);

    if (filesize < 0) {
        perror("ftell");
        fclose(fp_in);
        return 1;
    }

    if (filesize % sizeof(struct input_instr) != 0) {
        fprintf(stderr, "Error: File size (%ld bytes) is not a multiple of sizeof(input_instr) (%zu bytes)\n",
                filesize, sizeof(struct input_instr));
        fclose(fp_in);
        return 1;
    }

    int64_t total_records = filesize / sizeof(struct input_instr);

    /* Print operation info */
    fprintf(stderr, "# Input file: %s\n", in_path);
    fprintf(stderr, "# Total records: %ld\n", (long)total_records);
    fprintf(stderr, "# sizeof(input_instr) = %zu bytes\n", sizeof(struct input_instr));
    fprintf(stderr, "#\n");
    fprintf(stderr, "# Source range: [%ld, %ld) (%ld records)\n",
            (long)src_begin, (long)src_end, (long)copy_len);
    fprintf(stderr, "# Destination range: [%ld, %ld)\n",
            (long)dst_begin, (long)(dst_begin + copy_len));
    fprintf(stderr, "#\n");

    /* Validate ranges against total records */
    if (src_end > total_records) {
        fprintf(stderr, "Error: src_end (%ld) exceeds total records (%ld)\n",
                (long)src_end, (long)total_records);
        fclose(fp_in);
        return 1;
    }
    if (dst_begin + copy_len > total_records) {
        fprintf(stderr, "Error: dst range [%ld, %ld) exceeds total records (%ld)\n",
                (long)dst_begin, (long)(dst_begin + copy_len), (long)total_records);
        fclose(fp_in);
        return 1;
    }

    /* Check for overlapping ranges (would require special handling) */
    int64_t dst_end = dst_begin + copy_len;
    if ((src_begin < dst_end && src_end > dst_begin)) {
        fprintf(stderr, "Warning: Source and destination ranges overlap.\n");
        fprintf(stderr, "         This is supported but may produce unexpected results.\n");
        fprintf(stderr, "#\n");
    }

    if (dry_run) {
        fprintf(stderr, "# Dry run: Range validation passed. No output written.\n");
        fclose(fp_in);
        return 0;
    }

    /* Load source records into memory */
    fprintf(stderr, "# Loading source records into memory...\n");
    struct input_instr *src_records = malloc(copy_len * sizeof(struct input_instr));
    if (!src_records) {
        fprintf(stderr, "Error: Cannot allocate memory for %ld source records\n", (long)copy_len);
        fclose(fp_in);
        return 1;
    }

    /* Seek to source range and read */
    if (fseek(fp_in, src_begin * sizeof(struct input_instr), SEEK_SET) != 0) {
        perror("fseek");
        free(src_records);
        fclose(fp_in);
        return 1;
    }

    size_t read_count = fread(src_records, sizeof(struct input_instr), copy_len, fp_in);
    if ((int64_t)read_count != copy_len) {
        fprintf(stderr, "Error: Expected to read %ld records, got %zu\n", (long)copy_len, read_count);
        free(src_records);
        fclose(fp_in);
        return 1;
    }

    /* Reset input file to beginning */
    fseek(fp_in, 0, SEEK_SET);

    /* Open output file */
    FILE *fp_out = fopen(out_path, "wb");
    if (!fp_out) {
        perror("fopen");
        fprintf(stderr, "Error: Cannot create output file: %s\n", out_path);
        free(src_records);
        fclose(fp_in);
        return 1;
    }

    fprintf(stderr, "# Writing output to: %s\n", out_path);

    /* Process trace: copy with overwrite */
    struct input_instr rec;
    int64_t idx = 0;
    int64_t src_idx = 0;  /* Index into src_records array */

    while (fread(&rec, sizeof(rec), 1, fp_in) == 1) {
        if (idx >= dst_begin && idx < dst_begin + copy_len) {
            /* In destination range: output from source records */
            if (fwrite(&src_records[src_idx], sizeof(struct input_instr), 1, fp_out) != 1) {
                perror("fwrite");
                fprintf(stderr, "Error: Write failed at record %ld\n", (long)idx);
                free(src_records);
                fclose(fp_in);
                fclose(fp_out);
                return 1;
            }
            src_idx++;
        } else {
            /* Outside destination range: output original record */
            if (fwrite(&rec, sizeof(struct input_instr), 1, fp_out) != 1) {
                perror("fwrite");
                fprintf(stderr, "Error: Write failed at record %ld\n", (long)idx);
                free(src_records);
                fclose(fp_in);
                fclose(fp_out);
                return 1;
            }
        }
        idx++;
    }

    fprintf(stderr, "#\n");
    fprintf(stderr, "# Wrote %ld records\n", (long)idx);
    fprintf(stderr, "# Overwritten %ld records at [%ld, %ld)\n",
            (long)copy_len, (long)dst_begin, (long)(dst_begin + copy_len));
    fprintf(stderr, "# Done.\n");

    free(src_records);
    fclose(fp_in);
    fclose(fp_out);

    return 0;
}
