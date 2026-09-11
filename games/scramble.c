/**
 * scramble.c -- recreación ampliada del clásico de Konami (1981) para
 * Raspberry Pi Pico + TFT 2.0" SPI, sustituye al juego que antes se
 * llamaba "Defender" en el menú.
 *
 * ESTA VERSIÓN incorpora, sobre la base del scramble.c original, todas
 * las mejoras probadas primero en la maqueta scramble.html (versión
 * "Nivel 2 & Jefe Ajustado"): 6 zonas en vez de 4 (Montañas, OVNIs,
 * Meteoritos, Picos Altos, Cueva Estrecha, Jefe Final), objetos nuevos
 * (cohetes que despegan, OVNIs con trayectoria en "8", meteoritos
 * indestructibles), un jefe final formado por una parrilla de "ladrillos"
 * destructibles + núcleo con vida propia, sistema de partículas para
 * las explosiones, bombas con trayectoria parabólica (gravedad) y
 * colores de zona aleatorios por nivel. Se mantienen los mismos
 * principios de adaptación que pong.c/space_invaders.c/breakout.c/
 * asteroids.c:
 *
 *  - Resolución: campo a pantalla casi completa (320x240 apaisada),
 *    literales fijos en vez de TFT_WIDTH/TFT_HEIGHT.
 *  - Terreno PROCEDURAL (SIN_TAB de 32 pasos, como asteroids.c), pero
 *    ahora cuantizado en bloques de COL_W=32px (como en el HTML) en
 *    vez de columnas de 4px: bastantes menos renderer_fill_rect() por
 *    frame (~11 en vez de ~78), el desplazamiento se sigue viendo
 *    suave gracias al offset de scroll_px % COL_W. El terreno se seguía
 *    redibujando entero cada tick (no hay nada que redibujar
 *    incrementalmente en un scroll continuo) y se pinta encima
 *    nave/objetos/disparos sin posiciones "prev_*".
 *  - Objetos de tierra/aire (combustible, cohetes, bases, OVNIs,
 *    meteoritos) viven en un pool (MAX_OBJECTS) que se rellena por
 *    distancia recorrida, incluso varias zonas por delante (igual que
 *    el HTML), porque ahora la zona activa se deriva continuamente de
 *    scroll_px / ZONE_LENGTH en vez de resetear el pool en cada
 *    "zone_start()" del esquema antiguo.
 *  - Combustible: igual que antes (contador de ticks), pero ahora se
 *    rellena también al respawnear tras perder una nave (más permisivo,
 *    tomado del HTML) además de al destruir un depósito.
 *  - Jefe final (zona ZONE_BIG_SHIP): al acercarse bloquea el scroll
 *    (scroll_locked) hasta ser destruido -- ladrillos individuales dan
 *    puntos, el núcleo tiene HQ_HP golpes; al morir dispara
 *    SCR_VICTORY (con bonus) y se pasa de nivel, reiniciando el ciclo
 *    de 6 zonas con nuevos colores aleatorios y algo más de velocidad.
 *  - Controles: idénticos al original -- encoder 1 (giro) = sube/baja
 *    la nave con inercia (enc_momentum(), igual que la pala de
 *    breakout.c); BTN_ENC1_SW (mantenido) = empuje horizontal (mismo
 *    concepto que el thrust de asteroids.c); BTN_J1_A = disparo;
 *    BTN_J1_B = bomba (ahora con trayectoria parabólica en vez de caída
 *    recta, tomado del HTML).
 *  - Sonido: mapeado a los mismos efectos que ya existen --
 *    sound_effect_shoot (disparo/bomba), sound_effect_explosion
 *    (impacto/destrucción/choque), sound_effect_select (golpe parcial
 *    al jefe), sound_effect_lose_point (nave perdida),
 *    sound_effect_success (jefe destruido / zona), sound_effect_game_over.
 *  - Sin hs_input/SCR_ENTER_NAME: highscores_enter() bloqueante, como
 *    en los otros juegos.
 *  - Bucle propio: game_scramble_run(mode) con su propio bucle.
 *
 * SUPOSICIONES a revisar si no compila tal cual (renderer.h/controls.h/
 * sound.h no se adjuntaron con este encargo, así que me he ceñido
 * estrictamente a lo que el scramble.c original ya demostraba usar):
 *  - Paleta de color: sólo he usado COLOR_BLACK/WHITE/RED/GREEN/CYAN/
 *    YELLOW/MAGENTA, que son los que aparecían en el archivo original.
 *    El HTML usa colores RGB arbitrarios por zona (palette de 10 tonos);
 *    aquí ZONE_PALETTE[] elige aleatoriamente entre esos 5 colores "de
 *    juego" (se excluyen negro/blanco) en cada nivel. Si tu renderer.h
 *    define más constantes COLOR_*, amplía ZONE_PALETTE[] con ellas.
 *  - Se usa <math.h> (sinf/cosf) para las trayectorias de OVNIs, jefe
 *    final y bombas parabólicas, tal como hace el HTML con Math.sin/
 *    cos; el Pico SDK enlaza libm sin configuración extra.
 *  - MAX_PARTICLES se ha bajado de 120 (HTML, pensado para <canvas>)
 *    a 48 para no disparar el número de renderer_fill_rect() por frame
 *    en un microcontrolador.
 *  - Vidas: 3 (como el scramble.c original), no 99 (el HTML usaba 99
 *    como ayuda de desarrollo/depuración).
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>
#include "pico/stdlib.h"
#include "scramble.h"
#include "renderer.h"
#include "controls.h"
#include "highscores.h"
#include "sound.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

// ---------------------------------------------------------------------------
// Área de juego -- literales fijos (ver comentario largo en los otros
// archivos sobre por qué no TFT_WIDTH/TFT_HEIGHT aquí).
// ---------------------------------------------------------------------------
#define SCREEN_W 320
#define SCREEN_H 240

#define PLAY_X   4
#define PLAY_Y   3
#define PLAY_W   (SCREEN_W - 2 * PLAY_X)   // 312
#define PLAY_H   (SCREEN_H - 2 * PLAY_Y)   // 234
#define CX       (PLAY_X + PLAY_W / 2)
#define CY       (PLAY_Y + PLAY_H / 2)

#define TICKS_S  60   // referencia nominal para pausas simples (no rítmicas)

// ---------------------------------------------------------------------------
// Utilidades generales
// ---------------------------------------------------------------------------
static int clamp(int v, int lo, int hi) { return v<lo?lo:v>hi?hi:v; }
static float clampf(float v, float lo, float hi) { return v<lo?lo:v>hi?hi:v; }
static bool rects_overlap(int ax,int ay,int aw,int ah,int bx,int by,int bw,int bh) {
    return (ax < bx+bw && ax+aw > bx && ay < by+bh && ay+ah > by);
}
static bool rects_overlapf(float ax,float ay,int aw,int ah,float bx,float by,int bw,int bh) {
    return (ax < bx+bw && ax+aw > bx && ay < by+bh && ay+ah > by);
}

static uint32_t rng_state_v = 12345;
static uint32_t rng_next(void) {
    rng_state_v ^= rng_state_v << 13;
    rng_state_v ^= rng_state_v >> 17;
    rng_state_v ^= rng_state_v << 5;
    return rng_state_v;
}
static float rng_float(void) { return (float)(rng_next() % 1000) / 1000.0f; }

// ---------------------------------------------------------------------------
// Nave -- inercia vertical (encoder 1: girar sube/baja, mismo esquema
// que la pala de breakout.c) + empuje horizontal con el switch del
// encoder 1 (BTN_ENC1_SW), igual concepto que el thrust de
// asteroids.c (SHIP_THRUST + fricción) pero en 1D y acotado a un
// rango de pantalla en vez de espacio abierto.
// ---------------------------------------------------------------------------
#define SHIP_W   16
#define SHIP_H   10
#define SHIP_X_MIN (PLAY_X + 20)    // tope izquierdo (reposo)
#define SHIP_X_MAX (PLAY_X + 110)   // máximo avance con el empuje

#define HUD_H    16   // franja superior reservada para el HUD
#define SHIP_Y_MIN (PLAY_Y + HUD_H + 2)
#define SHIP_Y_MAX (PLAY_Y + PLAY_H - 2 - SHIP_H)

#define SHIP_ACCEL      2
#define SHIP_VEL_MAX    9
#define SHIP_DECAY_NUM  6
#define SHIP_DECAY_DEN 10
#define SHIP_AI_SPEED   3

#define THRUST_ACCEL   1   // aceleración del empuje horizontal, por tick
#define THRUST_VX_MAX  4   // velocidad horizontal máxima (avance o retorno)

#define RESPAWN_INV  (TICKS_S * 2)

static int ship_vel = 0;
static int ship_x  = SHIP_X_MIN;
static int ship_vx = 0;
static int ship_y, ship_inv_ticks;

static int enc_momentum(int enc_raw, int *vel) {
    if (enc_raw > 0) {
        *vel += SHIP_ACCEL * enc_raw;
        if (*vel > SHIP_VEL_MAX) *vel = SHIP_VEL_MAX;
    } else if (enc_raw < 0) {
        *vel += SHIP_ACCEL * enc_raw;
        if (*vel < -SHIP_VEL_MAX) *vel = -SHIP_VEL_MAX;
    } else {
        *vel = *vel * SHIP_DECAY_NUM / SHIP_DECAY_DEN;
    }
    return *vel;
}

// ---------------------------------------------------------------------------
// Terreno -- procedural con tabla seno de 32 pasos, cuantizado en
// bloques de COL_W px (ver cabecera del archivo).
// ---------------------------------------------------------------------------
#define FP 256
static const int16_t SIN_TAB[32] = {
      0,  50,  98, 142, 181, 213, 237, 251,
    256, 251, 237, 213, 181, 142,  98,  50,
      0, -50, -98,-142,-181,-213,-237,-251,
   -256,-251,-237,-213,-181,-142, -98, -50
};

#define ZONE_STEEP_MOUNTAINS 0
#define ZONE_UFOS             1
#define ZONE_METEORS          2
#define ZONE_HIGH_MOUNTAINS   3
#define ZONE_NARROW_CAVE      4
#define ZONE_BIG_SHIP         5
#define NUM_ZONES             6

#define GROUND_MIN_H  24
#define GROUND_MAX_H 180
#define ZONE_LENGTH 3200   // longitud (en unidades de mundo) de cada zona

#define COL_W    32
#define NUM_COLS (PLAY_W / COL_W + 2)

static const char *zone_short_names[NUM_ZONES] = {
    "MONTANAS", "OVNIS", "METEOROS", "PICOS ALT", "CUEVA", "JEFE"
};

// Paleta de colores de zona, elegida al azar por nivel (ver nota de
// suposiciones al inicio del archivo).
static uint16_t ZONE_PALETTE[5];
static uint16_t zone_colors[NUM_ZONES];

static void init_zone_palette(void) {
    ZONE_PALETTE[0] = COLOR_GREEN;
    ZONE_PALETTE[1] = COLOR_CYAN;
    ZONE_PALETTE[2] = COLOR_YELLOW;
    ZONE_PALETTE[3] = COLOR_MAGENTA;
    ZONE_PALETTE[4] = COLOR_RED;
}

static void randomize_level_colors(void) {
    for (int i=0;i<NUM_ZONES;i++)
        zone_colors[i] = ZONE_PALETTE[rng_next() % 5];
}

static int32_t wave(int32_t bx, int wavelength, int amplitude, int phase) {
    int32_t idx = ((bx + phase) * 32) / wavelength;
    return (amplitude * SIN_TAB[idx & 31]) / FP;
}

static int cave_corridor_offset(int32_t section_id) {
    uint32_t hash = ((uint32_t)section_id * 2654435761u) ^ ((uint32_t)section_id >> 16);
    return (int)(hash % 110) - 55;
}

static int floor_h_for(int32_t world_x, int zone) {
    if (world_x < 400) return 24;   // franja de aterrizaje segura al empezar
    int32_t bx = (world_x / COL_W) * COL_W;
    int h = GROUND_MIN_H;
    switch (zone) {
        case ZONE_STEEP_MOUNTAINS: {
            int macro = wave(bx,1200,100,0);
            if (macro > 10) h = 70 + wave(bx,140,65,0) + wave(bx,60,35,100);
            else             h = 30 + wave(bx,300,10,0);
            break;
        }
        case ZONE_UFOS:
            h = 35 + wave(bx,500,15,0);
            break;
        case ZONE_METEORS:
            h = 40 + wave(bx,350,20,100);
            break;
        case ZONE_HIGH_MOUNTAINS:
            h = 95 + wave(bx,200,75,0) + wave(bx,80,40,50);
            break;
        case ZONE_NARROW_CAVE: {
            int32_t section = bx / 256;
            int block = (int)((bx / 128) % 4);
            int offset = cave_corridor_offset(section);
            int base_h = 90 + offset;
            if (block == 0)      h = clamp(base_h+40, 30, 160);
            else if (block == 2) h = clamp(base_h-40, 30, 160);
            else                 h = 20;
            break;
        }
        case ZONE_BIG_SHIP:
            h = 25;
            break;
        default:
            h = GROUND_MIN_H;
            break;
    }
    return clamp(h, GROUND_MIN_H, GROUND_MAX_H);
}

static int ceil_h_for(int32_t world_x, int zone) {
    if (world_x < 400) return 0;
    int32_t bx = (world_x / COL_W) * COL_W;
    int h = 0;
    if (zone == ZONE_UFOS) {
        h = 25 + wave(bx,400,12,500);
    } else if (zone == ZONE_NARROW_CAVE) {
        int32_t section = bx / 256;
        int block = (int)((bx / 128) % 4);
        int offset = cave_corridor_offset(section);
        int base_h = 90 + offset;
        if (block == 0 || block == 2) {
            int fh = (block==0) ? clamp(base_h+40,30,160) : clamp(base_h-40,30,160);
            h = PLAY_H - fh - 26;
        } else {
            h = 15;
        }
    }
    return clamp(h, 0, 180);
}

static int zone_for_world(int32_t world_x) {
    int z = (int)(world_x / ZONE_LENGTH);
    if (z >= NUM_ZONES) z = NUM_ZONES - 1;
    if (z < 0) z = 0;
    return z;
}

// ---------------------------------------------------------------------------
// Objetos de tierra/aire -- combustible, cohetes, bases, OVNIs,
// meteoritos. Pool pequeño, se rellena por distancia recorrida
// (incluso varias zonas por delante, como el HTML).
// ---------------------------------------------------------------------------
#define OBJ_NONE   0
#define OBJ_FUEL   1
#define OBJ_ROCKET 2
#define OBJ_BASE   3
#define OBJ_UFO    4
#define OBJ_METEOR 5

#define OBJ_W 20
#define OBJ_H 16
#define MAX_OBJECTS 24

#define ROCKET_SPEED 3

typedef struct {
    bool    active;
    int32_t world_x;
    float   rel_y;        // ascenso del cohete tras despegar
    int     hp;
    uint8_t type;
    int     launch_delay;
    bool    launched;
    float   base_y;        // altura fija de OVNI/meteorito
    float   t;              // fase acumulada (oscilación OVNI)
    float   phase;           // desfase aleatorio
    float   amp;              // amplitud oscilación (OVNI)
    float   speed;             // velocidad propia (OVNI/meteorito)
} GroundObj;

static GroundObj gobjs[MAX_OBJECTS];

// ---------------------------------------------------------------------------
// Disparos -- ametralladora (horizontal), bomba única con trayectoria
// parabólica (gravedad), proyectiles enemigos del jefe final.
// ---------------------------------------------------------------------------
#define MAX_BULLETS 4
#define BULLET_SPD  7
#define SHOOT_COOLDOWN 7

#define MAX_BOMBS 1
#define BOMB_INIT_VX 3.0f
#define BOMB_INIT_VY 0.2f
#define BOMB_GRAVITY 0.08f
#define BOMB_COOLDOWN 12

#define MAX_ENEMY_BULLETS 12

typedef struct { int x, y; bool active; } Bullet;
typedef struct { float x, y, vx, vy; bool active; } Bomb;
typedef struct { float x, y, vx, vy; bool active; } EnemyBullet;

static Bullet      bullets[MAX_BULLETS];
static Bomb        bombs[MAX_BOMBS];
static EnemyBullet enemy_bullets[MAX_ENEMY_BULLETS];

// ---------------------------------------------------------------------------
// Partículas de explosión (versión reducida del HTML para no saturar
// el bus SPI con demasiados renderer_fill_rect() por frame).
// ---------------------------------------------------------------------------
#define MAX_PARTICLES 48

typedef struct { bool active; float x,y,vx,vy; int life; uint16_t color; } Particle;
static Particle particles[MAX_PARTICLES];

static void add_explosion(float x, float y, bool big) {
    static const uint16_t pcolors[4] = { COLOR_WHITE, COLOR_YELLOW, COLOR_RED, COLOR_CYAN };
    int count = big ? 40 : 8;
    int created = 0;
    for (int i=0;i<MAX_PARTICLES && created<count;i++) {
        if (particles[i].active) continue;
        Particle *p = &particles[i];
        p->active = true;
        p->x = x; p->y = y;
        float angle = rng_float() * 2.0f * (float)M_PI;
        float speed = big ? (1.5f + rng_float()*6.0f) : (1.0f + rng_float()*3.5f);
        p->vx = cosf(angle) * speed;
        p->vy = sinf(angle) * speed;
        p->life = big ? (18 + (int)(rng_next()%20)) : (10 + (int)(rng_next()%10));
        p->color = pcolors[rng_next() % 4];
        created++;
    }
}

static void update_particles(void) {
    for (int i=0;i<MAX_PARTICLES;i++) {
        if (!particles[i].active) continue;
        particles[i].x += particles[i].vx;
        particles[i].y += particles[i].vy;
        if (--particles[i].life <= 0) particles[i].active = false;
    }
}

static void draw_particles(void) {
    for (int i=0;i<MAX_PARTICLES;i++)
        if (particles[i].active)
            renderer_fill_rect((int)particles[i].x, (int)particles[i].y, 2, 2, particles[i].color);
}

// ---------------------------------------------------------------------------
// Jefe final (zona ZONE_BIG_SHIP) -- parrilla de "ladrillos"
// destructibles + núcleo con vida propia, mismo diseño que el HTML.
// ---------------------------------------------------------------------------
#define BRICK_W 12
#define BRICK_H  8
#define SHIP_GRID_COLS 12
#define SHIP_GRID_ROWS 10
#define ALIEN_MAX_HP 5
#define MAX_BRICKS 70
#define ALIEN_CORE_W 48
#define ALIEN_CORE_H 32

typedef struct { bool active; int rel_x, rel_y; uint16_t color; } Brick;
static Brick   big_ship_bricks[MAX_BRICKS];
static int     num_bricks;
static int     alien_hp;
static int32_t alien_x;
static int     alien_base_y;
static float   alien_t;
static int     alien_shoot_cd;

static float alien_float_offset(void) {
    return sinf(alien_t) * 28.0f + cosf(alien_t * 0.7f) * 12.0f;
}
static float alien_top(void) { return (float)alien_base_y + alien_float_offset(); }
static int   alien_screen_x(void);   // forward, definido tras 'scroll_px'

static void init_big_ship(int32_t scroll_px_now) {
    num_bricks = 0;
    for (int r=0;r<SHIP_GRID_ROWS;r++) {
        int c0=0, c1=-1;
        if (r==0 || r==9) { c0=4; c1=7; }
        else if (r==1 || r==8) { c0=3; c1=8; }
        else if (r==2 || r==7) { c0=2; c1=9; }
        else { c0=0; c1=11; }
        for (int c=c0;c<=c1;c++) {
            if (r>=3 && r<=6 && c>=4 && c<=7) continue;   // hueco central del núcleo
            if (num_bricks >= MAX_BRICKS) break;
            uint16_t col = COLOR_CYAN;
            if (r<=2) col = COLOR_CYAN;
            else if (r>=7) col = COLOR_YELLOW;
            else col = COLOR_MAGENTA;
            big_ship_bricks[num_bricks].active = true;
            big_ship_bricks[num_bricks].rel_x = c*BRICK_W;
            big_ship_bricks[num_bricks].rel_y = r*BRICK_H;
            big_ship_bricks[num_bricks].color = col;
            num_bricks++;
        }
    }
    alien_x = scroll_px_now + PLAY_W + 40;
    alien_base_y = PLAY_Y + 35;
    alien_t = rng_float() * 2.0f * (float)M_PI;
    alien_hp = ALIEN_MAX_HP;
    alien_shoot_cd = 25;
    for (int i=0;i<MAX_ENEMY_BULLETS;i++) enemy_bullets[i].active = false;
}

// ---------------------------------------------------------------------------
// Fuel / dificultad / puntuación
// ---------------------------------------------------------------------------
#define FUEL_MAX    100
#define FUEL_REFILL  30

#define SCROLL_SPD0    120
#define SCROLL_SPD_INC  24
#define SCROLL_SPD_MAX 260

#define SCR_FUEL_PTS    50
#define SCR_ROCKET_PTS 100
#define SCR_BASE_PTS   150
#define SCR_UFO_PTS    100
#define SCR_ALIEN_BONUS 3000

// ---------------------------------------------------------------------------
// Estado de partida
// ---------------------------------------------------------------------------
typedef enum {
    SCR_TITLE, SCR_READY, SCR_PLAYING, SCR_DEAD,
    SCR_VICTORY, SCR_OVER, SCR_SCORES,
} ScrState;

static ScrState state;
static bool demo, g_done;
static bool scroll_locked, boss_engaged;
static int  blink, demo_ticks, pause_cnt;
static int  lives, level, score, fuel, fuel_cd;
static int  zone;
static int32_t scroll_px, next_spawn_world;
static int  scroll_acc, scroll_spd;
static int  shoot_cd, bomb_cd;

static int fuel_ticks_for_level(void) {
    int t = 20 - (level-1)*2;
    return t < 6 ? 6 : t;
}

static int alien_screen_x(void) { return PLAY_X + (int)(alien_x - scroll_px); }

// Forward declarations (evitan reordenar todo el archivo por
// dependencias cruzadas entre update_objects/update_bullets/etc.)
static void ship_crash(void);
static void try_shoot(void);
static void try_bomb(void);
static void damage_object(int idx);
static void draw_field_static(void);
static void draw_ready_screen(void);
static bool check_alien_hit(float x, float y, int w, int h);
static void enemy_fire(float ox, float oy);

// ---------------------------------------------------------------------------
// Gestión de nave / partida
// ---------------------------------------------------------------------------
static void ship_respawn(void) {
    ship_x = SHIP_X_MIN;
    ship_vx = 0;
    ship_vel = 0;
    ship_inv_ticks = RESPAWN_INV;

    int32_t wx = scroll_px + (ship_x + SHIP_W/2 - PLAY_X);
    int fh = floor_h_for(wx, zone);
    int ch = ceil_h_for(wx, zone);
    int ground_top  = PLAY_Y + PLAY_H - 1 - fh;
    int ceil_bottom = PLAY_Y + 1 + ch;
    ship_y = clamp((ground_top+ceil_bottom)/2 - SHIP_H/2, SHIP_Y_MIN, SHIP_Y_MAX);
}

static void game_start(void) {
    lives = 3; level = 1; zone = ZONE_STEEP_MOUNTAINS; score = 0;
    fuel = FUEL_MAX; fuel_cd = fuel_ticks_for_level();
    scroll_px = 0; scroll_acc = 0;
    scroll_spd = SCROLL_SPD0;
    next_spawn_world = 250;
    scroll_locked = false;
    boss_engaged = false;
    rng_state_v = (uint32_t)time_us_32();
    randomize_level_colors();

    for (int i=0;i<MAX_OBJECTS;i++)        gobjs[i].active = false;
    for (int i=0;i<MAX_BULLETS;i++)        bullets[i].active = false;
    for (int i=0;i<MAX_BOMBS;i++)          bombs[i].active = false;
    for (int i=0;i<MAX_ENEMY_BULLETS;i++)  enemy_bullets[i].active = false;
    for (int i=0;i<MAX_PARTICLES;i++)      particles[i].active = false;
    shoot_cd = 0;
    bomb_cd = 0;
    ship_respawn();
}

static void next_level_setup(void) {
    level++;
    zone = ZONE_STEEP_MOUNTAINS;
    scroll_locked = false;
    boss_engaged = false;
    scroll_px = 0; scroll_acc = 0;
    scroll_spd = SCROLL_SPD0 + (level-1)*SCROLL_SPD_INC;
    if (scroll_spd > SCROLL_SPD_MAX) scroll_spd = SCROLL_SPD_MAX;
    next_spawn_world = 250;
    randomize_level_colors();
    for (int i=0;i<MAX_OBJECTS;i++)        gobjs[i].active = false;
    for (int i=0;i<MAX_ENEMY_BULLETS;i++)  enemy_bullets[i].active = false;
    ship_respawn();
}

// ---------------------------------------------------------------------------
// Objetos de tierra/aire
// ---------------------------------------------------------------------------
static void spawn_obj(uint8_t type, int32_t world_x, int hp, float extra_y) {
    for (int i=0;i<MAX_OBJECTS;i++) {
        if (gobjs[i].active) continue;
        GroundObj *o = &gobjs[i];
        o->active = true;
        o->type = type;
        o->world_x = world_x;
        o->rel_y = 0;
        o->hp = hp;
        o->launched = false;
        o->launch_delay = (type==OBJ_ROCKET) ? 60 + (int)(rng_next()%90) : 40 + (int)(rng_next()%80);
        o->base_y = extra_y;
        o->t = rng_float() * 2.0f * (float)M_PI;
        o->phase = rng_float() * 2.0f * (float)M_PI;
        o->amp = (type==OBJ_UFO) ? 28.0f + (rng_next()%22) : 10.0f + (rng_next()%12);
        o->speed = 1.8f + (rng_next()%20)/10.0f;
        return;
    }
}

static void maybe_spawn_objects(void) {
    if (zone == ZONE_BIG_SHIP) return;

    while (next_spawn_world <= scroll_px + PLAY_W + 40) {
        int target_zone = (int)(next_spawn_world / ZONE_LENGTH);
        if (target_zone >= ZONE_BIG_SHIP) break;

        int spacing = 48 + (int)(rng_next() % 32);
        int roll = (int)(rng_next() % 100);

        switch (target_zone) {
            case ZONE_STEEP_MOUNTAINS:
            case ZONE_HIGH_MOUNTAINS:
                if (roll < 90)      spawn_obj(OBJ_ROCKET, next_spawn_world, 1, 0);
                else if (roll < 98) spawn_obj(OBJ_FUEL,   next_spawn_world, 1, 0);
                else                spawn_obj(OBJ_BASE,   next_spawn_world, 1, 0);
                break;

            case ZONE_UFOS:
                if (roll < 70) {
                    float ufo_y = PLAY_Y + HUD_H + 15 + (rng_next() % (PLAY_H - HUD_H - 45));
                    spawn_obj(OBJ_UFO, next_spawn_world, 1, ufo_y);
                } else {
                    spawn_obj(OBJ_FUEL, next_spawn_world, 1, 0);
                }
                break;

            case ZONE_METEORS:
                if (roll < 65) {
                    float my = PLAY_Y + 30 + (rng_next() % 120);
                    spawn_obj(OBJ_METEOR, next_spawn_world, 1, my);
                } else if (roll < 85) {
                    spawn_obj(OBJ_BASE, next_spawn_world, 1, 0);
                } else {
                    spawn_obj(OBJ_FUEL, next_spawn_world, 1, 0);
                }
                break;

            case ZONE_NARROW_CAVE:
                spawn_obj(OBJ_FUEL, next_spawn_world, 1, 0);
                break;
        }
        next_spawn_world += spacing;
    }
}

static void damage_object(int idx) {
    if (gobjs[idx].type == OBJ_METEOR) return;   // indestructible, sólo se esquiva

    gobjs[idx].hp--;
    int sx = PLAY_X + (int)(gobjs[idx].world_x - scroll_px) + OBJ_W/2;
    int fh = floor_h_for(gobjs[idx].world_x, zone);
    int sy = (gobjs[idx].base_y > 0.5f) ? (int)gobjs[idx].base_y
                                        : (PLAY_Y+PLAY_H-1-fh-OBJ_H+OBJ_H/2);

    if (gobjs[idx].hp <= 0) {
        uint8_t type = gobjs[idx].type;
        gobjs[idx].active = false;
        sound_effect_explosion();
        add_explosion((float)sx, (float)sy, false);

        int pts = (type==OBJ_FUEL) ? SCR_FUEL_PTS
                : (type==OBJ_BASE) ? SCR_BASE_PTS
                : (type==OBJ_UFO)  ? SCR_UFO_PTS : SCR_ROCKET_PTS;
        score += pts * level;
        if (type == OBJ_FUEL) {
            fuel += FUEL_REFILL;
            if (fuel > FUEL_MAX) fuel = FUEL_MAX;
        }
    }
}

static void update_objects(void) {
    for (int i=0;i<MAX_OBJECTS;i++) {
        GroundObj *o = &gobjs[i];
        if (!o->active) continue;

        int sx;
        int top;

        if (o->type == OBJ_UFO) {
            o->t += 0.04f * o->speed;
            o->world_x -= (int32_t)(1.0f + o->speed*0.2f);
            sx = PLAY_X + (int)(o->world_x - scroll_px);

            int fh = floor_h_for(o->world_x, zone);
            int ch = ceil_h_for(o->world_x, zone);
            int ground_top  = PLAY_Y+PLAY_H-1-fh;
            int ceil_bottom = PLAY_Y+1+ch;

            float ft = o->base_y + sinf(o->t+o->phase) * o->amp;
            ft = clampf(ft, (float)(ceil_bottom+4), (float)(ground_top-OBJ_H-4));
            top = (int)ft;
            sx += (int)(sinf((o->t+o->phase)*2.0f) * (o->amp*0.9f));

        } else if (o->type == OBJ_METEOR) {
            o->world_x -= (int32_t)(o->speed * 2.0f);
            sx = PLAY_X + (int)(o->world_x - scroll_px);
            top = (int)o->base_y;

        } else {
            sx = PLAY_X + (int)(o->world_x - scroll_px);
            int fh = floor_h_for(o->world_x + OBJ_W/2, zone);
            int base_top = PLAY_Y + PLAY_H - fh - OBJ_H;
            top = base_top - (int)o->rel_y;

            if (o->type == OBJ_ROCKET && sx < PLAY_X+PLAY_W-10) {
                if (!o->launched) {
                    if (--o->launch_delay <= 0) o->launched = true;
                } else {
                    o->rel_y += ROCKET_SPEED;
                    if (top < PLAY_Y) { o->active = false; continue; }
                }
            }
        }

        if (sx + OBJ_W < PLAY_X) { o->active = false; continue; }

        if (ship_inv_ticks <= 0 && rects_overlap(ship_x,ship_y,SHIP_W,SHIP_H, sx,top,OBJ_W,OBJ_H)) {
            ship_crash();
        }
    }
}

// ---------------------------------------------------------------------------
// Disparos del jugador y del jefe final
// ---------------------------------------------------------------------------
static void try_shoot(void) {
    if (shoot_cd > 0) return;
    for (int i=0;i<MAX_BULLETS;i++) {
        if (bullets[i].active) continue;
        bullets[i].active = true;
        bullets[i].x = ship_x+SHIP_W;
        bullets[i].y = ship_y+SHIP_H/2-1;
        shoot_cd = SHOOT_COOLDOWN;
        sound_effect_shoot();
        return;
    }
}

static void try_bomb(void) {
    if (bomb_cd > 0) return;
    for (int i=0;i<MAX_BOMBS;i++) if (bombs[i].active) return;   // sólo 1 a la vez
    for (int i=0;i<MAX_BOMBS;i++) {
        bombs[i].active = true;
        bombs[i].x = (float)(ship_x + SHIP_W/2);
        bombs[i].y = (float)(ship_y + SHIP_H);
        bombs[i].vx = BOMB_INIT_VX;
        bombs[i].vy = BOMB_INIT_VY;
        bomb_cd = BOMB_COOLDOWN;
        sound_effect_shoot();
        return;
    }
}

static void enemy_fire(float ox, float oy) {
    int shots = 2;
    for (int s=0;s<shots;s++) {
        for (int i=0;i<MAX_ENEMY_BULLETS;i++) {
            if (enemy_bullets[i].active) continue;
            enemy_bullets[i].active = true;
            enemy_bullets[i].x = ox;
            enemy_bullets[i].y = oy + (s*8 - 4);
            enemy_bullets[i].vx = -2.2f - rng_float()*0.8f;
            enemy_bullets[i].vy = (rng_float()-0.5f) * 1.5f;
            break;
        }
    }
}

static void update_enemy_bullets(void) {
    for (int i=0;i<MAX_ENEMY_BULLETS;i++) {
        EnemyBullet *eb = &enemy_bullets[i];
        if (!eb->active) continue;
        eb->x += eb->vx;
        eb->y += eb->vy;
        if (eb->x < PLAY_X || eb->y < PLAY_Y || eb->y > PLAY_Y+PLAY_H) { eb->active=false; continue; }
        if (ship_inv_ticks<=0 && rects_overlapf(ship_x,ship_y,SHIP_W,SHIP_H, eb->x,eb->y,6,3)) {
            eb->active = false;
            ship_crash();
        }
    }
}

static bool check_alien_hit(float x, float y, int w, int h) {
    float top = alien_top();
    int   ax  = alien_screen_x();
    float ay  = top + 3*BRICK_H;

    if (rects_overlapf(x,y,w,h, (float)ax,ay,ALIEN_CORE_W,ALIEN_CORE_H)) {
        alien_hp--;
        sound_effect_select();
        add_explosion(ax+ALIEN_CORE_W/2, ay+ALIEN_CORE_H/2, false);
        if (alien_hp <= 0) {
            sound_effect_success();
            add_explosion(ax+ALIEN_CORE_W/2, ay+ALIEN_CORE_H/2, true);
            score += SCR_ALIEN_BONUS * level;
            pause_cnt = TICKS_S*3;
            state = SCR_VICTORY;
        }
        return true;
    }
    return false;
}

static void update_bullets(void) {
    if (shoot_cd > 0) shoot_cd--;
    float top = alien_top();

    for (int i=0;i<MAX_BULLETS;i++) {
        Bullet *b = &bullets[i];
        if (!b->active) continue;
        b->x += BULLET_SPD;
        if (b->x > PLAY_X+PLAY_W) { b->active=false; continue; }

        for (int j=0;j<MAX_OBJECTS;j++) {
            if (!gobjs[j].active || gobjs[j].type==OBJ_METEOR) continue;
            int sx = PLAY_X + (int)(gobjs[j].world_x - scroll_px);
            int fh = floor_h_for(gobjs[j].world_x + OBJ_W/2, zone);
            int sy = (gobjs[j].type==OBJ_UFO)
                     ? (int)(gobjs[j].base_y + sinf(gobjs[j].t+gobjs[j].phase)*gobjs[j].amp)
                     : (int)(PLAY_Y+PLAY_H-fh-OBJ_H-gobjs[j].rel_y);
            if (rects_overlap(b->x,b->y,3,2, sx,sy,OBJ_W,OBJ_H)) {
                b->active = false;
                damage_object(j);
                break;
            }
        }
        if (!b->active) continue;

        if (zone == ZONE_BIG_SHIP) {
            for (int k=0;k<num_bricks;k++) {
                if (!big_ship_bricks[k].active) continue;
                int bx = alien_screen_x() + big_ship_bricks[k].rel_x;
                int by = (int)top + big_ship_bricks[k].rel_y;
                if (rects_overlap(b->x,b->y,3,2, bx,by,BRICK_W,BRICK_H)) {
                    big_ship_bricks[k].active = false;
                    b->active = false;
                    sound_effect_select();
                    add_explosion(bx+BRICK_W/2, by+BRICK_H/2, false);
                    score += 20;
                    break;
                }
            }
            if (b->active && check_alien_hit((float)b->x,(float)b->y,3,2)) b->active = false;
        }
    }
}

static void update_bombs(void) {
    if (bomb_cd > 0) bomb_cd--;
    float top = alien_top();

    for (int i=0;i<MAX_BOMBS;i++) {
        Bomb *bm = &bombs[i];
        if (!bm->active) continue;

        bm->x += bm->vx;
        bm->y += bm->vy;
        bm->vy += BOMB_GRAVITY;
        bm->vx *= 0.985f;

        if (bm->x > PLAY_X+PLAY_W || bm->y > PLAY_Y+PLAY_H) { bm->active=false; continue; }

        int32_t wx = scroll_px + ((int32_t)bm->x - PLAY_X);
        int fh = floor_h_for(wx, zone);
        if (bm->y >= PLAY_Y+PLAY_H-1-fh) {
            bm->active = false;
            sound_effect_explosion();
            add_explosion(bm->x, bm->y, false);
            continue;
        }

        for (int j=0;j<MAX_OBJECTS;j++) {
            if (!gobjs[j].active || gobjs[j].type==OBJ_METEOR) continue;
            int sx = PLAY_X + (int)(gobjs[j].world_x - scroll_px);
            int sy = (gobjs[j].type==OBJ_UFO)
                     ? (int)(gobjs[j].base_y + sinf(gobjs[j].t+gobjs[j].phase)*gobjs[j].amp)
                     : (int)(PLAY_Y+PLAY_H-fh-OBJ_H-gobjs[j].rel_y);
            if (rects_overlapf(bm->x,bm->y,4,4, (float)sx,(float)sy,OBJ_W,OBJ_H)) {
                bm->active = false;
                damage_object(j);
                break;
            }
        }
        if (!bm->active) continue;

        if (zone == ZONE_BIG_SHIP) {
            for (int k=0;k<num_bricks;k++) {
                if (!big_ship_bricks[k].active) continue;
                int bx = alien_screen_x() + big_ship_bricks[k].rel_x;
                int by = (int)top + big_ship_bricks[k].rel_y;
                if (rects_overlapf(bm->x,bm->y,4,4, (float)bx,(float)by,BRICK_W,BRICK_H)) {
                    big_ship_bricks[k].active = false;
                    bm->active = false;
                    sound_effect_select();
                    add_explosion(bx+BRICK_W/2, by+BRICK_H/2, false);
                    score += 20;
                    break;
                }
            }
            if (bm->active && check_alien_hit(bm->x,bm->y,4,4)) bm->active = false;
        }
    }
}

// ---------------------------------------------------------------------------
// Nave -- física, combustible, scroll, jefe final
// ---------------------------------------------------------------------------
static void ship_crash(void) {
    if (state != SCR_PLAYING) return; // evita doble muerte en el mismo tick
    sound_effect_explosion();
    add_explosion((float)(ship_x+SHIP_W/2), (float)(ship_y+SHIP_H/2), true);
    sound_effect_lose_point();
    lives--;
    if (fuel <= 0) fuel = FUEL_MAX;   // más permisivo tras perder nave (tomado del HTML)
    pause_cnt = TICKS_S;
    state = SCR_DEAD;
}

static void update_ship_thrust(bool held) {
    if (held) {
        ship_vx += THRUST_ACCEL;
        if (ship_vx > THRUST_VX_MAX) ship_vx = THRUST_VX_MAX;
    } else {
        ship_vx -= THRUST_ACCEL;
        if (ship_vx < -THRUST_VX_MAX) ship_vx = -THRUST_VX_MAX;
    }
    ship_x = clamp(ship_x + ship_vx, SHIP_X_MIN, SHIP_X_MAX);
    if (ship_x <= SHIP_X_MIN || ship_x >= SHIP_X_MAX) ship_vx = 0;
}

static void update_fuel(void) {
    if (--fuel_cd <= 0) {
        fuel--;
        fuel_cd = fuel_ticks_for_level();
        if (fuel <= 0) { fuel = 0; ship_crash(); }
    }
}

static void update_scroll(void) {
    if (scroll_locked) return;
    scroll_acc += scroll_spd;
    while (scroll_acc >= 64) { scroll_px++; scroll_acc -= 64; }

    int target_zone = zone_for_world(scroll_px);
    if (target_zone != zone && target_zone < NUM_ZONES) {
        zone = target_zone;
        if (zone == ZONE_BIG_SHIP) init_big_ship(scroll_px);
    }
}

static void update_big_ship(void) {
    if (zone != ZONE_BIG_SHIP) return;

    int base_sx = alien_screen_x();
    if (base_sx <= PLAY_X+PLAY_W-160) { scroll_locked = true; boss_engaged = true; }

    alien_t += 0.025f;
    float top = alien_top();
    int ax = alien_screen_x();
    int ay = (int)top + 3*BRICK_H;

    if (scroll_locked && --alien_shoot_cd <= 0) {
        alien_shoot_cd = 25 + (int)(rng_next()%20);
        enemy_fire((float)ax, (float)(ay+16));
    }

    if (ship_inv_ticks <= 0) {
        for (int k=0;k<num_bricks;k++) {
            if (!big_ship_bricks[k].active) continue;
            int bx = ax + big_ship_bricks[k].rel_x;
            int by = (int)top + big_ship_bricks[k].rel_y;
            if (rects_overlap(ship_x,ship_y,SHIP_W,SHIP_H, bx,by,BRICK_W,BRICK_H)) {
                ship_crash();
                return;
            }
        }
        if (rects_overlap(ship_x,ship_y,SHIP_W,SHIP_H, ax,ay,ALIEN_CORE_W,ALIEN_CORE_H)) {
            ship_crash();
        }
    }
}

static void update_ship_collision(void) {
    if (ship_inv_ticks > 0) return;
    int32_t wx = scroll_px + (ship_x+SHIP_W/2-PLAY_X);
    int fh = floor_h_for(wx, zone);
    int ch = ceil_h_for(wx, zone);
    int ground_top  = PLAY_Y+PLAY_H-1-fh;
    int ceil_bottom = PLAY_Y+1+ch;
    if (ship_y+SHIP_H >= ground_top) ship_crash();
    else if (ch > 0 && ship_y <= ceil_bottom) ship_crash();
}

// ---------------------------------------------------------------------------
// IA de la demo -- mira un poco por delante y centra la nave en el
// hueco libre (suelo/techo); además esquiva el objeto activo más
// cercano por delante (OVNI/meteorito/cohete) si invade su carril.
// ---------------------------------------------------------------------------
static void demo_ai(void) {
    int32_t look_x = scroll_px + (ship_x+SHIP_W/2-PLAY_X) + 50;
    int fh = floor_h_for(look_x, zone);
    int ch = ceil_h_for(look_x, zone);
    int ground_top  = PLAY_Y+PLAY_H-1-fh;
    int ceil_bottom = PLAY_Y+1+ch;
    int target = (ground_top+ceil_bottom)/2 - SHIP_H/2;

    for (int i=0;i<MAX_OBJECTS;i++) {
        if (!gobjs[i].active) continue;
        if (gobjs[i].type!=OBJ_METEOR && gobjs[i].type!=OBJ_UFO) continue;
        int32_t dx = gobjs[i].world_x - (scroll_px + (ship_x-PLAY_X));
        if (dx > 0 && dx < 60) {
            int oy = (int)gobjs[i].base_y;
            target = (oy < CY) ? SHIP_Y_MAX-10 : SHIP_Y_MIN+10;
        }
    }

    target = clamp(target, SHIP_Y_MIN, SHIP_Y_MAX);
    if (ship_y < target-2)      ship_y = clamp(ship_y+SHIP_AI_SPEED, SHIP_Y_MIN, SHIP_Y_MAX);
    else if (ship_y > target+2) ship_y = clamp(ship_y-SHIP_AI_SPEED, SHIP_Y_MIN, SHIP_Y_MAX);

    bool thrust_held = ((blink / 50) % 3) == 0;
    update_ship_thrust(thrust_held);

    if ((blink % 14) == 0) try_shoot();
    if ((blink % 45) == 0) try_bomb();
}

// ---------------------------------------------------------------------------
// Render -- terreno en bloques de COL_W con offset de scroll suave.
// ---------------------------------------------------------------------------
static int centered_x(const char *text, int scale) {
    int w = (int)st7789_text_width(text, (uint8_t)scale);
    int x = (TFT_WIDTH - w) / 2;
    return (x < 0) ? 0 : x;
}

static void draw_terrain(void) {
    int32_t start_col = scroll_px / COL_W;
    int offset_x = -(int)(scroll_px % COL_W);

    for (int i=0;i<NUM_COLS;i++) {
        int32_t world_x = (start_col + i) * COL_W;
        int screen_x = PLAY_X + offset_x + i*COL_W;
        int col_zone = zone_for_world(world_x);

        int fh = floor_h_for(world_x, col_zone);
        int ch = ceil_h_for(world_x, col_zone);
        int ground_top = PLAY_Y+PLAY_H-1-fh;

        if (ch > 0) renderer_fill_rect(screen_x, PLAY_Y+1, COL_W, ch, zone_colors[col_zone]);

        int sky_y0 = PLAY_Y+1+ch;
        int sky_h  = ground_top - sky_y0;
        if (sky_h > 0) renderer_fill_rect(screen_x, sky_y0, COL_W, sky_h, COLOR_BLACK);

        renderer_fill_rect(screen_x, ground_top, COL_W, fh, zone_colors[col_zone]);
    }
}

static void draw_objects(void) {
    for (int i=0;i<MAX_OBJECTS;i++) {
        GroundObj *o = &gobjs[i];
        if (!o->active) continue;
        int sx = PLAY_X + (int)(o->world_x - scroll_px);

        if (o->type == OBJ_UFO) {
            int fh = floor_h_for(o->world_x, zone);
            int ch = ceil_h_for(o->world_x, zone);
            int ground_top  = PLAY_Y+PLAY_H-1-fh;
            int ceil_bottom = PLAY_Y+1+ch;
            float ft = clampf(o->base_y + sinf(o->t+o->phase)*o->amp,
                               (float)(ceil_bottom+4), (float)(ground_top-OBJ_H-4));
            int top = (int)ft;
            sx += (int)(sinf((o->t+o->phase)*2.0f) * (o->amp*0.9f));
            if (sx+OBJ_W < PLAY_X || sx > PLAY_X+PLAY_W) continue;
            renderer_fill_rect(sx+3, top+6, OBJ_W-6, 6, COLOR_CYAN);
            renderer_fill_rect(sx+7, top,   OBJ_W-14, 6, COLOR_MAGENTA);
            continue;
        }
        if (o->type == OBJ_METEOR) {
            int top = (int)o->base_y;
            if (sx+OBJ_W < PLAY_X || sx > PLAY_X+PLAY_W) continue;
            renderer_fill_rect(sx+2, top+2, OBJ_W-4, OBJ_H-4, COLOR_RED);
            renderer_fill_rect(sx+6, top+4, 6, 4, COLOR_YELLOW);
            continue;
        }

        int fh = floor_h_for(o->world_x + OBJ_W/2, zone);
        int base_top = PLAY_Y + PLAY_H - fh - OBJ_H;
        int top = base_top - (int)o->rel_y;
        if (sx+OBJ_W < PLAY_X || sx > PLAY_X+PLAY_W) continue;

        if (o->type == OBJ_FUEL) {
            renderer_fill_rect(sx+2, top+3, OBJ_W-4, OBJ_H-5, COLOR_YELLOW);
            renderer_draw_text(sx+OBJ_W/2-3, top+3, "F", COLOR_BLACK, COLOR_YELLOW, 1);
        } else if (o->type == OBJ_ROCKET) {
            renderer_fill_rect(sx+OBJ_W/2-3, top+4, 6, OBJ_H-4, COLOR_RED);
            renderer_fill_rect(sx+OBJ_W/2-5, top+OBJ_H-4, 10, 4, COLOR_WHITE);
            if (o->launched)
                renderer_fill_rect(sx+OBJ_W/2-2, top+OBJ_H, 4, 6, (blink%2==0)?COLOR_YELLOW:COLOR_RED);
        } else if (o->type == OBJ_BASE) {
            renderer_fill_rect(sx+1, top+6, OBJ_W-2, OBJ_H-6, COLOR_CYAN);
            renderer_fill_rect(sx+5, top+2, OBJ_W-10, 4, COLOR_WHITE);
        }
    }
}

static void draw_big_ship(void) {
    if (zone != ZONE_BIG_SHIP) return;
    float top = alien_top();
    int ax = alien_screen_x();

    for (int k=0;k<num_bricks;k++) {
        if (!big_ship_bricks[k].active) continue;
        int bx = ax + big_ship_bricks[k].rel_x;
        int by = (int)top + big_ship_bricks[k].rel_y;
        renderer_fill_rect(bx, by, BRICK_W, BRICK_H, big_ship_bricks[k].color);
    }

    int ay = (int)top + 3*BRICK_H;
    uint16_t core_col = (blink%4<2) ? COLOR_RED : COLOR_MAGENTA;
    renderer_fill_rect(ax, ay, ALIEN_CORE_W, ALIEN_CORE_H, core_col);
    renderer_fill_rect(ax+12, ay+8, ALIEN_CORE_W-24, ALIEN_CORE_H-16, COLOR_YELLOW);
    char buf[4];
    snprintf(buf, sizeof(buf), "%d", alien_hp);
    renderer_draw_text(ax+ALIEN_CORE_W/2-3, ay+ALIEN_CORE_H/2-3, buf, COLOR_BLACK, COLOR_YELLOW, 1);
}

static void draw_enemy_bullets(void) {
    for (int i=0;i<MAX_ENEMY_BULLETS;i++)
        if (enemy_bullets[i].active)
            renderer_fill_rect((int)enemy_bullets[i].x, (int)enemy_bullets[i].y, 4, 3, COLOR_RED);
}

static void draw_bullets(void) {
    for (int i=0;i<MAX_BULLETS;i++)
        if (bullets[i].active) renderer_fill_rect(bullets[i].x, bullets[i].y, 3, 2, COLOR_WHITE);
}

static void draw_bombs(void) {
    for (int i=0;i<MAX_BOMBS;i++)
        if (bombs[i].active) renderer_fill_rect((int)bombs[i].x, (int)bombs[i].y, 3, 3, COLOR_YELLOW);
}

static void draw_ship(void) {
    int x0=ship_x, x1=ship_x+SHIP_W;
    int ytop=ship_y, ybot=ship_y+SHIP_H-1, ymid=ship_y+SHIP_H/2;
    bool hide = (ship_inv_ticks>0) && ((blink/4)%2==0);
    if (hide) return;

    if (ship_vx > 0) {   // llama de empuje (tomado del HTML)
        uint16_t flame = (blink%2==0) ? COLOR_RED : COLOR_YELLOW;
        int flen = 5 + (int)(rng_next()%4);
        renderer_fill_rect(x0-flen, ymid-2, flen, 4, flame);
    }

    // Casco relleno por filas en vez de contorno pintado línea a línea:
    // un triángulo (x0,ytop)-(x1,ymid)-(x0,ybot) resuelto con UNA
    // fill_rect por fila (~11 llamadas SPI) en vez de las ~50 que
    // salían de trazar 4 líneas píxel a píxel con draw_line().
    uint16_t col = COLOR_WHITE;
    int half = SHIP_H/2;
    for (int dy=0; dy<SHIP_H; dy++) {
        int y = ytop+dy;
        int t_num = (dy<=half) ? dy : (SHIP_H-1-dy);
        int t_den = (dy<=half) ? half : (SHIP_H-1-half);
        if (t_den <= 0) t_den = 1;
        int edge = x0 + (x1-x0)*t_num/t_den;
        int w = edge - x0 + 1;
        if (w < 1) w = 1;
        renderer_fill_rect(x0, y, w, 1, col);
    }
    // Pequeña aleta trasera (detalle cosmético, una sola llamada).
    renderer_fill_rect(x0-3, ymid-1, 3, 3, col);
}

static void draw_hud(void) {
    char buf[24];
    renderer_fill_rect(PLAY_X+1, PLAY_Y+1, PLAY_W-2, HUD_H, COLOR_BLACK);

    snprintf(buf, sizeof(buf), "%d", score);
    renderer_draw_text(PLAY_X+3, PLAY_Y+3, buf, COLOR_WHITE, COLOR_BLACK, 1);

    snprintf(buf, sizeof(buf), "V:%d", lives);
    renderer_draw_text(PLAY_X+90, PLAY_Y+3, buf, COLOR_WHITE, COLOR_BLACK, 1);

    int bar_w=50, bar_h=7;
    int bar_x = PLAY_X+PLAY_W-bar_w-4, bar_y = PLAY_Y+4;
    renderer_draw_text(bar_x-28, PLAY_Y+3, "FUEL", COLOR_WHITE, COLOR_BLACK, 1);
    renderer_fill_rect(bar_x, bar_y, bar_w, bar_h, COLOR_BLACK);
    int fill_w = bar_w*fuel/FUEL_MAX;
    uint16_t fcol = (fuel>50) ? COLOR_GREEN : (fuel>20 ? COLOR_YELLOW : COLOR_RED);
    if (fill_w > 0) renderer_fill_rect(bar_x, bar_y, fill_w, bar_h, fcol);

    if (zone == ZONE_BIG_SHIP) {
        int pbar_w=40, pbar_x=PLAY_X+130;
        renderer_fill_rect(pbar_x, bar_y, pbar_w, bar_h, COLOR_BLACK);
        int pfill_w = pbar_w*alien_hp/ALIEN_MAX_HP;
        if (pfill_w > 0) renderer_fill_rect(pbar_x, bar_y, pfill_w, bar_h, COLOR_RED);
        renderer_draw_text(pbar_x-24, PLAY_Y+3, "PWR", COLOR_WHITE, COLOR_BLACK, 1);
    } else {
        renderer_draw_text(centered_x(zone_short_names[zone],1), PLAY_Y+3,
                            zone_short_names[zone], COLOR_WHITE, COLOR_BLACK, 1);
    }
}

static void draw_playing_frame(void) {
    draw_terrain();
    draw_objects();
    draw_big_ship();
    draw_enemy_bullets();
    draw_bullets();
    draw_bombs();
    draw_particles();
    if (state == SCR_PLAYING || state == SCR_DEAD) draw_ship();
    draw_hud();
    renderer_flush();
}

// ---------------------------------------------------------------------------
// Pantallas estáticas
// ---------------------------------------------------------------------------
static void draw_field_static(void) {
    renderer_clear(COLOR_BLACK);
    renderer_fill_rect(PLAY_X, PLAY_Y,          PLAY_W, 1, COLOR_WHITE);
    renderer_fill_rect(PLAY_X, PLAY_Y+PLAY_H-1, PLAY_W, 1, COLOR_WHITE);
    renderer_flush();
}

static void draw_title_screen(void) {
    renderer_clear(COLOR_BLACK);

    // ==========================================
    // 1. TÍTULO (Parte superior)
    // ==========================================
    renderer_draw_text(centered_x("SCRAMBLE", 3), 15, "SCRAMBLE", COLOR_CYAN, COLOR_BLACK, 3);
    //renderer_draw_text(centered_x("RETRO EDITION", 1), 45, "RETRO EDITION", COLOR_YELLOW, COLOR_BLACK, 1);

    // ==========================================
    // 2. DIBUJILLO COLORIDO / RETRO (Zona central)
    // ==========================================
    int art_cx = SCREEN_W / 2;

    // --- NAVE (Izquierda) ---
    int ship_x = art_cx - 85;
    // Llama trasera
    renderer_fill_rect(ship_x - 8, 71, 8, 8, COLOR_RED);
    // Cuerpo triangular apuntando a la derecha
    renderer_fill_rect(ship_x, 68, 8, 14, COLOR_CYAN);
    renderer_fill_rect(ship_x + 8, 70, 8, 10, COLOR_CYAN);
    renderer_fill_rect(ship_x + 16, 72, 8, 6, COLOR_CYAN);
    renderer_fill_rect(ship_x + 24, 74, 6, 2, COLOR_CYAN);

    // --- COHETE (Centro, más grande y detallado) ---
    int rocket_x = art_cx - 12;
    int rocket_y = 56;
    // Punta/Ojiva superior del cohete
    renderer_fill_rect(rocket_x + 6, rocket_y, 4, 4, COLOR_RED);
    // Cuerpo alargado
    renderer_fill_rect(rocket_x + 4, rocket_y + 4, 8, 20, COLOR_WHITE);
    // Ventana del cohete
    renderer_fill_rect(rocket_x + 6, rocket_y + 8, 4, 4, COLOR_CYAN);
    // Aletas laterales (patas/estabilizadores)
    renderer_fill_rect(rocket_x, rocket_y + 18, 4, 6, COLOR_RED);
    renderer_fill_rect(rocket_x + 12, rocket_y + 18, 4, 6, COLOR_RED);
    // Escape inferior
    renderer_fill_rect(rocket_x + 5, rocket_y + 24, 6, 3, COLOR_YELLOW);

    // --- DEPÓSITO DE COMBUSTIBLE (Derecha) ---
    int fuel_x = art_cx + 55;
    int fuel_y = 58;
    // Base amarilla cuadrada con esquinas redondeadas
    renderer_fill_rect(fuel_x + 3, fuel_y, 18, 24, COLOR_YELLOW);
    renderer_fill_rect(fuel_x, fuel_y + 3, 24, 18, COLOR_YELLOW);
    renderer_fill_rect(fuel_x, fuel_y, 3, 3, COLOR_BLACK);
    renderer_fill_rect(fuel_x + 21, fuel_y, 3, 3, COLOR_BLACK);
    renderer_fill_rect(fuel_x, fuel_y + 21, 3, 3, COLOR_BLACK);
    renderer_fill_rect(fuel_x + 21, fuel_y + 21, 3, 3, COLOR_BLACK);
    // 'F' negra centrada
    renderer_draw_text(fuel_x + 8, fuel_y + 6, "F", COLOR_BLACK, COLOR_YELLOW, 1);

    // ==========================================
    // 3. TEXTO DE AYUDA (Parte inferior)
    // ==========================================
    int start_y = 110;
    int spacing = 16;

   renderer_draw_text(centered_x("ENC: ALTURA/EMPUJE",2), CY+10, "ENC: ALTURA/EMPUJE", COLOR_RED, COLOR_BLACK, 2);
    renderer_draw_text(centered_x("A-FIRE / B/BOMB",2), CY+34, "A-FIRE / B/BOMB", COLOR_RED, COLOR_BLACK, 2);

    // Mensaje para empezar parpadeante (en tamaño 2 para que destaque)
    if ((blink / 30) % 2 == 0) {
        renderer_draw_text(centered_x("PULSA PARA JUGAR", 2), 185, "PULSA PARA JUGAR", COLOR_GREEN, COLOR_BLACK, 2);
    }

    renderer_flush();
}


static void draw_ready_screen(void) {
    renderer_clear(COLOR_BLACK);
    char buf[24];
    snprintf(buf, sizeof(buf), "NIVEL %d", level);
    renderer_draw_text(centered_x(buf,3), CY-20, buf, COLOR_YELLOW, COLOR_BLACK, 3);
    renderer_draw_text(centered_x(zone_short_names[zone],2), CY+16,
                        zone_short_names[zone], COLOR_WHITE, COLOR_BLACK, 2);
    renderer_flush();
}

static void draw_victory_screen(void) {
    renderer_fill_rect(PLAY_X+20, CY-40, PLAY_W-40, 80, COLOR_BLACK);
    renderer_draw_text(centered_x("JEFE DESTRUIDO",2), CY-30, "JEFE DESTRUIDO", COLOR_GREEN, COLOR_BLACK, 2);
    renderer_draw_text(centered_x("SIGUIENTE NIVEL...",1), CY+5,
                        "SIGUIENTE NIVEL...", COLOR_WHITE, COLOR_BLACK, 1);
    renderer_flush();
}

static void draw_over_screen(void) {
    renderer_clear(COLOR_BLACK);
    renderer_draw_text(centered_x("GAME OVER",3), CY-30, "GAME OVER", COLOR_YELLOW, COLOR_BLACK, 3);
    char buf[24];
    snprintf(buf, sizeof(buf), "PUNTOS: %d", score);
    renderer_draw_text(centered_x(buf,2), CY+8, buf, COLOR_WHITE, COLOR_BLACK, 2);
    if (!demo)
        renderer_draw_text(centered_x("PULSA PARA CONTINUAR",1), CY+40,
                            "PULSA PARA CONTINUAR", COLOR_WHITE, COLOR_BLACK, 1);
    renderer_flush();
}

static void draw_scores_screen(void) {
    renderer_clear(COLOR_BLACK);
    highscores_draw(SCR_GAME_ID, "SCRAMBLE", 20);
    renderer_flush();
}

// ---------------------------------------------------------------------------
// Tick principal
// ---------------------------------------------------------------------------
static void scr_tick(void) {
    blink++;

    if (demo) {
        bool any = controls_menu_select()
                || controls_get_raw_delta(0) != 0
                || controls_button_down(BTN_J1_A)
                || controls_button_down(BTN_J1_B);
        if (any || ++demo_ticks >= TICKS_S * 40) { g_done = true; return; }
    }

    switch (state) {

    case SCR_TITLE:
        if (controls_menu_select()) {
            game_start();
            pause_cnt = TICKS_S*2;
            state = SCR_READY;
            draw_ready_screen();
            sound_stop_menu_music();
        }
        break;

    case SCR_READY:
        if (--pause_cnt <= 0) {
            state = SCR_PLAYING;
            draw_field_static();
        }
        break;

    case SCR_PLAYING:
        if (!demo) {
            int d = controls_get_raw_delta(0);
            ship_vel = enc_momentum(d, &ship_vel);
            ship_y = clamp(ship_y+ship_vel, SHIP_Y_MIN, SHIP_Y_MAX);
            update_ship_thrust(controls_button_down(BTN_ENC1_SW));
            if (controls_button_pressed(BTN_J1_A)) try_shoot();
            if (controls_button_pressed(BTN_J1_B)) try_bomb();
        } else {
            demo_ai();
        }

        if (ship_inv_ticks > 0) ship_inv_ticks--;

        update_scroll();
        maybe_spawn_objects();
        update_objects();
        update_big_ship();
        update_enemy_bullets();
        update_bullets();
        update_bombs();
        update_particles();
        update_fuel();
        update_ship_collision();

        if (state == SCR_PLAYING) draw_playing_frame();
        break;

    case SCR_DEAD:
        update_particles();
        draw_playing_frame();
        if (--pause_cnt <= 0) {
            if (lives <= 0) {
                sound_effect_game_over();
                draw_playing_frame();
                if (!demo && highscores_is_top(SCR_GAME_ID, score)) {
                    highscores_enter(SCR_GAME_ID, (uint32_t)score); // bloqueante
                }
                pause_cnt = 0;
                state = SCR_OVER;
                draw_over_screen();
            } else {
                ship_respawn();
                state = SCR_PLAYING;
            }
        }
        break;

    case SCR_VICTORY:
        update_particles();
        draw_playing_frame();
        draw_victory_screen();
        if (--pause_cnt <= 0) {
            next_level_setup();
            pause_cnt = TICKS_S*2;
            state = SCR_READY;
            draw_ready_screen();
        }
        break;

    case SCR_OVER:
        if (++pause_cnt > TICKS_S) {
            if (controls_menu_select() || pause_cnt > TICKS_S*8) {
                pause_cnt = 0;
                state = SCR_SCORES;
                draw_scores_screen();
            }
        }
        break;

    case SCR_SCORES:
        if (++pause_cnt > TICKS_S*8) g_done = true;
        if (controls_menu_select()) g_done = true;
        break;
    }
}

// ---------------------------------------------------------------------------
// API pública
// ---------------------------------------------------------------------------
void game_scramble_run(game_mode_t mode) {
    demo = (mode == GAME_MODE_DEMO);
    blink = 0;
    demo_ticks = 0;
    g_done = false;
    ship_vel = 0;

    init_zone_palette();

    if (demo) {
        game_start();
        draw_field_static();
        state = SCR_PLAYING;
    } else {
        state = SCR_TITLE;
        draw_title_screen();
        sound_start_menu_music();
    }

    // Contador de FPS real opcional -- compílalo con
    // -DSCRAMBLE_DEBUG_FPS para ver por stdio (UART/USB) el tiempo
    // medio por vuelta de bucle y los FPS resultantes cada segundo.
    // Así se mide el techo REAL en tu hardware (dominado casi
    // siempre por el volcado SPI de renderer_flush(), no por este
    // sleep_ms) en vez de estimarlo a ciegas.
#ifdef SCRAMBLE_DEBUG_FPS
    uint32_t fps_frames = 0;
    absolute_time_t fps_window_start = get_absolute_time();
#endif

    while (!g_done) {
        controls_update();
        scr_tick();
        sound_update();
        sleep_ms(1);

#ifdef SCRAMBLE_DEBUG_FPS
        fps_frames++;
        int64_t elapsed_us = absolute_time_diff_us(fps_window_start, get_absolute_time());
        if (elapsed_us >= 1000000) {
            printf("[scramble] %lu fps (%lld us/frame de media)\n",
                   (unsigned long)fps_frames,
                   (long long)(elapsed_us / fps_frames));
            fflush(stdout);
            fps_frames = 0;
            fps_window_start = get_absolute_time();
        }
#endif
    }

    highscores_flush();
}
