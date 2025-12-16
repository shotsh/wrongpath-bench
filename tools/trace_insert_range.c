/*
 * trace_insert_range.c - Insert a range of trace records at a specified position (Phase 3.5)
 *
 * Usage: trace_insert_range --in PATH --out PATH --src-begin I --src-end J --insert-at K [--dry-run]
 *
 * Copies records from [src_begin, src_end) and inserts them at position insert_at.
 * Unlike overwrite mode, all original records are preserved and trace length increases.
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
    fprintf(stderr, "Usage: %s --in PATH --out PATH --src-begin I --src-end J --insert-at K [--dry-run]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --in PATH        Input trace file (required)\n");
    fprintf(stderr, "  --out PATH       Output trace file (required, unless --dry-run)\n");
    fprintf(stderr, "  --src-begin I    Source range start index, inclusive (required)\n");
    fprintf(stderr, "  --src-end J      Source range end index, exclusive (required)\n");
    fprintf(stderr, "  --insert-at K    Insertion point - records are inserted BEFORE this index (required)\n");
    fprintf(stderr, "  --dry-run        Validate ranges without writing output\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Behavior:\n");
    fprintf(stderr, "  Inserts records [src_begin, src_end) at position insert_at.\n");
    fprintf(stderr, "  All original records are preserved (insert, not overwrite).\n");
    fprintf(stderr, "  Output trace length = input length + (src_end - src_begin).\n");
}

int main(int argc, char *argv[]) {
    const char *in_path = NULL;
    const char *out_path = NULL;
    int64_t src_begin = -1;
    int64_t src_end = -1;
    int64_t insert_at = -1;
    int dry_run = 0;

    /* Parse command line options */
    static struct option long_options[] = {
        {"in",        required_argument, 0, 'i'},
        {"out",       required_argument, 0, 'o'},
        {"src-begin", required_argument, 0, 's'},
        {"src-end",   required_argument, 0, 'e'},
        {"insert-at", required_argument, 0, 'a'},
        {"dry-run",   no_argument,       0, 'r'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "i:o:s:e:a:rh", long_options, NULL)) != -1) {
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
            case 'a':
                insert_at = strtoll(optarg, NULL, 10);
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
    if (src_begin < 0 || src_end < 0 || insert_at < 0) {
        fprintf(stderr, "Error: --src-begin, --src-end, and --insert-at are required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    /* Validate source range */
    if (src_begin >= src_end) {
        fprintf(stderr, "Error: src_begin (%ld) must be less than src_end (%ld)\n",
                (long)src_begin, (long)src_end);
        return 1;
    }

    int64_t insert_len = src_end - src_begin;

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
    int64_t output_records = total_records + insert_len;

    /* Print operation info */
    fprintf(stderr, "# Input file: %s\n", in_path);
    fprintf(stderr, "# Total input records: %ld\n", (long)total_records);
    fprintf(stderr, "# sizeof(input_instr) = %zu bytes\n", sizeof(struct input_instr));
    fprintf(stderr, "#\n");
    fprintf(stderr, "# Source range: [%ld, %ld) (%ld records)\n",
            (long)src_begin, (long)src_end, (long)insert_len);
    fprintf(stderr, "# Insert at: %ld (records inserted BEFORE this index)\n", (long)insert_at);
    fprintf(stderr, "# Output records: %ld (input + %ld)\n", (long)output_records, (long)insert_len);
    fprintf(stderr, "#\n");

    /* Validate ranges against total records */
    if (src_end > total_records) {
        fprintf(stderr, "Error: src_end (%ld) exceeds total records (%ld)\n",
                (long)src_end, (long)total_records);
        fclose(fp_in);
        return 1;
    }
    if (insert_at > total_records) {
        fprintf(stderr, "Error: insert_at (%ld) exceeds total records (%ld)\n",
                (long)insert_at, (long)total_records);
        fclose(fp_in);
        return 1;
    }

    /* Check for potential issues with overlapping ranges */
    if (src_begin <= insert_at && insert_at < src_end) {
        fprintf(stderr, "Warning: insert_at (%ld) is within source range [%ld, %ld).\n",
                (long)insert_at, (long)src_begin, (long)src_end);
        fprintf(stderr, "         This may produce unexpected results.\n");
        fprintf(stderr, "#\n");
    }

    if (dry_run) {
        fprintf(stderr, "# Dry run: Range validation passed. No output written.\n");
        fprintf(stderr, "#\n");
        fprintf(stderr, "# Output index mapping:\n");
        fprintf(stderr, "#   [0, %ld) -> original [0, %ld)\n", (long)insert_at, (long)insert_at);
        fprintf(stderr, "#   [%ld, %ld) -> inserted from [%ld, %ld)\n",
                (long)insert_at, (long)(insert_at + insert_len),
                (long)src_begin, (long)src_end);
        fprintf(stderr, "#   [%ld, %ld) -> original [%ld, %ld)\n",
                (long)(insert_at + insert_len), (long)output_records,
                (long)insert_at, (long)total_records);
        fclose(fp_in);
        return 0;
    }

    /* Load source records into memory */
    fprintf(stderr, "# Loading source records into memory...\n");
    struct input_instr *src_records = malloc(insert_len * sizeof(struct input_instr));
    if (!src_records) {
        fprintf(stderr, "Error: Cannot allocate memory for %ld source records (%ld bytes)\n",
                (long)insert_len, (long)(insert_len * sizeof(struct input_instr)));
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

    size_t read_count = fread(src_records, sizeof(struct input_instr), insert_len, fp_in);
    if ((int64_t)read_count != insert_len) {
        fprintf(stderr, "Error: Expected to read %ld records, got %zu\n", (long)insert_len, read_count);
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

    /* Process trace: insert mode */
    struct input_instr rec;
    int64_t in_idx = 0;
    int64_t out_idx = 0;
    int inserted = 0;

    while (fread(&rec, sizeof(rec), 1, fp_in) == 1) {
        /* Check if we've reached the insertion point */
        if (!inserted && in_idx == insert_at) {
            /* Insert source records */
            if (fwrite(src_records, sizeof(struct input_instr), insert_len, fp_out) != (size_t)insert_len) {
                perror("fwrite");
                fprintf(stderr, "Error: Write failed during insertion at output index %ld\n", (long)out_idx);
                free(src_records);
                fclose(fp_in);
                fclose(fp_out);
                return 1;
            }
            out_idx += insert_len;
            inserted = 1;
        }

        /* Write original record */
        if (fwrite(&rec, sizeof(struct input_instr), 1, fp_out) != 1) {
            perror("fwrite");
            fprintf(stderr, "Error: Write failed at output index %ld\n", (long)out_idx);
            free(src_records);
            fclose(fp_in);
            fclose(fp_out);
            return 1;
        }
        in_idx++;
        out_idx++;
    }

    /* Handle insertion at the very end (insert_at == total_records) */
    if (!inserted && insert_at == total_records) {
        if (fwrite(src_records, sizeof(struct input_instr), insert_len, fp_out) != (size_t)insert_len) {
            perror("fwrite");
            fprintf(stderr, "Error: Write failed during insertion at end\n");
            free(src_records);
            fclose(fp_in);
            fclose(fp_out);
            return 1;
        }
        out_idx += insert_len;
        inserted = 1;
    }

    fprintf(stderr, "#\n");
    fprintf(stderr, "# Read %ld input records\n", (long)in_idx);
    fprintf(stderr, "# Wrote %ld output records\n", (long)out_idx);
    fprintf(stderr, "# Inserted %ld records at position %ld\n", (long)insert_len, (long)insert_at);
    fprintf(stderr, "# Done.\n");

    free(src_records);
    fclose(fp_in);
    fclose(fp_out);

    return 0;
}
