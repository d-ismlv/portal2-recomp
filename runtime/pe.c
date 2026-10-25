// Greffons PE traduits d'avance (voir rt.h, tools/lift.py generate_pe).
//
// libMiles charge les .asi de bin/linux32 avec son propre chargeur PE : il
// projette l'image (mmap, deux vues W/X d'un même fichier ou une projection
// anonyme), recopie les sections, applique .reloc puis appelle RIB_Main et
// les fonctions enregistrées par pointeurs. Rien ne signale la fin de ce
// chargement au runtime ; le point d'interception est donc le premier
// transfert de contrôle vers une adresse non traduite (rt_dispatch), au moment
// où l'image est forcément complète. L'image n'est acceptée que si les octets
// de toutes ses sections de code sont ceux du fichier traduit, chaque mot
// relocalisé valant exactement valeur_fichier + delta : une autre DLL, une
// autre version ou une image incomplète est refusée et le transfert piège.
#include "rt.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_IMAGES 16
#define MAX_LOADED 16
static const RtPeImage *images[MAX_IMAGES];
static unsigned image_count;
typedef struct { const RtPeImage *image; uint32_t base; } Loaded;
static Loaded loaded[MAX_LOADED];
static unsigned loaded_count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void rt_pe_register_image(const RtPeImage *image) {
    pthread_mutex_lock(&lock);
    if (image_count == MAX_IMAGES) {
        fprintf(stderr, "[pe] too many translated PE images (%s)\n", image->name);
        abort();
    }
    images[image_count++] = image;
    pthread_mutex_unlock(&lock);
}

uint64_t rt_pe_hash(const uint8_t *data, size_t size) {
    uint64_t value = UINT64_C(0xcbf29ce484222325);
    for (size_t i = 0; i < size; ++i) value = (value ^ data[i]) * UINT64_C(0x100000001b3);
    return value;
}

const RtPeImage *rt_pe_image_named(const char *name) {
    const RtPeImage *found = NULL;
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < image_count && !found; ++i)
        if (!strcasecmp(images[i]->name, name)) found = images[i];
    pthread_mutex_unlock(&lock);
    return found;
}

// Plage invitée entièrement lisible : pages suivies par vm.c (rt_mmap), ou
// pages hôtes lisibles ailleurs (tas invité, données des modules).
static int readable(uint32_t address, uint32_t length) {
    uint64_t end = (uint64_t)address + length;
    if (!length || end > (UINT64_C(1) << 32)) return 0;
    for (uint64_t page = address & ~UINT64_C(4095); page < end; page += 4096) {
        if (page >= RT_VM_BEGIN && page < RT_VM_END) {
            if ((rt_vm_permissions[(page - RT_VM_BEGIN) >> 12] & 9) != 9) return 0;
        } else if (!rt_guest_readable((uint32_t)page)) return 0;
    }
    return 1;
}

static uint32_t le32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

// Les sections de code de `image` sont-elles en mémoire invitée à `base`
// (vue exécutable), relocalisées comme le fait libMiles ? Une valeur qui
// désigne .text (bit 31 de l'entrée) vaut valeur + (base - ImageBase) ; les
// autres pointent la vue écrivable, dont le delta est déduit de la première
// d'entre elles puis exigé de toutes. Renvoie 1 et les deux deltas si oui.
static int image_matches(const RtPeImage *image, uint32_t base, uint32_t deltas[2]) {
    uint32_t code_delta = base - image->image_base, data_delta = code_delta;
    int data_known = 0;
    const uint32_t *reloc = image->relocations, *relocs_end = reloc + image->relocation_count;
    for (uint32_t s = 0; s < image->section_count; ++s) {
        const RtPeSection *section = &image->sections[s];
        uint64_t start = (uint64_t)base + section->rva;
        if (start + section->size > (UINT64_C(1) << 32) || !readable((uint32_t)start, section->size)) return 0;
        const uint8_t *guest = M + start;
        while (reloc < relocs_end && (*reloc & 0x7fffffffu) < section->rva) ++reloc;
        uint32_t offset = 0;
        for (; reloc < relocs_end && (*reloc & 0x7fffffffu) < section->rva + section->size; ++reloc) {
            uint32_t at = (*reloc & 0x7fffffffu) - section->rva;
            if (at < offset || at + 4 > section->size) return 0;   // impossible si le générateur est correct
            if (memcmp(guest + offset, section->bytes + offset, at - offset)) return 0;
            uint32_t found = le32(guest + at) - le32(section->bytes + at);
            if (*reloc & 0x80000000u) {
                if (found != code_delta) return 0;
            } else if (!data_known) {
                data_delta = found; data_known = 1;
            } else if (found != data_delta) return 0;
            offset = at + 4;
        }
        if (memcmp(guest + offset, section->bytes + offset, section->size - offset)) return 0;
    }
    deltas[0] = data_delta; deltas[1] = code_delta;
    return 1;
}

static int in_code(const RtPeImage *image, uint32_t rva) {
    for (uint32_t s = 0; s < image->section_count; ++s)
        if (rva - image->sections[s].rva < image->sections[s].size) return 1;
    return 0;
}

// Cherche une image traduite contenant `target` : bases candidates alignées
// sur SectionAlignment (le chargeur de Miles projette page par page).
static const Loaded *recognise(uint32_t target) {
    for (unsigned i = 0; i < image_count; ++i) {
        const RtPeImage *image = images[i];
        uint32_t alignment = image->section_alignment ? image->section_alignment : 4096;
        uint64_t base = target - target % alignment;
        for (; base + image->image_size > target; base -= alignment) {
            uint32_t deltas[2];
            if (in_code(image, (uint32_t)(target - base)) && image_matches(image, (uint32_t)base, deltas)) {
                for (unsigned j = 0; j < loaded_count; ++j)
                    if (loaded[j].image == image) {
                        // Un seul delta par image traduite : une seconde instance
                        // vivante ne peut pas être servie correctement.
                        fprintf(stderr, "[pe] %s already loaded at %#x, refusing a second copy at %#x\n",
                                image->name, loaded[j].base, (uint32_t)base);
                        return NULL;
                    }
                if (loaded_count == MAX_LOADED) return NULL;
                image->delta[0] = deltas[0];
                image->delta[1] = deltas[1];
                loaded[loaded_count] = (Loaded){image, (uint32_t)base};
                fprintf(stderr, "[pe] %s recognised at %#x (code delta %#x, data delta %#x)\n", image->name,
                        (uint32_t)base, deltas[1], deltas[0]);
                return &loaded[loaded_count++];
            }
            if (base < alignment) break;
        }
    }
    return NULL;
}

// Cycle de vie : une image oubliée (munmap) puis reconnue de nouveau réécrit
// ses deltas. Le code invité est sérialisé par le verrou invité et Miles ne
// démappe un greffon qu'après avoir cessé de l'appeler ; un appel encore en
// cours dans une image démappée serait déjà une faute sous Linux.
gfn rt_pe_lookup(uint32_t target) {
    pthread_mutex_lock(&lock);
    const Loaded *found = NULL;
    for (unsigned i = 0; i < loaded_count && !found; ++i)
        if (target - loaded[i].base < loaded[i].image->image_size) found = &loaded[i];
    if (!found && image_count) found = recognise(target);
    Loaded entry = found ? *found : (Loaded){0};
    pthread_mutex_unlock(&lock);
    if (!entry.image) return NULL;
    const RtPeImage *image = entry.image;
    uint32_t rva = target - entry.base, lo = 0, hi = image->function_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (image->function_rvas[mid] < rva) lo = mid + 1; else hi = mid;
    }
    if (lo < image->function_count && image->function_rvas[lo] == rva) {
        // Diagnostic : P2_PE_TRACE=1 affiche chaque fonction appelée par adresse
        // (depuis Miles ou par pointeur), à son premier appel et tous les 2^k.
        static _Atomic int trace = -1;
        int tracing = atomic_load_explicit(&trace, memory_order_relaxed);
        if (tracing < 0) atomic_store_explicit(&trace, tracing = getenv("P2_PE_TRACE") != NULL, memory_order_relaxed);
        if (tracing) {
            static _Atomic uint32_t counts[4096];
            uint32_t n = ++counts[(rva ^ (uint32_t)(uintptr_t)image) & 4095];
            if (!(n & (n - 1))) fprintf(stderr, "[pe] %s RVA %#x called (%u)\n", image->name, rva, n);
        }
        return image->functions[lo];
    }
    fprintf(stderr, "[pe] %s: RVA %#x (address %#x) is not a recompiled function\n", image->name, rva, target);
    return NULL;
}

void rt_pe_forget(uint32_t address, uint64_t length) {
    pthread_mutex_lock(&lock);
    for (unsigned i = 0; i < loaded_count;) {
        uint64_t base = loaded[i].base, end = base + loaded[i].image->image_size;
        if (base < (uint64_t)address + length && address < end) loaded[i] = loaded[--loaded_count];
        else ++i;
    }
    pthread_mutex_unlock(&lock);
}

// Le fichier hôte `path` est-il exactement le greffon traduit ? Verdict
// mémorisé par image tant que l'identité du fichier (inode, taille, date)
// ne change pas : la résolution de chemins le demande à chaque accès.
typedef struct { const RtPeImage *image; dev_t device; ino_t inode; off_t size; struct timespec modified; int verdict; } FileVerdict;
static FileVerdict verdicts[MAX_IMAGES];
int rt_pe_file_matches(const RtPeImage *image, const char *path) {
    // Identité et octets du même descripteur ouvert : un remplacement du
    // fichier entre les deux ne peut pas associer un verdict au mauvais fichier.
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    struct stat info;
    if (fstat(fileno(file), &info) || !S_ISREG(info.st_mode) || (uint64_t)info.st_size != image->file_size) {
        fclose(file);
        return 0;
    }
    pthread_mutex_lock(&lock);
    FileVerdict *slot = NULL;
    for (unsigned i = 0; i < MAX_IMAGES; ++i)
        if (verdicts[i].image == image || (!slot && !verdicts[i].image)) slot = &verdicts[i];
    int known = slot && slot->image == image && slot->device == info.st_dev && slot->inode == info.st_ino &&
                slot->size == info.st_size && slot->modified.tv_sec == info.st_mtimespec.tv_sec &&
                slot->modified.tv_nsec == info.st_mtimespec.tv_nsec;
    int verdict = known ? slot->verdict : 0;
    pthread_mutex_unlock(&lock);
    if (known) { fclose(file); return verdict; }
    uint8_t *data = malloc(image->file_size ? image->file_size : 1);
    verdict = data && fread(data, 1, image->file_size, file) == image->file_size && fgetc(file) == EOF &&
              rt_pe_hash(data, image->file_size) == image->file_hash;
    free(data);
    fclose(file);
    pthread_mutex_lock(&lock);
    if (slot) *slot = (FileVerdict){image, info.st_dev, info.st_ino, info.st_size, info.st_mtimespec, verdict};
    pthread_mutex_unlock(&lock);
    return verdict;
}
