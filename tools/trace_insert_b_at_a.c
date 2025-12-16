/*
 * trace_insert_b_at_a.c - Insert B chunk records at a position within A sweep (Phase 3.6)
 *
 * Usage: trace_insert_b_at_a --in PATH --out PATH
 *            --a-begin I --a-end J --b-begin K --b-end L
 *            --a-pos RATIO --b-ratio RATIO [--dry-run]
 *
 * This tool provides a simplified interface for insertion experiments:
 * - a-pos: Where in A to insert (0.0=start, 0.5=middle, 1.0=end)
 * - b-ratio: How much of B chunk to insert (0.5=first half, 1.0=all)
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
    fprintf(stderr, "Usage: %s --in PATH --out PATH \\\n", prog);
    fprintf(stderr, "           --a-begin I --a-end J --b-begin K --b-end L \\\n");
    fprintf(stderr, "           --a-pos RATIO --b-ratio RATIO [--dry-run]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --in PATH        Input trace file (required)\n");
    fprintf(stderr, "  --out PATH       Output trace file (required, unless --dry-run)\n");
    fprintf(stderr, "  --a-begin I      A sweep start index, inclusive (required)\n");
    fprintf(stderr, "  --a-end J        A sweep end index, exclusive (required)\n");
    fprintf(stderr, "  --b-begin K      B chunk start index, inclusive (required)\n");
    fprintf(stderr, "  --b-end L        B chunk end index, exclusive (required)\n");
    fprintf(stderr, "  --a-pos RATIO    Position within A to insert (0.0-1.0, required)\n");
    fprintf(stderr, "                   0.0 = at A start, 0.5 = at A middle, 1.0 = at A end\n");
    fprintf(stderr, "  --b-ratio RATIO  Fraction of B chunk to insert (0.0-1.0, required)\n");
    fprintf(stderr, "                   0.5 = first half of B, 1.0 = all of B\n");
    fprintf(stderr, "  --dry-run        Validate and show calculated values without writing\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Example:\n");
    fprintf(stderr, "  # Insert all of B at the middle of A\n");
    fprintf(stderr, "  %s --in trace.bin --out out.bin \\\n", prog);
    fprintf(stderr, "      --a-begin 322141 --a-end 350820 \\\n");
    fprintf(stderr, "      --b-begin 350820 --b-end 371296 \\\n");
    fprintf(stderr, "      --a-pos 0.5 --b-ratio 1.0\n");
}

int main(int argc, char *argv[]) {
    const char *in_path = NULL;
    const char *out_path = NULL;
    int64_t a_begin = -1;
    int64_t a_end = -1;
    int64_t b_begin = -1;
    int64_t b_end = -1;
    double a_pos = -1.0;
    double b_ratio = -1.0;
    int dry_run = 0;

    /* Parse command line options */
    static struct option long_options[] = {
        {"in",       required_argument, 0, 'i'},
        {"out",      required_argument, 0, 'o'},
        {"a-begin",  required_argument, 0, 'A'},
        {"a-end",    required_argument, 0, 'B'},
        {"b-begin",  required_argument, 0, 'C'},
        {"b-end",    required_argument, 0, 'D'},
        {"a-pos",    required_argument, 0, 'p'},
        {"b-ratio",  required_argument, 0, 'r'},
        {"dry-run",  no_argument,       0, 'd'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "i:o:A:B:C:D:p:r:dh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i':
                in_path = optarg;
                break;
            case 'o':
                out_path = optarg;
                break;
            case 'A':
                a_begin = strtoll(optarg, NULL, 10);
                break;
            case 'B':
                a_end = strtoll(optarg, NULL, 10);
                break;
            case 'C':
                b_begin = strtoll(optarg, NULL, 10);
                break;
            case 'D':
                b_end = strtoll(optarg, NULL, 10);
                break;
            case 'p':
                a_pos = strtod(optarg, NULL);
                break;
            case 'r':
                b_ratio = strtod(optarg, NULL);
                break;
            case 'd':
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
    if (a_begin < 0 || a_end < 0 || b_begin < 0 || b_end < 0) {
        fprintf(stderr, "Error: --a-begin, --a-end, --b-begin, --b-end are required\n\n");
        print_usage(argv[0]);
        return 1;
    }
    if (a_pos < 0.0 || b_ratio < 0.0) {
        fprintf(stderr, "Error: --a-pos and --b-ratio are required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Validate ranges */
    if (a_begin >= a_end) {
        fprintf(stderr, "Error: a_begin (%ld) must be less than a_end (%ld)\n",
                (long)a_begin, (long)a_end);
        return 1;
    }
    if (b_begin >= b_end) {
        fprintf(stderr, "Error: b_begin (%ld) must be less than b_end (%ld)\n",
                (long)b_begin, (long)b_end);
        return 1;
    }

    /* Validate ratios */
    if (a_pos < 0.0 || a_pos > 1.0) {
        fprintf(stderr, "Error: a_pos (%.4f) must be in range [0.0, 1.0]\n", a_pos);
        return 1;
    }
    if (b_ratio <= 0.0 || b_ratio > 1.0) {
        fprintf(stderr, "Error: b_ratio (%.4f) must be in range (0.0, 1.0]\n", b_ratio);
        return 1;
    }

    /* Calculate derived values */
    int64_t a_len = a_end - a_begin;
    int64_t b_len = b_end - b_begin;

    int64_t insert_at = a_begin + (int64_t)(a_len * a_pos);
    int64_t b_insert_len = (int64_t)(b_len * b_ratio);
    if (b_insert_len == 0) {
        b_insert_len = 1;  /* At minimum, insert 1 record */
    }

    int64_t src_begin = b_begin;
    int64_t src_end = b_begin + b_insert_len;

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
    int64_t output_records = total_records + b_insert_len;

    /* Print operation info */
    fprintf(stderr, "# Input file: %s\n", in_path);
    fprintf(stderr, "# Total input records: %ld\n", (long)total_records);
    fprintf(stderr, "# sizeof(input_instr) = %zu bytes\n", sizeof(struct input_instr));
    fprintf(stderr, "#\n");
    fprintf(stderr, "# A range: [%ld, %ld) (%ld records)\n",
            (long)a_begin, (long)a_end, (long)a_len);
    fprintf(stderr, "# B range: [%ld, %ld) (%ld records)\n",
            (long)b_begin, (long)b_end, (long)b_len);
    fprintf(stderr, "#\n");
    fprintf(stderr, "# Parameters:\n");
    fprintf(stderr, "#   a_pos = %.4f (position within A)\n", a_pos);
    fprintf(stderr, "#   b_ratio = %.4f (fraction of B to insert)\n", b_ratio);
    fprintf(stderr, "#\n");
    fprintf(stderr, "# Calculated:\n");
    fprintf(stderr, "#   insert_at = %ld (A[%ld] + %.0f%% of A length)\n",
            (long)insert_at, (long)a_begin, a_pos * 100);
    fprintf(stderr, "#   B insert: [%ld, %ld) (%ld records, %.0f%% of B)\n",
            (long)src_begin, (long)src_end, (long)b_insert_len, b_ratio * 100);
    fprintf(stderr, "#\n");
    fprintf(stderr, "# Output records: %ld + %ld = %ld\n",
            (long)total_records, (long)b_insert_len, (long)output_records);
    fprintf(stderr, "#\n");

    /* Validate ranges against total records */
    if (a_end > total_records) {
        fprintf(stderr, "Error: a_end (%ld) exceeds total records (%ld)\n",
                (long)a_end, (long)total_records);
        fclose(fp_in);
        return 1;
    }
    if (b_end > total_records) {
        fprintf(stderr, "Error: b_end (%ld) exceeds total records (%ld)\n",
                (long)b_end, (long)total_records);
        fclose(fp_in);
        return 1;
    }
    if (insert_at > total_records) {
        fprintf(stderr, "Error: insert_at (%ld) exceeds total records (%ld)\n",
                (long)insert_at, (long)total_records);
        fclose(fp_in);
        return 1;
    }

    /* Warn if insert_at is outside A range (unusual but allowed) */
    if (insert_at < a_begin || insert_at > a_end) {
        fprintf(stderr, "Warning: insert_at (%ld) is outside A range [%ld, %ld)\n",
                (long)insert_at, (long)a_begin, (long)a_end);
        fprintf(stderr, "#\n");
    }

    if (dry_run) {
        fprintf(stderr, "# Dry run: Validation passed. No output written.\n");
        fprintf(stderr, "#\n");
        fprintf(stderr, "# Output index mapping:\n");
        fprintf(stderr, "#   [0, %ld) -> original [0, %ld)\n", (long)insert_at, (long)insert_at);
        fprintf(stderr, "#   [%ld, %ld) -> B records [%ld, %ld)\n",
                (long)insert_at, (long)(insert_at + b_insert_len),
                (long)src_begin, (long)src_end);
        fprintf(stderr, "#   [%ld, %ld) -> original [%ld, %ld)\n",
                (long)(insert_at + b_insert_len), (long)output_records,
                (long)insert_at, (long)total_records);
        fclose(fp_in);
        return 0;
    }

    /* Load B records to insert into memory */
    fprintf(stderr, "# Loading B records into memory...\n");
    struct input_instr *b_records = malloc(b_insert_len * sizeof(struct input_instr));
    if (!b_records) {
        fprintf(stderr, "Error: Cannot allocate memory for %ld B records (%ld bytes)\n",
                (long)b_insert_len, (long)(b_insert_len * sizeof(struct input_instr)));
        fclose(fp_in);
        return 1;
    }

    /* Seek to B range and read */
    if (fseek(fp_in, src_begin * sizeof(struct input_instr), SEEK_SET) != 0) {
        perror("fseek");
        free(b_records);
        fclose(fp_in);
        return 1;
    }

    size_t read_count = fread(b_records, sizeof(struct input_instr), b_insert_len, fp_in);
    if ((int64_t)read_count != b_insert_len) {
        fprintf(stderr, "Error: Expected to read %ld records, got %zu\n", (long)b_insert_len, read_count);
        free(b_records);
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
        free(b_records);
        fclose(fp_in);
        return 1;
    }

    fprintf(stderr, "# Writing output to: %s\n", out_path);

    /* Process trace: insert mode */
    struct input_instr rec;
    int64_t in_idx = 0;
    int64_t out_idx = 0;
    int inserted = 0;

    while (fread(&rec, sizeof(rec), 1, fp_in) == 1) {
        /* Check if we've reached the insertion point */
        if (!inserted && in_idx == insert_at) {
            /* Insert B records */
            if (fwrite(b_records, sizeof(struct input_instr), b_insert_len, fp_out) != (size_t)b_insert_len) {
                perror("fwrite");
                fprintf(stderr, "Error: Write failed during insertion at output index %ld\n", (long)out_idx);
                free(b_records);
                fclose(fp_in);
                fclose(fp_out);
                return 1;
            }
            out_idx += b_insert_len;
            inserted = 1;
        }

        /* Write original record */
        if (fwrite(&rec, sizeof(struct input_instr), 1, fp_out) != 1) {
            perror("fwrite");
            fprintf(stderr, "Error: Write failed at output index %ld\n", (long)out_idx);
            free(b_records);
            fclose(fp_in);
            fclose(fp_out);
            return 1;
        }
        in_idx++;
        out_idx++;
    }

    /* Handle insertion at the very end (insert_at == total_records) */
    if (!inserted && insert_at == total_records) {
        if (fwrite(b_records, sizeof(struct input_instr), b_insert_len, fp_out) != (size_t)b_insert_len) {
            perror("fwrite");
            fprintf(stderr, "Error: Write failed during insertion at end\n");
            free(b_records);
            fclose(fp_in);
            fclose(fp_out);
            return 1;
        }
        out_idx += b_insert_len;
        inserted = 1;
    }

    fprintf(stderr, "#\n");
    fprintf(stderr, "# Read %ld input records\n", (long)in_idx);
    fprintf(stderr, "# Wrote %ld output records\n", (long)out_idx);
    fprintf(stderr, "# Inserted %ld B records at position %ld\n", (long)b_insert_len, (long)insert_at);
    fprintf(stderr, "# Done.\n");

    free(b_records);
    fclose(fp_in);
    fclose(fp_out);

    return 0;
}
