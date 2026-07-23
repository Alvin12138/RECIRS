#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <elf.h>
#include <link.h>
#include <dlfcn.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include "openplc_got.h"

/* ============================================================================
 * Robust GOT/PLT Parser - Two methods:
 * 1. Section headers (preferred, gives PLT addresses)
 * 2. Program headers + dynamic segment (fallback, works on stripped binaries)
 * ============================================================================ */

typedef struct {
    uint8_t *base;
    size_t size;
    Elf64_Ehdr *ehdr;
    Elf64_Shdr *shdrs;
    char *shstrtab;
    int shnum;
} elf_image_t;

static int read_self_exe(elf_image_t *elf) {
    int fd = open("/proc/self/exe", O_RDONLY);
    if (fd < 0) return -1;

    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);

    elf->base = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (elf->base == MAP_FAILED) return -1;

    elf->size = size;
    elf->ehdr = (Elf64_Ehdr *)elf->base;
    elf->shdrs = NULL;
    elf->shstrtab = NULL;
    elf->shnum = 0;

    /* Check ELF magic number */
    if (elf->ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        elf->ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        elf->ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        elf->ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        fprintf(stderr, "[GOT] Not a valid ELF file\n");
        return -1;
    }

    /* Read section headers (if exist) */
    if (elf->ehdr->e_shoff != 0 && elf->ehdr->e_shnum > 0) {
        elf->shdrs = (Elf64_Shdr *)(elf->base + elf->ehdr->e_shoff);
        elf->shnum = elf->ehdr->e_shnum;

        if (elf->ehdr->e_shstrndx < elf->shnum) {
            elf->shstrtab = (char *)(elf->base +
                elf->shdrs[elf->ehdr->e_shstrndx].sh_offset);
        }
    }

    return 0;
}

static void free_elf(elf_image_t *elf) {
    if (elf->base && elf->base != MAP_FAILED) {
        munmap(elf->base, elf->size);
        elf->base = NULL;
    }
}

static Elf64_Shdr *find_section(elf_image_t *elf, const char *name) {
    if (!elf->shstrtab || !elf->shdrs) return NULL;
    for (int i = 0; i < elf->shnum; i++) {
        const char *secname = elf->shstrtab + elf->shdrs[i].sh_name;
        if (strcmp(secname, name) == 0) {
            return &elf->shdrs[i];
        }
    }
    return NULL;
}

/* from /proc/self/maps Find the loading base address */
static uint64_t get_load_base(void) {
    FILE *fp = fopen("/proc/self/maps", "r");
    if (!fp) return 0;

    char line[512];
    uint64_t base = 0;
    while (fgets(line, sizeof(line), fp)) {
        /* Find the r-xp mapping of the current executable file (first one) */
        if (strstr(line, "r-xp") && !strstr(line, "[")) {
            /* exclude [vdso], [vsyscall], etc. kernel mapping */
            if (sscanf(line, "%lx", &base) == 1) {
                fclose(fp);
                return base;
            }
        }
    }
    fclose(fp);
    return base;
}

/* ============================================================================
 * Method 1: Parse via section headers (preferred, gives PLT addresses)
 * ============================================================================ */
static int got_parse_sections(elf_image_t *elf, uint64_t load_base,
                               got_entry_t **entries, int *count) {
    Elf64_Shdr *plt_sec = find_section(elf, ".plt");
    Elf64_Shdr *got_plt_sec = find_section(elf, ".got.plt");
    Elf64_Shdr *rela_plt_sec = find_section(elf, ".rela.plt");

    if (!plt_sec || !got_plt_sec || !rela_plt_sec) {
        /* try .rel.plt (REL vs RELA) */
        rela_plt_sec = find_section(elf, ".rel.plt");
        if (!plt_sec || !got_plt_sec || !rela_plt_sec) {
            return -1;  /* rollback to Method 2 */
        }
    }

    int use_rela = (rela_plt_sec->sh_entsize == sizeof(Elf64_Rela));
    int nentries = rela_plt_sec->sh_size /
                   (use_rela ? sizeof(Elf64_Rela) : sizeof(Elf64_Rel));
    if (nentries <= 0) return -1;

    *entries = calloc(nentries, sizeof(got_entry_t));
    if (!*entries) return -1;

    uint64_t *got_table = (uint64_t *)(load_base + got_plt_sec->sh_addr);

    for (int i = 0; i < nentries; i++) {
        (*entries)[i].plt_addr = load_base + plt_sec->sh_addr + i * 16;
        (*entries)[i].got_addr = (uint64_t)&got_table[i];
        (*entries)[i].got_target = got_table[i];
    }

    *count = nentries;
    printf("[GOT] Parsed via section headers: %d entries\n", nentries);
    return 0;
}

/* ============================================================================
 * Method 2: Parse via program headers + dynamic segment
 * (fallback for stripped binaries without section headers)
 * ============================================================================ */

typedef struct {
    uint64_t load_base;
    uint64_t pltgot;
    uint64_t jmprel;
    uint64_t pltrelsz;
    uint64_t symtab;
    uint64_t strtab;
    uint64_t gnu_pltgot;
    int use_rela;
} dyn_info_t;

/* dl_iterate_phdr callback to find the executable's PT_DYNAMIC */
static int find_dynamic_callback(struct dl_phdr_info *info, size_t size, void *data) {
    (void)size;
    dyn_info_t *dyn = (dyn_info_t *)data;

    /* Skip shared libraries that are not currently executable files */
    if (info->dlpi_name && info->dlpi_name[0] != '\0') {
        return 0;  /* continue */
    }

    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            Elf64_Dyn *dynamic = (Elf64_Dyn *)(info->dlpi_addr +
                                                info->dlpi_phdr[i].p_vaddr);
            for (int j = 0; dynamic[j].d_tag != DT_NULL; j++) {
                switch (dynamic[j].d_tag) {
                    case DT_PLTGOT:
                        dyn->pltgot = dynamic[j].d_un.d_ptr;
                        break;
                    case DT_JMPREL:
                        dyn->jmprel = dynamic[j].d_un.d_ptr;
                        break;
                    case DT_PLTRELSZ:
                        dyn->pltrelsz = dynamic[j].d_un.d_val;
                        break;
                    case DT_SYMTAB:
                        dyn->symtab = dynamic[j].d_un.d_ptr;
                        break;
                    case DT_STRTAB:
                        dyn->strtab = dynamic[j].d_un.d_ptr;
                        break;
                    case DT_PLTREL:
                        dyn->use_rela = (dynamic[j].d_un.d_val == DT_RELA);
                        break;
                    case DT_GNU_HASH:
                    case DT_HASH:
                        break;
                }
            }
            dyn->load_base = info->dlpi_addr;
            return 1;  /* found, stop iteration */
        }
    }
    return 0;  /* continue */
}

static int got_parse_dynamic(got_entry_t **entries, int *count) {
    dyn_info_t dyn = {0};
    dyn.use_rela = 1;  /* AArch64 default RELA */

    int ret = dl_iterate_phdr(find_dynamic_callback, &dyn);
    if (ret == 0 || dyn.pltgot == 0 || dyn.jmprel == 0) {
        fprintf(stderr, "[GOT] PT_DYNAMIC not found\n");
        return -1;
    }

    size_t rel_size = dyn.use_rela ? sizeof(Elf64_Rela) : sizeof(Elf64_Rel);
    int nentries = dyn.pltrelsz / rel_size;
    if (nentries <= 0) {
        fprintf(stderr, "[GOT] No PLT entries in dynamic segment\n");
        return -1;
    }

    *entries = calloc(nentries, sizeof(got_entry_t));
    if (!*entries) return -1;

    /* GOT table starts from pltgot+3 (the first 3 items are reserved) */
    uint64_t *got_table = (uint64_t *)dyn.pltgot;

    for (int i = 0; i < nentries; i++) {
        uint64_t got_addr = (uint64_t)&got_table[i + 3];
        /* GOT item index: got[3], got[4], ...
         * plt_addr=0 indicate unknown; use got_addr as only key
         * TEE uses plt_addr as the deduplication key, so it is set to gotaddr here */
        (*entries)[i].plt_addr = got_addr;   /* Use got_addr as the unique identifier */
        (*entries)[i].got_addr = got_addr;
        (*entries)[i].got_target = got_table[i + 3];
    }

    *count = nentries;
    printf("[GOT] Parsed via program headers: %d entries\n", nentries);
    return 0;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int got_parse_self(got_entry_t **entries, int *count) {
    elf_image_t elf = {0};
    int ret = -1;

    if (read_self_exe(&elf) < 0) {
        fprintf(stderr, "[GOT] Failed to read /proc/self/exe\n");
        goto fallback;
    }

    uint64_t load_base = get_load_base();
    if (load_base == 0) {
        fprintf(stderr, "[GOT] Failed to get load base\n");
        free_elf(&elf);
        goto fallback;
    }

    /* Try Method 1: section headers */
    ret = got_parse_sections(&elf, load_base, entries, count);
    free_elf(&elf);

    if (ret == 0) return 0;

    fprintf(stderr, "[GOT] Section headers not available, trying program headers...\n");

fallback:
    /* Method 2: program headers + dynamic segment (stripped binaries) */
    ret = got_parse_dynamic(entries, count);
    return ret;
}

void got_free_entries(got_entry_t *entries) {
    free(entries);
}

uint64_t got_read_target(uint64_t got_addr) {
    if (got_addr == 0) return 0;
    return *(uint64_t *)got_addr;
}

int got_compute_hash(const got_entry_t *entries, int count, uint8_t *hash_out) {
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return -1;

    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);

    for (int i = 0; i < count; i++) {
        uint64_t target = entries[i].got_target;
        EVP_DigestUpdate(ctx, &target, sizeof(uint64_t));
    }

    EVP_DigestFinal_ex(ctx, hash_out, NULL);
    EVP_MD_CTX_free(ctx);
    return 0;
}
