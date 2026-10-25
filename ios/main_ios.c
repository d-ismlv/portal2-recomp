// Point d'entrée iOS : SDL2 (libSDL2main) démarre UIKit puis appelle ce main
// sur le thread principal. Il prépare l'environnement et appelle le main du
// runtime (src/main.c, compilé avec -Dmain=p2_main).
//
// Arguments : ceux du lancement (simctl launch … -- arguments), sinon le
// fichier launch.args du paquet (un argument par ligne, # pour commenter).
// --user-dir est ajouté s'il manque : Documents/portal2-userdata du conteneur.
#include <SDL_main.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

int p2_main(int argc, char **argv);

static char *arguments[256];
static int count;
static void push(const char *value) { if (count < 255) arguments[count++] = strdup(value); }

static void read_launch_file(const char *bundle) {
    char path[PATH_MAX], line[PATH_MAX];
    snprintf(path, sizeof(path), "%s/launch.args", bundle);
    FILE *file = fopen(path, "r");
    if (!file) return;
    while (fgets(line, sizeof(line), file)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] && line[0] != '#') push(line);
    }
    fclose(file);
}

int main(int argc, char **argv) {
    // Journal fiable : la console de simctl perd la fin quand le processus meurt.
    const char *log = getenv("P2_LOG_FILE");
    if (log && *log && freopen(log, "w", stderr)) {
        dup2(fileno(stderr), fileno(stdout));
        setvbuf(stderr, NULL, _IONBF, 0);
    }
    // Le moteur garde ouvertes ses archives VPK (172 pour Portal 2) : la limite
    // de 256 descripteurs d'une app iOS ne suffit pas (ReadFile échoue alors).
    struct rlimit files;
    if (!getrlimit(RLIMIT_NOFILE, &files)) {
        rlim_t wanted = files.rlim_max == RLIM_INFINITY || files.rlim_max > 10240 ? 10240 : files.rlim_max;
        if (files.rlim_cur < wanted) { files.rlim_cur = wanted; setrlimit(RLIMIT_NOFILE, &files); }
        getrlimit(RLIMIT_NOFILE, &files);
        fprintf(stderr, "[ios] descripteurs de fichiers : %llu\n", (unsigned long long)files.rlim_cur);
    }
    char exe[PATH_MAX], bundle[PATH_MAX];
    uint32_t size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &size)) return 2;
    snprintf(bundle, sizeof(bundle), "%s", exe);
    char *slash = strrchr(bundle, '/');
    if (slash) *slash = 0;
    // Stub Steam du build iOS (runtime/ios/steam_stub.c) : lié dans l'exécutable.
    setenv("P2_STEAM_API_DYLIB", exe, 0);
    setenv("SteamAppId", "620", 0);
    setenv("SteamGameId", "620", 0);
    push(argc > 0 ? argv[0] : "portal2");
    if (argc > 1) for (int i = 1; i < argc; ++i) push(argv[i]);
    else read_launch_file(bundle);
    int has_user = 0;
    for (int i = 1; i < count; ++i) has_user |= !strcmp(arguments[i], "--user-dir");
    if (!has_user && getenv("HOME")) {
        char user[PATH_MAX], documents[PATH_MAX];
        snprintf(documents, sizeof(documents), "%s/Documents", getenv("HOME"));
        snprintf(user, sizeof(user), "%s/portal2-userdata", documents);
        mkdir(documents, 0755); mkdir(user, 0755);
        // Les options du runtime précèdent « -- » : insérer en tête.
        memmove(arguments + 3, arguments + 1, (size_t)(count - 1) * sizeof(*arguments));
        arguments[1] = strdup("--user-dir"); arguments[2] = strdup(user);
        count += 2;
    }
    arguments[count] = NULL;
    fprintf(stderr, "[ios] %d arguments, Steam : stub iOS (%s)\n", count, exe);
    return p2_main(count, arguments);
}
