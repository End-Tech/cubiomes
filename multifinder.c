/**
 * A quad hut finder with lots of fancy options.
 */

#include "finders.h"
#include "generator.h"
#include "layers.h"

#include <errno.h>
#include <getopt.h>
#include <math.h>
#include <unistd.h>


typedef struct {
    int radius;  /* Search radius in blocks. */
    int hutRadius;
    long startSeed;
    long endSeed;
    int threads;
    char outputDir[256];
} SearchOptions;

typedef struct {
    int thread;
    int startIndex;
    const long *qhcandidates;
    long qhcount;
    const SearchOptions *opts;
    char filename[256];
} ThreadInfo;

const char* INT_ERROR = "An integer argument is required with --%s\n";

void usage() {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  multifinder [options]\n");
    fprintf(stderr, "    --help\n");
    fprintf(stderr, "    --radius=<integer>\n");
    fprintf(stderr, "    --start_seed=<integer>\n");
    fprintf(stderr, "    --end_seed=<integer>\n");
    fprintf(stderr, "    --threads=<integer>\n");
    fprintf(stderr, "    --output_dir=<string>\n");
}


long parseHumanArgument(const char *flagName) {
    char *endptr;

    int len = strlen(optarg);
    if (len < 1) {
        fprintf(stderr, INT_ERROR, flagName);
        exit(-1);
    }

    long mult = 1;
    switch (optarg[len-1]) {
        case 'K': mult = 1024L; break;
        case 'M': mult = 1024L*1024L; break;
        case 'B': mult = 1024L*1024L*1024L; break;
        case 'G': mult = 1024L*1024L*1024L; break;
        case 'T': mult = 1024L*1024L*1024L*1024L; break;
    }

    if (mult != 1)
        optarg[len-1] = 0;
    long val = strtol(optarg, &endptr, 10);
    if (errno != 0) {
        fprintf(stderr, INT_ERROR, flagName);
        exit(-1);
    }

    return val*mult;
}


int parseIntArgument(const char *flagName) {
    char *endptr;

    int val = strtol(optarg, &endptr, 10);
    if (errno != 0) {
        fprintf(stderr, INT_ERROR, flagName);
        exit(-1);
    }

    return val;
}


SearchOptions parseOptions(int argc, char *argv[]) {
    int c;
    SearchOptions opts = {
        2048,    // Search radius, in blocks.
        4,       // Witch Hut region search radius.
        0,       // Start base seed.
        1L<<48,  // End base seed.
        1,       // Number of search threads.
        "",
    };

    while (1) {
        static struct option longOptions[] = {
            {"radius",     required_argument, NULL, 'r'},
            {"start_seed", required_argument, NULL, 's'},
            {"end_seed",   required_argument, NULL, 'e'},
            {"threads",    required_argument, NULL, 't'},
            {"output_dir", required_argument, NULL, 'o'},
            {"help",       no_argument,       NULL, 'h'},
        };
        int index = 0;
        c = getopt_long(argc, argv, "r:s:e:t:o:h", longOptions, &index);

        if (c == -1)
            break;

        switch (c) {
            case 'r':
                opts.radius = parseIntArgument(longOptions[index].name);
                opts.hutRadius = (int)ceil((double)opts.radius / 512.0);
                break;
            case 's':
                opts.startSeed = parseHumanArgument(longOptions[index].name);
                break;
            case 'e':
                opts.endSeed = parseHumanArgument(longOptions[index].name);
                break;
            case 't':
                opts.threads = parseIntArgument(longOptions[index].name);
                break;
            case 'o':
                if (strlen(optarg) > 255-13) {
                    fprintf(stderr, "Output path too long.");
                    exit(-1);
                }
                strncpy(opts.outputDir, optarg, 256);
                int len = strlen(opts.outputDir);
                if (opts.outputDir[len-1] == '/')
                    opts.outputDir[len-1] = 0;
                break;
            case 'h':
                usage();
                exit(0);
                break;
            default:
                exit(-1);
        }
    }
    return opts;
}


long* getBaseSeeds(long *qhcount, int threads) {
    const char *seedFileName = "./seeds/quadbases_Q1.txt";

    if (access(seedFileName, F_OK)) {
        fprintf(stderr, "Seed base file does not exist: Creating new one.\n"
                "This may take a few minutes...\n");
        int quality = 1;
        baseQuadWitchHutSearch(seedFileName, threads, quality);
    }

    return loadSavedSeeds(seedFileName, qhcount);
}


int getBiomeAt(const LayerStack g, const Pos pos, int *buf) {
    genArea(&g.layers[g.layerNum-1], buf, pos.x, pos.z, 1, 1);
    return buf[0];
}


void *searchQuadHutsThread(void *data) {
    const ThreadInfo info = *(const ThreadInfo *)data;
    const SearchOptions opts = *info.opts;

    LayerStack g = setupGenerator();
    Layer *lFilterBiome = &g.layers[L_BIOME_256];
    int *biomeCache = allocCache(lFilterBiome, 3, 3);
    int *lastLayerCache = allocCache(&g.layers[g.layerNum-1], 3, 3);
    long j, base, seed;

    // Load the positions of the four structures that make up the quad-structure
    // so we can test the biome at these positions.
    Pos qhpos[4];

    // Setup a dummy layer for Layer 19: Biome.
    Layer layerBiomeDummy;
    setupLayer(256, &layerBiomeDummy, NULL, 200, NULL);

    FILE *fh;
    if (strlen(info.filename)) {
        fh = fopen(info.filename, "w");
        if (fh == NULL) {
            fprintf(stderr, "Could not open file %s.", info.filename);
            return NULL;
        }
    } else {
        fh = stdout;
    }

    // Search for a swamp at the structure positions
    for(int i=info.startIndex;
            i < info.qhcount && info.qhcandidates[i] < opts.endSeed;
            i+=opts.threads) {
        int basehits = 0;
        for (int rZ = -opts.hutRadius-1; rZ < opts.hutRadius; rZ++) {
            for (int rX = -opts.hutRadius-1; rX < opts.hutRadius; rX++) {

                // rZ, rX is the hut region in the upper left of the potential
                // quad hut. Hut regions are 32 chunks/512 blocks. The biome
                // generation layers we're looking at are 1:256 zoom. So
                // the biome area is 2* the hut region. Also, we want the area
                // at the center of the quad-hut regions, so +1.
                int areaX = (rX << 1) + 1;
                int areaZ = (rZ << 1) + 1;

                base = moveTemple(info.qhcandidates[i], rX, rZ);

                // This little magic code checks if there is a meaningful chance
                // for this seed base to generate swamps in the area.

                // The idea is that the conversion from Lush temperature to
                // swampland is independent of surroundings, so we can test the
                // conversion beforehand. Furthermore biomes tend to leak into
                // the negative coordinates because of the Zoom layers, so the
                // majority of hits will occur when SouthEast corner (at a 1:256
                // scale) of the quad-hut has a swampland. (This assumption
                // misses about 1 in 500 quad-hut seeds.) Finally, here we also
                // exploit that the minecraft random number generator is quite
                // bad, such that for the "mcNextRand() mod 6" check it has a
                // period pattern of ~3 on the high seed-bits.
                for (j = 0; j < 5; j++) {
                    seed = base + ((j+0x53) << 48);
                    setWorldSeed(&layerBiomeDummy, seed);
                    setChunkSeed(&layerBiomeDummy, areaX+1, areaZ+1);
                    if(mcNextInt(&layerBiomeDummy, 6) == 5)
                        break;
                }
                if (j >= 5)
                    continue;

                qhpos[0] = getWitchHutPos(base, 0+rX, 0+rZ);
                qhpos[1] = getWitchHutPos(base, 0+rX, 1+rZ);
                qhpos[2] = getWitchHutPos(base, 1+rX, 0+rZ);
                qhpos[3] = getWitchHutPos(base, 1+rX, 1+rZ);

                long hits = 0, swpc;

                for(j = 0; j < 0x10000; j++)
                {
                    seed = base + (j << 48);

                    /** Pre-Generation Checks **/
                    // We can check that at least one swamp could generate in
                    // this area before doing the biome generator checks.
                    setWorldSeed(&layerBiomeDummy, seed);
                    setChunkSeed(&layerBiomeDummy, areaX+1, areaZ+1);
                    if(mcNextInt(&layerBiomeDummy, 6) != 5)
                        continue;

                    // This seed base does not seem to contain many quad huts,
                    // so make a more detailed analysis of the surroundings and
                    // see if there is enough potential for more swamps to
                    // justify searching fruther.
                    if(hits == 0 && (j & 0xfff) == 0xfff)
                    {
                        swpc = 0;
                        setChunkSeed(&layerBiomeDummy, areaX, areaZ+1);
                        swpc += mcNextInt(&layerBiomeDummy, 6) == 5;
                        setChunkSeed(&layerBiomeDummy, areaX+1, areaZ);
                        swpc += mcNextInt(&layerBiomeDummy, 6) == 5;
                        setChunkSeed(&layerBiomeDummy, areaX, areaZ);
                        swpc += mcNextInt(&layerBiomeDummy, 6) == 5;

                        if(swpc < (j > 0x1000 ? 2 : 1))
                            break;
                    }

                    // Dismiss seeds that don't have a swamp near the quad
                    // temple.
                    setWorldSeed(lFilterBiome, seed);
                    genArea(lFilterBiome, biomeCache, areaX+1, areaZ+1, 1, 1);

                    if(biomeCache[0] != swampland)
                        continue;

                    applySeed(&g, seed);
                    if (getBiomeAt(g, qhpos[0], lastLayerCache) != swampland)
                        continue;
                    if (getBiomeAt(g, qhpos[1], lastLayerCache) != swampland)
                        continue;
                    if (getBiomeAt(g, qhpos[2], lastLayerCache) != swampland)
                        continue;
                    if (getBiomeAt(g, qhpos[3], lastLayerCache) != swampland)
                        continue;

                    fprintf(fh, "%ld\n", seed);
                    hits++;
                    basehits++;
                }
                fflush(fh);
            }
        }
        fprintf(stderr, "Base seed %ld (thread %d): %d hits\n",
                info.qhcandidates[i], info.thread, basehits);
    }

    if (strlen(info.filename)) {
        fclose(fh);
    }
    fprintf(stderr, "%s written.\n", info.filename);
    free(biomeCache);
    free(lastLayerCache);
    freeGenerator(g);

    return NULL;
}


int main(int argc, char *argv[])
{
    SearchOptions opts = parseOptions(argc, argv);

    if (opts.threads > 1 && strlen(opts.outputDir) < 1) {
        fprintf(stderr,
                "Must specify --output_dir if using more than one thread.");
        exit(-1);
    }

    fprintf(stderr,
            "Searching base seeds %ld-%ld, radius %d using %d threads...\n",
            opts.startSeed, opts.endSeed, opts.radius, opts.threads);

    // Always initialize the biome list before starting any seed finder or
    // biome generator.
    initBiomes();

    long qhcount;
    const long *qhcandidates = getBaseSeeds(&qhcount, opts.threads);
    int startIndex = 0;
    while (qhcandidates[startIndex] < opts.startSeed && startIndex < qhcount) {
        startIndex++;
    }

    pthread_t threadID[opts.threads];
    ThreadInfo info[opts.threads];

    for (int t=0; t<opts.threads; t++) {
        info[t].thread = t;
        info[t].startIndex = startIndex + t;
        info[t].qhcandidates = qhcandidates;
        info[t].qhcount = qhcount;
        info[t].opts = &opts;

        if (opts.threads == 1 && !strlen(opts.outputDir)) {
            info[t].filename[0] = 0;
        } else {
            snprintf(info[t].filename, 256,
                    "%s/seeds-%02d.txt", opts.outputDir, t);
        }
    }

    for (int t=0; t<opts.threads; t++) {
        pthread_create(
                &threadID[t], NULL, searchQuadHutsThread, (void*)&info[t]);
    }

    for (int t=0; t<opts.threads; t++) {
        pthread_join(threadID[t], NULL);
    }

    if (strlen(opts.outputDir)) {
        char filename[256];
        snprintf(filename, 256, "%s/COMPLETE", opts.outputDir);
        FILE *fh = fopen(filename, "w");
        fprintf(fh, "Done.\n");
        fclose(fh);
    }
    fprintf(stderr, "Done.\n");

    return 0;
}
