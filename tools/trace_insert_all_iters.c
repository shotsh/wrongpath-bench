/*
 * trace_insert_all_iters.c - Insert B chunks at A positions for all iterations (Phase 4)
 *
 * Usage: trace_insert_all_iters --in PATH --out PATH
 *            --first-a-begin IDX --a-len N --b-len N --iterations N
 *            --a-pos RATIO --b-ratio RATIO [--every N] [--dry-run]
 *
 * Applies the same insertion (a_pos, b_ratio) to all outer iterations.
 * Each iteration's B chunk is inserted at its corresponding A position.
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
    fprintf(stderr, "           --first-a-begin IDX --a-len N --b-len N --iterations N \\\n");
    fprintf(stderr, "           --a-pos RATIO --b-ratio RATIO [--every N] [--dry-run]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --in PATH           Input trace file (required)\n");
    fprintf(stderr, "  --out PATH          Output trace file (required, unless --dry-run)\n");
    fprintf(stderr, "  --first-a-begin IDX First A sweep start index (required)\n");
    fprintf(stderr, "  --a-len N           Length of each A sweep in records (required)\n");
    fprintf(stderr, "  --b-len N           Length of each B chunk in records (required)\n");
    fprintf(stderr, "  --iterations N      Total number of outer iterations (required)\n");
    fprintf(stderr, "  --a-pos RATIO       Position within A to insert (0.0-1.0, required)\n");
    fprintf(stderr, "  --b-ratio RATIO     Fraction of B chunk to insert (0.0-1.0, required)\n");
    fprintf(stderr, "  --every N           Insert every Nth iteration (default: 1 = all)\n");
    fprintf(stderr, "                      0 = no insertions (validation only)\n");
    fprintf(stderr, "  --dry-run           Validate and show plan without writing\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Example:\n");
    fprintf(stderr, "  # Insert B at A midpoint, every 8th iteration\n");
    fprintf(stderr, "  %s --in trace.bin --out out.bin \\\n", prog);
    fprintf(stderr, "      --first-a-begin 322141 --a-len 28679 --b-len 20476 \\\n");
    fprintf(stderr, "      --iterations 4096 --a-pos 0.5 --b-ratio 1.0 --every 8\n");
}

int main(int argc, char *argv[]) {
    const char *in_path = NULL;
    const char *out_path = NULL;
    int64_t first_a_begin = -1;
    int64_t a_len = -1;
    int64_t b_len = -1;
    int64_t iterations = -1;
    double a_pos = -1.0;
    double b_ratio = -1.0;
    int64_t every = 1;  /* Default: insert every iteration */
    int dry_run = 0;

    /* Parse command line options */
    static struct option long_options[] = {
        {"in",             required_argument, 0, 'i'},
        {"out",            required_argument, 0, 'o'},
        {"first-a-begin",  required_argument, 0, 'f'},
        {"a-len",          required_argument, 0, 'a'},
        {"b-len",          required_argument, 0, 'b'},
        {"iterations",     required_argument, 0, 'n'},
        {"a-pos",          required_argument, 0, 'p'},
        {"b-ratio",        required_argument, 0, 'r'},
        {"every",          required_argument, 0, 'e'},
        {"dry-run",        no_argument,       0, 'd'},
        {"help",           no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "i:o:f:a:b:n:p:r:e:dh", long_options, NULL)) != -1) {
        switch (opt) {
            case 'i':
                in_path = optarg;
                break;
            case 'o':
                out_path = optarg;
                break;
            case 'f':
                first_a_begin = strtoll(optarg, NULL, 10);
                break;
            case 'a':
                a_len = strtoll(optarg, NULL, 10);
                break;
            case 'b':
                b_len = strtoll(optarg, NULL, 10);
                break;
            case 'n':
                iterations = strtoll(optarg, NULL, 10);
                break;
            case 'p':
                a_pos = strtod(optarg, NULL);
                break;
            case 'r':
                b_ratio = strtod(optarg, NULL);
                break;
            case 'e':
                every = strtoll(optarg, NULL, 10);
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
    if (first_a_begin < 0 || a_len <= 0 || b_len <= 0 || iterations <= 0) {
        fprintf(stderr, "Error: --first-a-begin, --a-len, --b-len, --iterations are required\n\n");
        print_usage(argv[0]);
        return 1;
    }
    if (a_pos < 0.0 || b_ratio < 0.0) {
        fprintf(stderr, "Error: --a-pos and --b-ratio are required\n\n");
        print_usage(argv[0]);
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
    if (every < 0) {
        fprintf(stderr, "Error: --every must be >= 0\n");
        return 1;
    }

    /* Calculate derived values */
    int64_t iter_len = a_len + b_len;
    int64_t b_insert_len = (int64_t)(b_len * b_ratio);
    if (b_insert_len == 0) {
        b_insert_len = 1;  /* At minimum, insert 1 record */
    }
    int64_t a_offset = (int64_t)(a_len * a_pos);

    /* Count active iterations */
    int64_t active_iters = 0;
    if (every > 0) {
        for (int64_t i = 0; i < iterations; i++) {
            if (i % every == 0) {
                active_iters++;
            }
        }
    }

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
    int64_t total_insert = active_iters * b_insert_len;
    int64_t output_records = total_records + total_insert;

    /* Print operation info */
    fprintf(stderr, "# Input file: %s\n", in_path);
    fprintf(stderr, "# Total input records: %ld\n", (long)total_records);
    fprintf(stderr, "# sizeof(input_instr) = %zu bytes\n", sizeof(struct input_instr));
    fprintf(stderr, "#\n");
    fprintf(stderr, "# Structure:\n");
    fprintf(stderr, "#   first_a_begin = %ld\n", (long)first_a_begin);
    fprintf(stderr, "#   a_len = %ld, b_len = %ld\n", (long)a_len, (long)b_len);
    fprintf(stderr, "#   iter_len = %ld\n", (long)iter_len);
    fprintf(stderr, "#   iterations = %ld\n", (long)iterations);
    fprintf(stderr, "#\n");
    fprintf(stderr, "# Parameters:\n");
    fprintf(stderr, "#   a_pos = %.4f, b_ratio = %.4f\n", a_pos, b_ratio);
    fprintf(stderr, "#   every = %ld\n", (long)every);
    fprintf(stderr, "#\n");
    fprintf(stderr, "# Per-iteration insert: %ld records at A+%ld\n",
            (long)b_insert_len, (long)a_offset);
    fprintf(stderr, "# Active iterations: %ld (every %ldth of %ld)\n",
            (long)active_iters, (long)every, (long)iterations);
    fprintf(stderr, "# Total insertions: %ld x %ld = %ld records\n",
            (long)active_iters, (long)b_insert_len, (long)total_insert);
    fprintf(stderr, "# Output records: %ld + %ld = %ld\n",
            (long)total_records, (long)total_insert, (long)output_records);
    fprintf(stderr, "#\n");

    /* Validate structure against total records */
    int64_t last_iter_end = first_a_begin + iterations * iter_len;
    if (last_iter_end > total_records) {
        fprintf(stderr, "Error: Structure exceeds trace bounds\n");
        fprintf(stderr, "       last_iter_end = %ld, total_records = %ld\n",
                (long)last_iter_end, (long)total_records);
        fclose(fp_in);
        return 1;
    }

    if (dry_run) {
        fprintf(stderr, "# Dry run: Validation passed. No output written.\n");
        fprintf(stderr, "#\n");
        fprintf(stderr, "# First 5 insertion points (input indices):\n");
        int count = 0;
        for (int64_t i = 0; i < iterations && count < 5; i++) {
            if (every > 0 && i % every == 0) {
                int64_t a_begin_i = first_a_begin + i * iter_len;
                int64_t insert_at_i = a_begin_i + a_offset;
                int64_t b_begin_i = a_begin_i + a_len;
                fprintf(stderr, "#   iter %ld: insert_at=%ld, B src=[%ld, %ld)\n",
                        (long)i, (long)insert_at_i, (long)b_begin_i, (long)(b_begin_i + b_insert_len));
                count++;
            }
        }
        if (active_iters > 5) {
            fprintf(stderr, "#   ... (%ld more)\n", (long)(active_iters - 5));
        }
        fclose(fp_in);
        return 0;
    }

    /* Allocate buffer for B insert records */
    struct input_instr *b_buf = malloc(b_insert_len * sizeof(struct input_instr));
    if (!b_buf) {
        fprintf(stderr, "Error: Cannot allocate memory for %ld B records (%ld bytes)\n",
                (long)b_insert_len, (long)(b_insert_len * sizeof(struct input_instr)));
        fclose(fp_in);
        return 1;
    }

    /* Open output file */
    FILE *fp_out = fopen(out_path, "wb");
    if (!fp_out) {
        perror("fopen");
        fprintf(stderr, "Error: Cannot create output file: %s\n", out_path);
        free(b_buf);
        fclose(fp_in);
        return 1;
    }

    fprintf(stderr, "# Writing output to: %s\n", out_path);

    /* Build list of insertion points (input indices) */
    /* We process in order, so we just need to track next insertion */
    int64_t next_iter = 0;  /* Next iteration to check for insertion */
    int64_t next_insert_at = -1;  /* Next insertion point (input index) */
    int64_t next_b_begin = -1;  /* B chunk start for next insertion */
    int64_t insertions_done = 0;

    /* Find first insertion point */
    while (next_iter < iterations) {
        if (every > 0 && next_iter % every == 0) {
            int64_t a_begin_i = first_a_begin + next_iter * iter_len;
            next_insert_at = a_begin_i + a_offset;
            next_b_begin = a_begin_i + a_len;
            break;
        }
        next_iter++;
    }

    /* Process trace */
    struct input_instr rec;
    int64_t in_idx = 0;
    int64_t out_idx = 0;
    long current_pos = 0;  /* Track file position for seeking */

    while (fread(&rec, sizeof(rec), 1, fp_in) == 1) {
        current_pos = ftell(fp_in);

        /* Check if we've reached an insertion point */
        if (next_insert_at >= 0 && in_idx == next_insert_at) {
            /* Save current position */
            long saved_pos = current_pos;

            /* Seek to B chunk and read */
            if (fseek(fp_in, next_b_begin * sizeof(struct input_instr), SEEK_SET) != 0) {
                perror("fseek");
                fprintf(stderr, "Error: Cannot seek to B chunk at idx %ld\n", (long)next_b_begin);
                free(b_buf);
                fclose(fp_in);
                fclose(fp_out);
                return 1;
            }

            size_t read_count = fread(b_buf, sizeof(struct input_instr), b_insert_len, fp_in);
            if ((int64_t)read_count != b_insert_len) {
                fprintf(stderr, "Error: Expected to read %ld B records, got %zu\n",
                        (long)b_insert_len, read_count);
                free(b_buf);
                fclose(fp_in);
                fclose(fp_out);
                return 1;
            }

            /* Write inserted B records */
            if (fwrite(b_buf, sizeof(struct input_instr), b_insert_len, fp_out) != (size_t)b_insert_len) {
                perror("fwrite");
                fprintf(stderr, "Error: Write failed during insertion at output index %ld\n", (long)out_idx);
                free(b_buf);
                fclose(fp_in);
                fclose(fp_out);
                return 1;
            }
            out_idx += b_insert_len;
            insertions_done++;

            /* Seek back to continue reading */
            if (fseek(fp_in, saved_pos, SEEK_SET) != 0) {
                perror("fseek");
                fprintf(stderr, "Error: Cannot seek back to position %ld\n", saved_pos);
                free(b_buf);
                fclose(fp_in);
                fclose(fp_out);
                return 1;
            }

            /* Find next insertion point */
            next_iter++;
            next_insert_at = -1;
            while (next_iter < iterations) {
                if (every > 0 && next_iter % every == 0) {
                    int64_t a_begin_i = first_a_begin + next_iter * iter_len;
                    next_insert_at = a_begin_i + a_offset;
                    next_b_begin = a_begin_i + a_len;
                    break;
                }
                next_iter++;
            }
        }

        /* Write original record */
        if (fwrite(&rec, sizeof(struct input_instr), 1, fp_out) != 1) {
            perror("fwrite");
            fprintf(stderr, "Error: Write failed at output index %ld\n", (long)out_idx);
            free(b_buf);
            fclose(fp_in);
            fclose(fp_out);
            return 1;
        }
        in_idx++;
        out_idx++;

        /* Progress indicator for large traces */
        if (in_idx % 50000000 == 0) {
            fprintf(stderr, "#   Processed %ld M records, %ld insertions...\n",
                    (long)(in_idx / 1000000), (long)insertions_done);
        }
    }

    fprintf(stderr, "#\n");
    fprintf(stderr, "# Read %ld input records\n", (long)in_idx);
    fprintf(stderr, "# Wrote %ld output records\n", (long)out_idx);
    fprintf(stderr, "# Performed %ld insertions\n", (long)insertions_done);
    fprintf(stderr, "# Done.\n");

    free(b_buf);
    fclose(fp_in);
    fclose(fp_out);

    return 0;
}
