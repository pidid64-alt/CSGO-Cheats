/*
 * target_sim - the process the lab reads from.
 *
 * This is the "game" for the Linux lessons: a plain program that keeps a world
 * of simulated entities in heap memory and moves them around 60 times a second.
 * It belongs to whoever runs it, so attaching to it, reading its memory and
 * writing into it is a lesson and not an attack on anybody.
 *
 * What it is built to teach:
 *   - a global pointer that lives inside the binary image (g_world), so the
 *     inspector has to do "module base + static offset" and then dereference,
 *     exactly like the client.dll + dwLocalPlayer pattern in the Windows code;
 *   - a fixed struct layout, so field offsets are a real thing to discover;
 *   - values that change over time, so a reader has to poll.
 *
 * Build: gcc -O2 -Wall -Wextra -g -o target_sim target_sim.c -lm
 */

#define _GNU_SOURCE
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/prctl.h>

#define MAX_ENTITIES 12
#define WORLD_MAGIC 0x4C41424Fu /* "OBAL" little endian, a marker to recognise the struct */

typedef struct {
    float x, y, z;
} Vec3;

/* 64 bytes, no padding: see the offset table in inspector.py */
typedef struct {
    int32_t id;       /* 0x00 */
    int32_t health;   /* 0x04 */
    int32_t team;     /* 0x08 */
    int32_t alive;    /* 0x0C */
    Vec3 origin;      /* 0x10 */
    Vec3 velocity;    /* 0x1C */
    char name[24];    /* 0x28 */
} Entity;

/* 800 bytes */
typedef struct {
    uint32_t magic;             /* 0x00 */
    int32_t entity_count;       /* 0x04 */
    uint64_t tick;              /* 0x08 */
    Vec3 camera_pos;            /* 0x10 */
    float camera_yaw;           /* 0x1C */
    Entity entities[MAX_ENTITIES]; /* 0x20 */
} World;

/*
 * The pointer chain the inspector follows:
 *   ELF symbol g_world -> static offset
 *   + module base from /proc/<pid>/maps -> address of this pointer
 *   -> read 8 bytes -> address of the World on the heap.
 */
World *g_world = NULL;

static const char *kNames[MAX_ENTITIES] = {
    "alpha", "bravo", "charlie", "delta", "echo", "foxtrot",
    "golf", "hotel", "india", "juliet", "kilo", "lima"
};

static void sleep_ms(long ms) {
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);

    /*
     * Explicit consent to be inspected. On distributions where
     * /proc/sys/kernel/yama/ptrace_scope is 1, only a parent may read another
     * process; this line lets the lab's inspector attach anyway. A real game
     * would never do this, which is part of the point.
     */
    prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0);

    World *w = calloc(1, sizeof(World));
    if (!w) {
        fprintf(stderr, "target_sim: out of memory\n");
        return 1;
    }

    w->magic = WORLD_MAGIC;
    w->entity_count = MAX_ENTITIES;
    w->camera_pos.x = 0.f;
    w->camera_pos.y = 0.f;
    w->camera_pos.z = 64.f;
    w->camera_yaw = 0.f;

    for (int i = 0; i < MAX_ENTITIES; i++) {
        Entity *e = &w->entities[i];
        e->id = 1000 + i;
        e->health = 40 + (i * 5) % 61;
        e->team = (i % 2) ? 3 : 2;
        e->alive = 1;
        float angle = (float)i / MAX_ENTITIES * 6.2831853f;
        float radius = 150.f + (float)(i % 4) * 90.f;
        e->origin.x = cosf(angle) * radius;
        e->origin.y = sinf(angle) * radius;
        e->origin.z = 0.f;
        e->velocity.x = 0.4f + (float)(i % 3) * 0.25f; /* angular speed, rad/s */
        e->velocity.y = radius;
        e->velocity.z = angle;
        snprintf(e->name, sizeof(e->name), "%s", kNames[i]);
    }

    g_world = w; /* only now is the pointer valid */

    printf("target_sim: pid        = %d\n", (int)getpid());
    printf("target_sim: &g_world   = %p   (pointer inside the binary image)\n", (void *)&g_world);
    printf("target_sim: g_world    = %p   (World struct on the heap)\n", (void *)w);
    printf("target_sim: sizeof(World)=%zu sizeof(Entity)=%zu\n", sizeof(World), sizeof(Entity));
    printf("target_sim: simulating %d entities at 60 Hz, ctrl-c to stop\n", MAX_ENTITIES);

    int last_health[MAX_ENTITIES];
    for (int i = 0; i < MAX_ENTITIES; i++)
        last_health[i] = w->entities[i].health;

    const float dt = 1.f / 60.f;
    while (1) {
        w->tick++;
        w->camera_yaw = fmodf(w->camera_yaw + dt * 0.3f, 6.2831853f);

        for (int i = 0; i < MAX_ENTITIES; i++) {
            Entity *e = &w->entities[i];

            /* Anything that changed since our own last write came from outside. */
            if (e->health != last_health[i]) {
                printf("target_sim: entity %d (%s) health changed externally: %d -> %d\n",
                       e->id, e->name, last_health[i], e->health);
                last_health[i] = e->health;
            }

            e->velocity.z = fmodf(e->velocity.z + e->velocity.x * dt, 6.2831853f);
            e->origin.x = cosf(e->velocity.z) * e->velocity.y;
            e->origin.y = sinf(e->velocity.z) * e->velocity.y;
            e->origin.z = sinf((float)w->tick * 0.01f + (float)i) * 20.f;

            /* A slow damage/regen cycle so the health bars are not frozen. */
            if (w->tick % 180 == (uint64_t)(i * 15) % 180) {
                int delta = ((int)(w->tick / 180 + (uint64_t)i) % 3 == 0) ? -7 : 5;
                int hp = e->health + delta;
                if (hp > 100) hp = 100;
                if (hp < 1) hp = 100;
                e->health = hp;
                last_health[i] = hp;
            }
        }

        sleep_ms(16);
    }

    free(w);
    return 0;
}
