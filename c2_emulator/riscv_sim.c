#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/ip.h>
#include <fcntl.h>

#include "elf.h"
#include "sail.h"
#include "rts.h"
#ifdef SAILCOV
#include "sail_coverage.h"
#endif
#include "riscv_platform.h"
#include "riscv_platform_impl.h"
#include MODEL_H

#ifdef ENABLE_SPIKE
#include "tv_spike_intf.h"
#else
struct tv_spike_t;
#endif

const char *RV64ISA = "RV64IMAC";
const char *RV32ISA = "RV32IMAC";

/* Selected CSRs from riscv-isa-sim/riscv/encoding.h */
#define CSR_STVEC 0x105
#define CSR_SEPC 0x141
#define CSR_SCAUSE 0x142
#define CSR_STVAL 0x143

#define CSR_MSTATUS 0x300
#define CSR_MISA 0x301
#define CSR_MEDELEG 0x302
#define CSR_MIDELEG 0x303
#define CSR_MIE 0x304
#define CSR_MTVEC 0x305
#define CSR_MEPC 0x341
#define CSR_MCAUSE 0x342
#define CSR_MTVAL 0x343
#define CSR_MIP 0x344

static bool do_dump_dts = false;
static bool do_show_times = false;
struct tv_spike_t *s = NULL;
char *term_log = NULL;
char *dtb_file = NULL;
unsigned char *dtb = NULL;
size_t dtb_len = 0;
#ifdef RVFI_DII
static bool rvfi_dii = false;
static int rvfi_dii_port;
static int rvfi_dii_sock;
#endif

unsigned char *spike_dtb = NULL;
size_t spike_dtb_len = 0;

char *sig_file = NULL;
uint64_t mem_sig_start = 0;
uint64_t mem_sig_end = 0;

bool config_print_instr = true;
bool config_print_reg = true;
bool config_print_mem_access = true;
bool config_print_platform = true;

void set_config_print(char *var, bool val) {
  if (var == NULL || strcmp("all", var) == 0) {
    config_print_instr = val;
    config_print_mem_access = val;
    config_print_reg = val;
    config_print_platform = val;
  } else if (strcmp("instr", var) == 0) {
    config_print_instr = val;
  } else if (strcmp("reg", var) == 0) {
    config_print_reg = val;
  } else if (strcmp("mem", var) == 0) {
    config_print_mem_access = val;
  } else if (strcmp("platform", var) == 0) {
    config_print_platform = val;
  } else {
    fprintf(stderr, "Unknown trace category: '%s' (should be instr|reg|mem|platform|all)\n", var);
    exit(1);
  }
}

struct timeval init_start, init_end, run_end;
int total_insns = 0;
int insn_limit = 0;

static struct option options[] = {
  {"enable-dirty-update",         no_argument,       0, 'd'},
  {"enable-misaligned",           no_argument,       0, 'm'},
  {"enable-pmp",                  no_argument,       0, 'P'},
  {"ram-size",                    required_argument, 0, 'z'},
  {"disable-compressed",          no_argument,       0, 'C'},
  {"disable-writable-misa",       no_argument,       0, 'I'},
  {"disable-fdext",               no_argument,       0, 'F'},
  {"mtval-has-illegal-inst-bits", no_argument,       0, 'i'},
  {"device-tree-blob",            required_argument, 0, 'b'},
  {"terminal-log",                required_argument, 0, 't'},
  {"show-times",                  required_argument, 0, 'p'},
  {"report-arch",                 no_argument,       0, 'a'},
  {"test-signature",              required_argument, 0, 'T'},
#ifdef RVFI_DII
  {"rvfi-dii",                    required_argument, 0, 'r'},
#endif
  {"help",                        no_argument,       0, 'h'},
  {"trace",                       optional_argument, 0, 'v'},
  {"no-trace",                    optional_argument, 0, 'V'},
  {"inst-limit",                  required_argument, 0, 'l'},
  {0, 0, 0, 0}
};

static void print_usage(const char *argv0, int ec)
{
#ifdef RVFI_DII
  fprintf(stdout, "Usage: %s [options] <elf_file>\n       %s [options] -r <port>\n", argv0, argv0);
#else
  fprintf(stdout, "Usage: %s [options] <elf_file>\n", argv0);
#endif
  struct option *opt = options;
  while (opt->name) {
    fprintf(stdout, "\t -%c\t --%s\n", (char)opt->val, opt->name);
    opt++;
  }
  exit(ec);
}

static void report_arch(sail_state *state)
{
  fprintf(stdout, "RV%" PRIu64 "\n", state->xlen_val);
  exit(0);
}

static bool is_32bit_model(sail_state *state)
{
  return state->xlen_val == 32;
}

static void dump_dts(sail_state *state)
{
#ifdef ENABLE_SPIKE
  size_t dts_len = 0;
  const char *isa = is_32bit_model(state) ? RV32ISA : RV64ISA;
  struct tv_spike_t *s = tv_init(isa, rv_ram_size, 0);
  tv_get_dts(s, NULL, &dts_len);
  if (dts_len > 0) {
    unsigned char *dts = (unsigned char *)malloc(dts_len + 1);
    dts[dts_len] = '\0';
    tv_get_dts(s, dts, &dts_len);
    fprintf(stdout, "%s\n", dts);
  }
#else
  fprintf(stdout, "Spike linkage is currently needed to generate DTS.\n");
#endif
  exit(0);
}

static void read_dtb(const char *path)
{
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    fprintf(stderr, "Unable to read DTB file %s: %s\n", path, strerror(errno));
    exit(1);
  }
  struct stat st;
  if (fstat(fd, &st) < 0) {
    fprintf(stderr, "Unable to stat DTB file %s: %s\n", path, strerror(errno));
    exit(1);
  }
  char *m = (char *)mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
  if (m == MAP_FAILED) {
    fprintf(stderr, "Unable to map DTB file %s: %s\n", path, strerror(errno));
    exit(1);
  }
  dtb = (unsigned char *)malloc(st.st_size);
  if (dtb == NULL) {
    fprintf(stderr, "Cannot allocate DTB from file %s!\n", path);
    exit(1);
  }
  memcpy(dtb, m, st.st_size);
  dtb_len = st.st_size;
  munmap(m, st.st_size);
  close(fd);

  fprintf(stdout, "Read %" PRIi64 " bytes of DTB from %s.\n", dtb_len, path);
}

char *process_args(sail_state *state, int argc, char **argv)
{
  int c;
  uint64_t ram_size = 0;
  while(true) {
    c = getopt_long(argc, argv,
                    "a"
                    "d"
                    "m"
                    "C"
                    "I"
                    "i"
                    "s"
                    "p"
                    "z:"
                    "b:"
                    "t:"
                    "h"
                    "r:"
                    "T:"
                    "V::"
                    "v::"
                    "l:"
                    "F:"
                         , options, NULL);
    if (c == -1) break;
    switch (c) {
    case 'a':
      report_arch(state);
      break;
    case 'd':
      fprintf(stderr, "enabling dirty update.\n");
      rv_enable_dirty_update = true;
      break;
    case 'm':
      fprintf(stderr, "enabling misaligned access.\n");
      rv_enable_misaligned = true;
      break;
    case 'P':
      fprintf(stderr, "enabling PMP support.\n");
      rv_enable_pmp = true;
      break;
    case 'C':
      fprintf(stderr, "disabling RVC compressed instructions.\n");
      rv_enable_rvc = false;
      break;
    case 'I':
      fprintf(stderr, "disabling writable misa CSR.\n");
      rv_enable_writable_misa = false;
      break;
    case 'F':
      fprintf(stderr, "disabling floating point (F and D extensions).\n");
      rv_enable_fdext = false;
      break;
    case 'i':
      fprintf(stderr, "enabling storing illegal instruction bits in mtval.\n");
      rv_mtval_has_illegal_inst_bits = true;
      break;
    case 's':
      do_dump_dts = true;
      break;
    case 'p':
      fprintf(stderr, "will show execution times on completion.\n");
      do_show_times = true;
      break;
    case 'z':
      ram_size = atol(optarg);
      if (ram_size) {
        fprintf(stderr, "setting ram-size to %" PRIu64 " MB\n", ram_size);
        rv_ram_size = ram_size << 20;
      } else {
        fprintf(stderr, "invalid ram-size '%s' provided.\n", optarg);
        exit(1);
      }
      break;
    case 'b':
      dtb_file = strdup(optarg);
      fprintf(stderr, "using %s as DTB file.\n", dtb_file);
      break;
    case 't':
      term_log = strdup(optarg);
      fprintf(stderr, "using %s for terminal output.\n", term_log);
      break;
    case 'T':
      sig_file = strdup(optarg);
      fprintf(stderr, "using %s for test-signature output.\n", sig_file);
      break;
    case 'h':
      print_usage(argv[0], 0);
      break;
#ifdef RVFI_DII
    case 'r':
      rvfi_dii = true;
      rvfi_dii_port = atoi(optarg);
      fprintf(stderr, "using %d as RVFI port.\n", rvfi_dii_port);
      break;
#endif
    case 'V':
      set_config_print(optarg, false);
      break;
    case 'v':
      set_config_print(optarg, true);
      break;
    case 'l':
      insn_limit = atoi(optarg);
      break;
    case '?':
      print_usage(argv[0], 1);
      break;
    }
  }
  if (do_dump_dts) dump_dts(state);
#ifdef RVFI_DII
  if (optind > argc || (optind == argc && !rvfi_dii)) print_usage(argv[0], 0);
#else
  if (optind >= argc) {
    fprintf(stderr, "No elf file provided.\n");
    print_usage(argv[0], 0);
  }
#endif
  if (dtb_file) read_dtb(dtb_file);

#ifdef RVFI_DII
  if (!rvfi_dii)
#endif
  fprintf(stdout, "Running file %s.\n", argv[optind]);

  return argv[optind];
}

void check_elf(sail_state *state, bool is32bit)
{
  if (is32bit) {
    if (state->xlen_val != 32) {
      fprintf(stderr, "32-bit ELF not supported by RV%" PRIu64 " model.\n", state->xlen_val);
      exit(1);
    }
  } else {
    if (state->xlen_val != 64) {
      fprintf(stderr, "64-bit ELF not supported by RV%" PRIu64 " model.\n", state->xlen_val);
      exit(1);
    }
  }
}

uint64_t load_sail(sail_state *state, char *f)
{
  bool is32bit;
  uint64_t entry;
  uint64_t begin_sig, end_sig;
  load_elf(f, &is32bit, &entry);
  check_elf(state, is32bit);
  fprintf(stdout, "ELF Entry @ 0x%" PRIx64 "\n", entry);
  /* locate htif ports */
  if (lookup_sym(f, "tohost", &rv_htif_tohost) < 0) {
    fprintf(stderr, "Unable to locate htif tohost port.\n");
    exit(1);
  }
  fprintf(stderr, "tohost located at 0x%0" PRIx64 "\n", rv_htif_tohost);
  /* locate test-signature locations if any */
  if (!lookup_sym(f, "begin_signature", &begin_sig)) {
    fprintf(stdout, "begin_signature: 0x%0" PRIx64 "\n", begin_sig);
    mem_sig_start = begin_sig;
  }
  if (!lookup_sym(f, "end_signature", &end_sig)) {
    fprintf(stdout, "end_signature: 0x%0" PRIx64 "\n", end_sig);
    mem_sig_end = end_sig;
  }
  return entry;
}

void init_spike(const char *f, uint64_t entry, uint64_t ram_size)
{
#ifdef ENABLE_SPIKE
  bool mismatch = false;
  const char *isa = is_32bit_model() ? RV32ISA : RV64ISA;
  s = tv_init(isa, ram_size, 1);
  if (tv_is_dirty_enabled(s) != rv_enable_dirty_update) {
    mismatch = true;
    fprintf(stderr, "inconsistent enable-dirty-update setting: spike %s, sail %s\n",
            tv_is_dirty_enabled(s) ? "on" : "off",
            rv_enable_dirty_update ? "on" : "off");
  }
  if (tv_is_misaligned_enabled(s) != rv_enable_misaligned) {
    mismatch = true;
    fprintf(stderr, "inconsistent enable-misaligned-access setting: spike %s, sail %s\n",
            tv_is_misaligned_enabled(s) ? "on" : "off",
            rv_enable_misaligned        ? "on" : "off");
  }
  if (tv_ram_size(s) != rv_ram_size) {
    mismatch = true;
    fprintf(stderr, "inconsistent ram-size setting: spike 0x%" PRIx64 ", sail 0x%" PRIx64 "\n",
            tv_ram_size(s), rv_ram_size);
  }
  if (mismatch) exit(1);

  /* The initialization order below matters. */
  tv_set_verbose(s, 1);
  tv_set_dtb_in_rom(s, 1);
  tv_load_elf(s, f);
  tv_reset(s);

  /* sync the insns per tick */
  rv_insns_per_tick = tv_get_insns_per_tick(s);

  /* get DTB from spike */
  tv_get_dtb(s, NULL, &spike_dtb_len);
  if (spike_dtb_len > 0) {
    spike_dtb = (unsigned char *)malloc(spike_dtb_len + 1);
    spike_dtb[spike_dtb_len] = '\0';
    if (!tv_get_dtb(s, spike_dtb, &spike_dtb_len)) {
      fprintf(stderr, "Got %" PRIu64 " bytes of dtb at %p\n", spike_dtb_len, spike_dtb);
    } else {
      fprintf(stderr, "Error getting DTB from Spike.\n");
      exit(1);
    }
  } else {
    fprintf(stderr, "No DTB available from Spike.\n");
  }
#else
  s = NULL;
#endif
}

void tick_spike()
{
#ifdef ENABLE_SPIKE
  tv_tick_clock(s);
  tv_step_io(s);
#endif
}

void init_sail_reset_vector(sail_state *state, uint64_t entry)
{
#define RST_VEC_SIZE 8
  uint32_t reset_vec[RST_VEC_SIZE] = {
    0x297,                                      // auipc  t0,0x0
    0x28593 + (RST_VEC_SIZE * 4 << 20),         // addi   a1, t0, &dtb
    0xf1402573,                                 // csrr   a0, mhartid
    is_32bit_model(state) ?
      0x0182a283u :                             // lw     t0,24(t0)
      0x0182b283u,                              // ld     t0,24(t0)
    0x28067,                                    // jr     t0
    0,
    (uint32_t) (entry & 0xffffffff),
    (uint32_t) (entry >> 32)
  };

  rv_rom_base = DEFAULT_RSTVEC;
  uint64_t addr = rv_rom_base;
  for (int i = 0; i < sizeof(reset_vec); i++)
    write_mem(addr++, (uint64_t)((char *)reset_vec)[i]);

  if (dtb && dtb_len) {
    for (size_t i = 0; i < dtb_len; i++)
      write_mem(addr++, dtb[i]);
  }

#ifdef ENABLE_SPIKE
  if (dtb && dtb_len) {
    // Ensure that Spike's DTB matches the one provided.
    bool matched = dtb_len == spike_dtb_len;
    if (matched) {
      for (size_t i = 0; i < dtb_len; i++)
        matched = matched && (dtb[i] == spike_dtb[i]);
    }
    if (!matched) {
      fprintf(stderr, "Provided DTB does not match Spike's!\n");
      exit(1);
    }
  } else {
    if (spike_dtb_len > 0) {
      // Use the DTB from Spike.
      for (size_t i = 0; i < spike_dtb_len; i++)
        write_mem(addr++, spike_dtb[i]);
    } else {
      fprintf(stderr, "Running without rom device tree.\n");
    }
  }
#endif

  /* zero-fill to page boundary */
  const int align = 0x1000;
  uint64_t rom_end = (addr + align -1)/align * align;
  for (int i = addr; i < rom_end; i++)
    write_mem(addr++, 0);

  /* set rom size */
  rv_rom_size = rom_end - rv_rom_base;
  /* boot at reset vector */
  state->PC = rv_rom_base;
}

void init_sail(sail_state *state, uint64_t elf_entry)
{
  init_model(state, UNIT);
#ifdef RVFI_DII
  if (rvfi_dii) {
    zext_rvfi_init(UNIT);
    rv_ram_base = UINT64_C(0x80000000);
    rv_ram_size = UINT64_C(0x800000);
    rv_rom_base = UINT64_C(0);
    rv_rom_size = UINT64_C(0);
    rv_clint_base = UINT64_C(0);
    rv_clint_size = UINT64_C(0);
    rv_htif_tohost = UINT64_C(0);
    state->PC = elf_entry;
  } else
#endif
  init_sail_reset_vector(state, elf_entry);

  // this is probably unnecessary now; remove
  if (!rv_enable_rvc) set_misa_c(state, &(state->misa), 0);
}

int init_check(struct tv_spike_t *s)
{
  int passed = 1;
#ifdef ENABLE_SPIKE
  passed &= tv_check_csr(s, CSR_MISA, zmisa.zMisa_chunk_0);
#endif
  return passed;
}

void write_signature(const char *file)
{
  if (mem_sig_start >= mem_sig_end) {
    fprintf(stderr, "Invalid signature region [0x%0" PRIx64 ",0x%0" PRIx64 "] to %s.\n",
            mem_sig_start, mem_sig_end, file);
    return;
  }
  FILE *f = fopen(file, "w");
  if (!f) {
    fprintf(stderr, "Cannot open file '%s': %s\n", file, strerror(errno));
    return;
  }
  /* write out words in signature area */
  for (uint64_t addr = mem_sig_start; addr < mem_sig_end; addr += 4) {
    /* most-significant byte first */
    for (int i = 3; i >= 0; i--) {
      uint8_t byte = (uint8_t) read_mem(addr+i);
      fprintf(f, "%02x", byte);
    }
    fprintf(f, "\n");
  }
  fclose(f);
}

void close_logs(void) {
#ifdef SAILCOV
  if (sail_coverage_exit() != 0) {
    fprintf(stderr, "Could not write coverage information!\n");
    exit(EXIT_FAILURE);
  }
#endif
}

void finish(int ec)
{
  if (sig_file) write_signature(sig_file);

#ifdef ENABLE_SPIKE
  tv_free(s);
#endif
  if (gettimeofday(&run_end, NULL) < 0) {
    fprintf(stderr, "Cannot gettimeofday: %s\n", strerror(errno));
    exit(1);
  }
  if (do_show_times) {
    int init_msecs = (init_end.tv_sec - init_start.tv_sec)*1000 + (init_end.tv_usec - init_start.tv_usec)/1000;
    int exec_msecs = (run_end.tv_sec - init_end.tv_sec)*1000 + (run_end.tv_usec - init_end.tv_usec)/1000;
    double Kips    = ((double)total_insns)/((double)exec_msecs);
    fprintf(stderr, "Initialization:   %d msecs\n", init_msecs);
    fprintf(stderr, "Execution:        %d msecs\n", exec_msecs);
    fprintf(stderr, "Instructions:     %d\n", total_insns);
    fprintf(stderr, "Perf:             %.3f Kips\n", Kips);
  }
  close_logs();
  exit(ec);
}

int compare_states(struct tv_spike_t *s)
{
  int passed = 1;

#ifdef ENABLE_SPIKE
#define TV_CHECK(reg, spike_reg, sail_reg)   \
  passed &= tv_check_ ## reg(s, spike_reg, sail_reg);

  // fix default C enum map for cur_privilege
  uint8_t priv = (zcur_privilege == 2) ? 3 : zcur_privilege;
  passed &= tv_check_priv(s, priv);

  passed &= tv_check_pc(s, zPC);

  TV_CHECK(gpr, 1, zx1);
  TV_CHECK(gpr, 2, zx2);
  TV_CHECK(gpr, 3, zx3);
  TV_CHECK(gpr, 4, zx4);
  TV_CHECK(gpr, 5, zx5);
  TV_CHECK(gpr, 6, zx6);
  TV_CHECK(gpr, 7, zx7);
  TV_CHECK(gpr, 8, zx8);
  TV_CHECK(gpr, 9, zx9);
  TV_CHECK(gpr, 10, zx10);
  TV_CHECK(gpr, 11, zx11);
  TV_CHECK(gpr, 12, zx12);
  TV_CHECK(gpr, 13, zx13);
  TV_CHECK(gpr, 14, zx14);
  TV_CHECK(gpr, 15, zx15);
  TV_CHECK(gpr, 15, zx15);
  TV_CHECK(gpr, 16, zx16);
  TV_CHECK(gpr, 17, zx17);
  TV_CHECK(gpr, 18, zx18);
  TV_CHECK(gpr, 19, zx19);
  TV_CHECK(gpr, 20, zx20);
  TV_CHECK(gpr, 21, zx21);
  TV_CHECK(gpr, 22, zx22);
  TV_CHECK(gpr, 23, zx23);
  TV_CHECK(gpr, 24, zx24);
  TV_CHECK(gpr, 25, zx25);
  TV_CHECK(gpr, 25, zx25);
  TV_CHECK(gpr, 26, zx26);
  TV_CHECK(gpr, 27, zx27);
  TV_CHECK(gpr, 28, zx28);
  TV_CHECK(gpr, 29, zx29);
  TV_CHECK(gpr, 30, zx30);
  TV_CHECK(gpr, 31, zx31);

  /* some selected CSRs for now */

  TV_CHECK(csr, CSR_MCAUSE, zmcause.zMcause_chunk_0);
  TV_CHECK(csr, CSR_MEPC, zmepc);
  TV_CHECK(csr, CSR_MTVAL, zmtval);
  TV_CHECK(csr, CSR_MSTATUS, zmstatus);

  TV_CHECK(csr, CSR_SCAUSE, zscause.zMcause_chunk_0);
  TV_CHECK(csr, CSR_SEPC, zsepc);
  TV_CHECK(csr, CSR_STVAL, zstval);

#undef TV_CHECK
#endif

  return passed;
}

void flush_logs(void)
{
  if(config_print_instr) {
    fprintf(stderr, "\n");
    fflush(stderr);
    fprintf(stdout, "\n");
    fflush(stdout);
  }
}

#ifdef RVFI_DII
void rvfi_send_trace(void) {
  sail_bits packet;
  CREATE(lbits)(&packet);
  zrvfi_get_exec_packet(&packet, UNIT);
  if (packet.len % 8 != 0) {
    fprintf(stderr, "RVFI-DII trace packet not byte aligned: %d\n", (int)packet.len);
    exit(1);
  }
  unsigned char bytes[packet.len / 8];
  /* mpz_export might not write all of the null bytes */
  memset(bytes, 0, sizeof(bytes));
  mpz_export(bytes, NULL, -1, 1, 0, 0, *(packet.bits));
  if (write(rvfi_dii_sock, bytes, packet.len / 8) == -1) {
    fprintf(stderr, "Writing RVFI DII trace failed: %s", strerror(errno));
    exit(1);
  }
  KILL(lbits)(&packet);
}
#endif

void run_sail(uint64_t entry)
{
  sail_state state;
  sail_initialize_state(&state);
  initialize_registers(&state, UNIT);
  init_sail(&state, entry);
  
  bool spike_done;
  bool stepped;
  bool diverged = false;

  /* initialize the step number */
  mach_int step_no = 0;
  int insn_cnt = 0;
#ifdef RVFI_DII
  bool need_instr = true;
#endif

  struct timeval interval_start;
  if (gettimeofday(&interval_start, NULL) < 0) {
    fprintf(stderr, "Cannot gettimeofday: %s\n", strerror(errno));
    exit(1);
  }

  while (!state.htif_done && (insn_limit == 0 || total_insns < insn_limit)) {
#ifdef RVFI_DII
    if (rvfi_dii) {
      mach_bits instr_bits;
      int res = read(rvfi_dii_sock, &instr_bits, sizeof(instr_bits));
      if (res == 0) {
        rvfi_dii = false;
        return;
      }
      if (res < sizeof(instr_bits)) {
        fprintf(stderr, "Reading RVFI DII command failed: insufficient input");
        exit(1);
      }
      if (res == -1) {
        fprintf(stderr, "Reading RVFI DII command failed: %s", strerror(errno));
        exit(1);
      }
      zrvfi_set_instr_packet(instr_bits);
      zrvfi_zzero_exec_packet(UNIT);
      mach_bits cmd = zrvfi_get_cmd(UNIT);
      switch (cmd) {
      case 0: /* EndOfTrace */
        zrvfi_halt_exec_packet(UNIT);
        rvfi_send_trace();
        return;
      case 1: /* Instruction */
        break;
      default:
        fprintf(stderr, "Unknown RVFI-DII command: %d\n", (int)cmd);
        exit(1);
      }
      sail_int sail_step;
      CREATE(sail_int)(&sail_step);
      CONVERT_OF(sail_int, mach_int)(&sail_step, step_no);
      stepped = step(&state, sail_step);
      if (state.have_exception) goto step_exception;
      flush_logs();
      KILL(sail_int)(&sail_step);
      rvfi_send_trace();
    } else /* if (!rvfi_dii) */
#endif
    { /* run a Sail step */
      sail_int sail_step;
      CREATE(sail_int)(&sail_step);
      CONVERT_OF(sail_int, mach_int)(&sail_step, step_no);
      stepped = step(&state, sail_step);
      if (state.have_exception) goto step_exception;
      flush_logs();
      KILL(sail_int)(&sail_step);
    }
    if (stepped) {
      step_no++;
      insn_cnt++;
      total_insns++;
    }

    if (do_show_times && (total_insns & 0xfffff) == 0) {
      uint64_t start_us = 1000000 * ((uint64_t) interval_start.tv_sec)  + ((uint64_t)interval_start.tv_usec);
      if (gettimeofday(&interval_start, NULL) < 0) {
        fprintf(stderr, "Cannot gettimeofday: %s\n", strerror(errno));
         exit(1);
      }
      uint64_t end_us = 1000000 * ((uint64_t) interval_start.tv_sec)  + ((uint64_t)interval_start.tv_usec);
      fprintf(stdout, "kips: %" PRIu64 "\n", ((uint64_t)1000) * 0x100000 / (end_us - start_us));
    }
#ifdef ENABLE_SPIKE
    { /* run a Spike step */
      tv_step(s);
      spike_done = tv_is_done(s);
      flush_logs();
    }

    if (state.htif_done) {
      if (!spike_done) {
        fprintf(stdout, "Sail done (exit-code %" PRIi64 "), but not Spike!\n", state.htif_exit_code);
        exit(1);
      }
    } else {
      if (spike_done) {
        fprintf(stdout, "Spike done, but not Sail!\n");
        exit(1);
      }
    }
    if (!compare_states(s)) {
      diverged = true;
      break;
    }
#endif
    if (state.htif_done) {
      /* check exit code */
      if (state.htif_exit_code == 0)
        fprintf(stdout, "SUCCESS\n");
      else
        fprintf(stdout, "FAILURE: %" PRIi64 "\n", state.htif_exit_code);
    }

    if (insn_cnt == rv_insns_per_tick) {
      insn_cnt = 0;
      tick_clock(&state, UNIT);
      tick_platform(&state, UNIT);

      tick_spike();
    }
  }

dump_state:
  if (diverged) {
    /* TODO */
  }
  sail_finalize_state(&state);
  finish(diverged);

step_exception:
  fprintf(stderr, "Sail exception!");
  goto dump_state;
}

void init_logs()
{
#ifdef ENABLE_SPIKE
  // The Spike interface uses stdout for terminal output, and stderr for logs.
  // Do the same here.
  if (dup2(1, 2) < 0) {
    fprintf(stderr, "Unable to dup 1 -> 2: %s\n", strerror(errno));
    exit(1);
  }
#endif

  if (term_log != NULL && (term_fd = open(term_log, O_WRONLY|O_CREAT|O_TRUNC, S_IRUSR|S_IRGRP|S_IROTH|S_IWUSR)) < 0) {
    fprintf(stderr, "Cannot create terminal log '%s': %s\n", term_log, strerror(errno));
    exit(1);
  }
}

int main(int argc, char **argv)
{
  // Initialize model state so that we can check or report its architecture.
  sail_state state;
  sail_initialize_state(&state);
  
  char *file = process_args(&state, argc, argv);
  init_logs();

  if (gettimeofday(&init_start, NULL) < 0) {
    fprintf(stderr, "Cannot gettimeofday: %s\n", strerror(errno));
    exit(1);
  }

#ifdef RVFI_DII
  uint64_t entry;
  if (rvfi_dii) {
    entry = 0x80000000;
    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == -1) {
      fprintf(stderr, "Unable to create socket: %s", strerror(errno));
      return 1;
    }
    int opt = 1;
    if (setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1) {
      fprintf(stderr, "Unable to set reuseaddr on socket: %s", strerror(errno));
      return 1;
    }
    struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
      .sin_port = htons(rvfi_dii_port)
    };
    if (bind(listen_sock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
      fprintf(stderr, "Unable to set bind socket: %s", strerror(errno));
      return 1;
    }
    if (listen(listen_sock, 1) == -1) {
      fprintf(stderr, "Unable to listen on socket: %s", strerror(errno));
      return 1;
    }
    printf("Waiting for connection\n");
    rvfi_dii_sock = accept(listen_sock, NULL, NULL);
    if (rvfi_dii_sock == -1) {
      fprintf(stderr, "Unable to accept connection on socket: %s", strerror(errno));
      return 1;
    }
    close(listen_sock);
    printf("Connected\n");
  } else
    entry = load_sail(&state, file);
#else
  uint64_t entry = load_sail(&state, file);
#endif
 
  sail_finalize_state(&state);
  
  /* initialize spike before sail so that we can access the device-tree blob,
   * until we roll our own.
   */
  init_spike(file, entry, rv_ram_size);
  /*
  init_sail(entry);

  if (!init_check(s)) finish(1);
  */

  setup_rts();

  if (gettimeofday(&init_end, NULL) < 0) {
    fprintf(stderr, "Cannot gettimeofday: %s\n", strerror(errno));
    exit(1);
  }

  do {
    run_sail(entry);
#ifndef RVFI_DII
  } while (0);
#else
    if (rvfi_dii) {
      /* Reset for next test */
      reinit_sail(entry);
    }
  } while (rvfi_dii);
#endif
  flush_logs();
  close_logs();
}