/**
 * scramble.c -- recreación simplificada del clásico de Konami (1981),
 * sustituye al juego que antes se llamaba "Defender" en el menú.
 * Mismos principios de adaptación que pong.c/space_invaders.c/
 * breakout.c/asteroids.c:
 *
 *  - Resolución: campo a pantalla casi completa (320x240 apaisada),
 *    literales fijos en vez de TFT_WIDTH/TFT_HEIGHT (ver el mismo
 *    comentario, más largo, en los otros archivos).
 *  - Terreno PROCEDURAL, no por tablas: en vez de guardar un mapa de
 *    altura fijo (memoria cara para un scroll "infinito"), la altura
 *    del suelo/techo en cada punto del mundo se calcula con una suma
 *    de ondas senoidales, reutilizando la misma SIN_TAB de 32 pasos
 *    que ya usa asteroids.c para su física. Así cada zona (montañas,
 *    combustible, cueva, base) es solo una fórmula distinta -- cero
 *    tablas de nivel que mantener.
 *  - Render de la franja de juego: a diferencia de Pong/Breakout
 *    (que solo redibujan lo que se ha movido), aquí TODO el fondo se
 *    mueve todos los ticks -- el redibujado incremental no aporta
 *    nada. Se opta por redibujar el terreno completo cada tick
 *    (columnas de COL_W px con renderer_fill_rect) y pintar encima
 *    nave/objetos/disparos SIN llevar posiciones "prev_*": el propio
 *    terreno ya tapa el fotograma anterior. Un único renderer_flush()
 *    al final del frame.
 *  - Objetos de tierra (combustible, torretas, base final) viven en
 *    un pool pequeño (MAX_OBJECTS) que se va poblando por distancia
 *    recorrida (next_spawn_world), no por nivel completo en memoria
 *    -- necesario para un scroll que no tiene fin fijo.
 *  - Combustible: se agota con el tiempo (contador de ticks, mismo
 *    esquema que shoot_cooldown en breakout.c); destruir un depósito
 *    de combustible lo rellena. Llegar a 0 = accidente, igual que
 *    perder la bola en Breakout.
 *  - Base final (zona BASE): al entrar en pantalla bloquea el scroll
 *    (scroll_locked) hasta destruirla -- mini "combate de jefe" muy
 *    simple, se desbloquea y completa la zona al derribarla.
 *  - Controles: encoder 1 (giro) = sube/baja la nave con inercia
 *    (mismo enc_momentum() que la pala de breakout.c, aquí en
 *    vertical); BTN_ENC1_SW (switch del mismo encoder, mantenido) =
 *    empuje horizontal -- la nave acelera hacia la derecha mientras
 *    se mantiene pulsado (mismo concepto que el thrust de
 *    asteroids.c), y al soltarlo decelera y es arrastrada poco a
 *    poco de vuelta al tope izquierdo; BTN_J1_A = disparo
 *    (ametralladora horizontal, también sirve contra misiles
 *    enemigos); BTN_J1_B = bomba (cae recta, para objetivos en
 *    tierra).
 *  - Sonido: mapeado a los mismos efectos que ya existen --
 *    sound_effect_shoot (disparo/bomba), sound_effect_explosion
 *    (impacto/destrucción), sound_effect_select (impacto parcial),
 *    sound_effect_lose_point (nave perdida, igual que "bola perdida"
 *    en breakout.c), sound_effect_success (zona completada),
 *    sound_effect_game_over.
 *  - Sin hs_input/SCR_ENTER_NAME: highscores_enter() bloqueante, como
 *    en los otros juegos.
 *  - Bucle propio: game_scramble_run(mode) con su propio bucle.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "scramble.h"
#include "renderer.h"
#include "controls.h"
#include "highscores.h"
#include "sound.h"

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
// Nave -- inercia vertical (encoder 1: girar sube/baja, mismo esquema
// que la pala de breakout.c) + empuje horizontal con el switch del
// encoder 1 (BTN_ENC1_SW), igual concepto que el thrust de
// asteroids.c (SHIP_THRUST + fricción) pero en 1D y acotado a un
// rango de pantalla en vez de espacio abierto: mientras se mantiene
// pulsado el switch, la nave acelera hacia la derecha (se "adelanta");
// al soltarlo, decelera y luego es arrastrada poco a poco de vuelta
// al tope izquierdo (no salta de golpe).
// ---------------------------------------------------------------------------
#define SHIP_W   14
#define SHIP_H    8
#define SHIP_X_MIN (PLAY_X + 20)    // tope izquierdo (reposo)
#define SHIP_X_MAX (PLAY_X + 110)   // máximo avance con el empuje

#define HUD_H    16   // franja superior reservada para el HUD
#define SHIP_Y_MIN (PLAY_Y + HUD_H + 2)
#define SHIP_Y_MAX (PLAY_Y + PLAY_H - 2 - SHIP_H)

#define SHIP_ACCEL      2
#define SHIP_VEL_MAX    8
#define SHIP_DECAY_NUM  6
#define SHIP_DECAY_DEN 10
#define SHIP_AI_SPEED   2

#define THRUST_ACCEL   1   // aceleración del empuje horizontal, por tick
#define THRUST_VX_MAX  4   // velocidad horizontal máxima (avance o retorno)

#define RESPAWN_INV  (TICKS_S * 2)

static int ship_vel = 0;
static int ship_x  = SHIP_X_MIN;
static int ship_vx = 0;

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
// Terreno -- procedural con tabla seno de 32 pasos (misma idea que
// asteroids.c para su física de ángulos, aquí para relieve).
// ---------------------------------------------------------------------------
#define FP 256
static const int16_t SIN_TAB[32] = {
      0,  50,  98, 142, 181, 213, 237, 251,
    256, 251, 237, 213, 181, 142,  98,  50,
      0, -50, -98,-142,-181,-213,-237,-251,
   -256,-251,-237,-213,-181,-142, -98, -50
};

#define ZONE_MOUNTAINS 0
#define ZONE_FUEL      1
#define ZONE_CAVE      2
#define ZONE_BASE      3
#define NUM_ZONES      4

#define GROUND_MIN_H  20
#define GROUND_MAX_H 150
#define CEIL_MAX_H    90
#define CORRIDOR_MIN  70   // hueco vertical mínimo garantizado en la cueva

#define COL_W    4
#define NUM_COLS (PLAY_W / COL_W)

static int32_t wave(int32_t world_x, int wavelength, int amplitude, int phase) {
    int32_t idx = ((world_x + phase) * 32) / wavelength;
    return (amplitude * SIN_TAB[idx & 31]) / FP;
}

static int floor_h_for(int32_t world_x, int zone) {
    int h;
    switch (zone) {
        case ZONE_MOUNTAINS: h = 60 + wave(world_x,260,55,0) + wave(world_x,95,22,777); break;
        case ZONE_FUEL:      h = 45 + wave(world_x,300,10,0); break;
        case ZONE_CAVE:      h = 40 + wave(world_x,210,18,150); break;
        case ZONE_BASE:      h = 42 + wave(world_x,260,8,0); break;
        default:             h = GROUND_MIN_H; break;
    }
    if (h < GROUND_MIN_H) h = GROUND_MIN_H;
    if (h > GROUND_MAX_H) h = GROUND_MAX_H;
    return h;
}

static int ceil_h_for(int32_t world_x, int zone) {
    if (zone != ZONE_CAVE) return 0;
    int h = 45 + wave(world_x,230,22,900);
    if (h < 0) h = 0;
    if (h > CEIL_MAX_H) h = CEIL_MAX_H;
    int fh = floor_h_for(world_x, zone);
    int gap = PLAY_H - fh - h;
    if (gap < CORRIDOR_MIN) h = PLAY_H - fh - CORRIDOR_MIN;
    if (h < 0) h = 0;
    return h;
}

static uint16_t terrain_color(int zone, bool ceiling) {
    if (ceiling) return COLOR_CYAN;
    switch (zone) {
        case ZONE_CAVE: return COLOR_CYAN;
        case ZONE_BASE: return COLOR_MAGENTA;
        default:        return COLOR_GREEN;
    }
}

// ---------------------------------------------------------------------------
// Objetos de tierra -- combustible, torretas, base final. Pool
// pequeño, se rellena por distancia recorrida (scroll "infinito").
// ---------------------------------------------------------------------------
#define OBJ_NONE   0
#define OBJ_FUEL   1
#define OBJ_TURRET 2
#define OBJ_HQ     3

#define OBJ_W 12
#define OBJ_H 10
#define MAX_OBJECTS 6

#define FUEL_SPACING   220
#define TURRET_SPACING 260
#define TURRET_FIRE_CD_BASE 90
#define HQ_FIRE_CD_BASE     50
#define HQ_HP  3

#define BASE_HQ_DIST 500
#define ZONE_LENGTH 1800

typedef struct {
    int32_t world_x;
    int     hp;
    uint8_t type;
    bool    active;
    int     fire_cd;
} GroundObj;

// ---------------------------------------------------------------------------
// Disparos -- ametralladora (horizontal), bombas (caen), proyectiles
// enemigos de torretas/base (suben).
// ---------------------------------------------------------------------------
#define MAX_BULLETS 4
#define BULLET_SPD  6
#define SHOOT_COOLDOWN 10

#define MAX_BOMBS 3
#define BOMB_FALL_SPD 3
#define BOMB_COOLDOWN 18

#define MAX_ENEMY_PROJ 6
#define ENEMY_PROJ_SPD 3

typedef struct { int x, y; bool active; } Bullet;
typedef struct { int x, y; bool active; } Bomb;
typedef struct { int x, y; bool active; } EnemyProj;

// ---------------------------------------------------------------------------
// Combustible y velocidad de scroll
// ---------------------------------------------------------------------------
#define FUEL_MAX    100
#define FUEL_REFILL  40

#define SCROLL_SPD0     80
#define SCROLL_SPD_INC  16
#define SCROLL_SPD_MAX 220

// ---------------------------------------------------------------------------
// Puntuación
// ---------------------------------------------------------------------------
#define SCR_FUEL_PTS         50
#define SCR_TURRET_PTS      100
#define SCR_ENEMYPROJ_PTS    20
#define SCR_HQ_HIT_PTS      100
#define SCR_HQ_DESTROY_BONUS 1000
#define SCR_ZONE_BONUS       300

// ---------------------------------------------------------------------------
// Estado
// ---------------------------------------------------------------------------
typedef enum {
    SCR_TITLE, SCR_READY, SCR_PLAYING, SCR_DEAD,
    SCR_LEVELUP, SCR_OVER, SCR_SCORES,
} ScrState;

static ScrState state;
static bool demo, g_done;
static bool scroll_locked, hq_spawned, hq_engaged, zone_complete_pending;
static int  blink, demo_ticks, pause_cnt;
static int  lives, level, score, fuel, fuel_cd;
static int  zone;
static int32_t scroll_px, next_spawn_world;
static int  scroll_acc, scroll_spd;
static int  ship_y, ship_inv_ticks;
static int  shoot_cd, bomb_cd;

static GroundObj  gobjs[MAX_OBJECTS];
static Bullet     bullets[MAX_BULLETS];
static Bomb       bombs[MAX_BOMBS];
static EnemyProj  enemy_proj[MAX_ENEMY_PROJ];

static uint32_t rng_state_v = 12345;
static uint32_t rng_next(void) {
    rng_state_v ^= rng_state_v << 13;
    rng_state_v ^= rng_state_v >> 17;
    rng_state_v ^= rng_state_v << 5;
    return rng_state_v;
}

static int  clamp(int v, int lo, int hi) { return v<lo?lo:v>hi?hi:v; }
static bool rects_overlap(int ax,int ay,int aw,int ah,int bx,int by,int bw,int bh) {
    return (ax < bx+bw && ax+aw > bx && ay < by+bh && ay+ah > by);
}

static void draw_line(int x0,int y0,int x1,int y1,uint16_t color) {
    int dx = abs(x1-x0), sx = x0<x1?1:-1;
    int dy = -abs(y1-y0), sy = y0<y1?1:-1;
    int err = dx+dy, e2;
    for (;;) {
        renderer_fill_rect(x0,y0,2,2,color);
        if (x0==x1 && y0==y1) break;
        e2 = 2*err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// Forward declarations (evitan reordenar todo el archivo por
// dependencias cruzadas entre update_objects/update_bullets/etc.)
static void spawn_enemy_proj(int x, int y);
static void ship_crash(void);
static void try_shoot(void);
static void try_bomb(void);
static void damage_object(int idx);
static void draw_field_static(void);
static void draw_ready_screen(void);

// ---------------------------------------------------------------------------
// Fuel / dificultad
// ---------------------------------------------------------------------------
static int fuel_ticks_for_level(void) {
    int t = 60 - (level-1)*3;
    return t < 20 ? 20 : t;
}

// ---------------------------------------------------------------------------
// Gestión de zona / partida
// ---------------------------------------------------------------------------
static void ship_respawn(void) {
    ship_y = CY;
    ship_vel = 0;
    ship_x = SHIP_X_MIN;
    ship_vx = 0;
    ship_inv_ticks = RESPAWN_INV;
}

static void zone_start(void) {
    scroll_px = 0;
    scroll_acc = 0;
    scroll_spd = SCROLL_SPD0 + (level-1)*SCROLL_SPD_INC;
    if (scroll_spd > SCROLL_SPD_MAX) scroll_spd = SCROLL_SPD_MAX;
    scroll_locked = false;
    hq_spawned = false;
    hq_engaged = false;
    zone_complete_pending = false;
    next_spawn_world = 150;
    for (int i=0;i<MAX_OBJECTS;i++)    gobjs[i].active = false;
    for (int i=0;i<MAX_BULLETS;i++)    bullets[i].active = false;
    for (int i=0;i<MAX_BOMBS;i++)      bombs[i].active = false;
    for (int i=0;i<MAX_ENEMY_PROJ;i++) enemy_proj[i].active = false;
    shoot_cd = 0;
    bomb_cd = 0;
    ship_respawn();
}

static void game_start(void) {
    lives = 3; level = 1; zone = ZONE_MOUNTAINS; score = 0;
    fuel = FUEL_MAX; fuel_cd = fuel_ticks_for_level();
    rng_state_v = (uint32_t)time_us_32();
    zone_start();
}

// ---------------------------------------------------------------------------
// Objetos de tierra
// ---------------------------------------------------------------------------
static void spawn_obj(uint8_t type, int32_t world_x, int hp) {
    for (int i=0;i<MAX_OBJECTS;i++) {
        if (gobjs[i].active) continue;
        gobjs[i].active = true;
        gobjs[i].type = type;
        gobjs[i].world_x = world_x;
        gobjs[i].hp = hp;
        gobjs[i].fire_cd = TURRET_FIRE_CD_BASE/2 + (int)(rng_next() % TURRET_FIRE_CD_BASE);
        return;
    }
}

static void maybe_spawn_objects(void) {
    if (zone == ZONE_BASE) {
        if (!hq_spawned && scroll_px >= BASE_HQ_DIST) {
            spawn_obj(OBJ_HQ, scroll_px + PLAY_W + 60, HQ_HP);
            hq_spawned = true;
        }
        return;
    }
    while (next_spawn_world <= scroll_px + PLAY_W + 20) {
        uint8_t type = (zone == ZONE_FUEL) ? OBJ_FUEL : OBJ_TURRET;
        spawn_obj(type, next_spawn_world, 1);
        int spacing = (zone == ZONE_FUEL) ? FUEL_SPACING : TURRET_SPACING;
        next_spawn_world += spacing + (int)(rng_next() % 40);
    }
}

static void damage_object(int idx) {
    gobjs[idx].hp--;
    if (gobjs[idx].hp <= 0) {
        uint8_t type = gobjs[idx].type;
        gobjs[idx].active = false;
        sound_effect_explosion();
        int pts = (type==OBJ_FUEL) ? SCR_FUEL_PTS : (type==OBJ_HQ) ? SCR_HQ_HIT_PTS : SCR_TURRET_PTS;
        score += pts * level;
        if (type == OBJ_FUEL) {
            fuel += FUEL_REFILL;
            if (fuel > FUEL_MAX) fuel = FUEL_MAX;
        }
        if (type == OBJ_HQ) {
            score += SCR_HQ_DESTROY_BONUS * level;
            scroll_locked = false;
            zone_complete_pending = true;
        }
    } else {
        sound_effect_select();
        if (gobjs[idx].type == OBJ_HQ) score += SCR_HQ_HIT_PTS * level;
    }
}

static void update_objects(void) {
    for (int i=0;i<MAX_OBJECTS;i++) {
        if (!gobjs[i].active) continue;
        int sx = PLAY_X + (int)(gobjs[i].world_x - scroll_px);

        if (gobjs[i].type == OBJ_HQ) {
            if (!hq_engaged && sx <= PLAY_X+PLAY_W-120) { hq_engaged = true; scroll_locked = true; }
        } else if (sx + OBJ_W < PLAY_X) {
            gobjs[i].active = false;
            continue;
        }

        if (ship_inv_ticks <= 0) {
            int fh = floor_h_for(gobjs[i].world_x, zone);
            int top = PLAY_Y+PLAY_H-1-fh-OBJ_H;
            if (rects_overlap(ship_x,ship_y,SHIP_W,SHIP_H, sx,top,OBJ_W,OBJ_H)) {
                ship_crash();
            }
        }

        if (gobjs[i].type == OBJ_TURRET || gobjs[i].type == OBJ_HQ) {
            bool can_fire = (gobjs[i].type == OBJ_TURRET)
                             ? (sx >= PLAY_X && sx <= PLAY_X+PLAY_W)
                             : hq_engaged;
            if (can_fire && --gobjs[i].fire_cd <= 0) {
                int fh = floor_h_for(gobjs[i].world_x, zone);
                spawn_enemy_proj(sx+OBJ_W/2, PLAY_Y+PLAY_H-1-fh-OBJ_H);
                gobjs[i].fire_cd = (gobjs[i].type==OBJ_HQ ? HQ_FIRE_CD_BASE : TURRET_FIRE_CD_BASE)
                                    + (int)(rng_next() % 30);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Disparos del jugador y proyectiles enemigos
// ---------------------------------------------------------------------------
static void spawn_enemy_proj(int x, int y) {
    for (int i=0;i<MAX_ENEMY_PROJ;i++) {
        if (enemy_proj[i].active) continue;
        enemy_proj[i].active = true;
        enemy_proj[i].x = x; enemy_proj[i].y = y;
        return;
    }
}

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
    for (int i=0;i<MAX_BOMBS;i++) {
        if (bombs[i].active) continue;
        bombs[i].active = true;
        bombs[i].x = ship_x+SHIP_W/2-1;
        bombs[i].y = ship_y+SHIP_H;
        bomb_cd = BOMB_COOLDOWN;
        sound_effect_shoot();
        return;
    }
}

static void update_bullets(void) {
    if (shoot_cd > 0) shoot_cd--;
    for (int i=0;i<MAX_BULLETS;i++) {
        if (!bullets[i].active) continue;
        bullets[i].x += BULLET_SPD;
        if (bullets[i].x > PLAY_X+PLAY_W) { bullets[i].active=false; continue; }

        for (int j=0;j<MAX_OBJECTS;j++) {
            if (!gobjs[j].active) continue;
            int sx = PLAY_X + (int)(gobjs[j].world_x - scroll_px);
            int fh = floor_h_for(gobjs[j].world_x, zone);
            int top = PLAY_Y+PLAY_H-1-fh-OBJ_H;
            if (rects_overlap(bullets[i].x,bullets[i].y,3,2, sx,top,OBJ_W,OBJ_H)) {
                bullets[i].active = false;
                damage_object(j);
                break;
            }
        }
        if (!bullets[i].active) continue;

        for (int k=0;k<MAX_ENEMY_PROJ;k++) {
            if (!enemy_proj[k].active) continue;
            if (rects_overlap(bullets[i].x,bullets[i].y,3,2, enemy_proj[k].x,enemy_proj[k].y,2,4)) {
                bullets[i].active = false;
                enemy_proj[k].active = false;
                score += SCR_ENEMYPROJ_PTS * level;
                sound_effect_explosion();
                break;
            }
        }
    }
}

static void update_bombs(void) {
    if (bomb_cd > 0) bomb_cd--;
    for (int i=0;i<MAX_BOMBS;i++) {
        if (!bombs[i].active) continue;
        bombs[i].y += BOMB_FALL_SPD;
        if (bombs[i].y > PLAY_Y+PLAY_H) { bombs[i].active=false; continue; }

        for (int j=0;j<MAX_OBJECTS;j++) {
            if (!gobjs[j].active) continue;
            int sx = PLAY_X + (int)(gobjs[j].world_x - scroll_px);
            int fh = floor_h_for(gobjs[j].world_x, zone);
            int top = PLAY_Y+PLAY_H-1-fh-OBJ_H;
            if (rects_overlap(bombs[i].x,bombs[i].y,3,3, sx,top,OBJ_W,OBJ_H)) {
                bombs[i].active = false;
                damage_object(j);
                break;
            }
        }
    }
}

static void update_enemy_proj(void) {
    for (int i=0;i<MAX_ENEMY_PROJ;i++) {
        if (!enemy_proj[i].active) continue;
        enemy_proj[i].y -= ENEMY_PROJ_SPD;
        if (enemy_proj[i].y < PLAY_Y) { enemy_proj[i].active=false; continue; }
        if (ship_inv_ticks<=0 &&
            rects_overlap(enemy_proj[i].x,enemy_proj[i].y,2,4, ship_x,ship_y,SHIP_W,SHIP_H)) {
            enemy_proj[i].active = false;
            ship_crash();
        }
    }
}

// ---------------------------------------------------------------------------
// Nave -- física, combustible, scroll
// ---------------------------------------------------------------------------
static void ship_crash(void) {
    if (state != SCR_PLAYING) return; // evita doble muerte en el mismo tick
    sound_effect_explosion();
    sound_effect_lose_point();
    lives--;
    pause_cnt = TICKS_S;
    state = SCR_DEAD;
}

static void update_ship_thrust(bool held) {
    if (held) {
        ship_vx += THRUST_ACCEL;
        if (ship_vx > THRUST_VX_MAX) ship_vx = THRUST_VX_MAX;
    } else {
        ship_vx -= THRUST_ACCEL;   // decelera y, pasado cero, tira hacia atrás
        if (ship_vx < -THRUST_VX_MAX) ship_vx = -THRUST_VX_MAX;
    }
    ship_x = clamp(ship_x + ship_vx, SHIP_X_MIN, SHIP_X_MAX);
    if (ship_x <= SHIP_X_MIN || ship_x >= SHIP_X_MAX) ship_vx = 0; // no acumular velocidad en el tope
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
    if (zone != ZONE_BASE && scroll_px >= ZONE_LENGTH) zone_complete_pending = true;
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
// hueco libre (suelo/techo), disparando y bombardeando de vez en
// cuando para lucirse.
// ---------------------------------------------------------------------------
static void demo_ai(void) {
    int32_t look_x = scroll_px + (ship_x+SHIP_W/2-PLAY_X) + 50;
    int fh = floor_h_for(look_x, zone);
    int ch = ceil_h_for(look_x, zone);
    int ground_top  = PLAY_Y+PLAY_H-1-fh;
    int ceil_bottom = PLAY_Y+1+ch;
    int target = clamp((ground_top+ceil_bottom)/2 - SHIP_H/2, SHIP_Y_MIN, SHIP_Y_MAX);

    if (ship_y < target-2)      ship_y = clamp(ship_y+SHIP_AI_SPEED, SHIP_Y_MIN, SHIP_Y_MAX);
    else if (ship_y > target+2) ship_y = clamp(ship_y-SHIP_AI_SPEED, SHIP_Y_MIN, SHIP_Y_MAX);

    bool thrust_held = ((blink / 50) % 3) == 0; // patrón simple para lucir el empuje en el demo
    update_ship_thrust(thrust_held);

    if ((blink % 14) == 0) try_shoot();
    if ((blink % 37) == 0) try_bomb();
}

// ---------------------------------------------------------------------------
// Render -- terreno completo cada tick (ver cabecera del archivo),
// sin llevar posiciones "prev_*" para nave/objetos/disparos.
// ---------------------------------------------------------------------------
static int centered_x(const char *text, int scale) {
    int w = (int)st7789_text_width(text, (uint8_t)scale);
    int x = (TFT_WIDTH - w) / 2;
    return (x < 0) ? 0 : x;
}

static void draw_terrain(void) {
    for (int i=0;i<NUM_COLS;i++) {
        int32_t world_x = scroll_px + (int32_t)i*COL_W;
        int screen_x = PLAY_X + i*COL_W;
        int fh = floor_h_for(world_x, zone);
        int ch = ceil_h_for(world_x, zone);
        int ground_top = PLAY_Y+PLAY_H-1-fh;

        if (ch > 0) renderer_fill_rect(screen_x, PLAY_Y+1, COL_W, ch, terrain_color(zone, true));

        int sky_y0 = PLAY_Y+1+ch;
        int sky_h  = ground_top - sky_y0;
        if (sky_h > 0) renderer_fill_rect(screen_x, sky_y0, COL_W, sky_h, COLOR_BLACK);

        renderer_fill_rect(screen_x, ground_top, COL_W, fh, terrain_color(zone, false));
    }
}

static void draw_objects(void) {
    for (int i=0;i<MAX_OBJECTS;i++) {
        if (!gobjs[i].active) continue;
        int sx = PLAY_X + (int)(gobjs[i].world_x - scroll_px);
        if (sx+OBJ_W < PLAY_X || sx > PLAY_X+PLAY_W) continue;
        int fh = floor_h_for(gobjs[i].world_x, zone);
        int top = PLAY_Y+PLAY_H-1-fh-OBJ_H;
        uint16_t col = (gobjs[i].type==OBJ_FUEL) ? COLOR_YELLOW
                     : (gobjs[i].type==OBJ_HQ)   ? COLOR_MAGENTA
                                                  : COLOR_RED;
        const char *lab = (gobjs[i].type==OBJ_FUEL) ? "F"
                         : (gobjs[i].type==OBJ_HQ)   ? "H" : "T";
        renderer_fill_rect(sx, top, OBJ_W, OBJ_H, col);
        renderer_draw_text(sx+2, top, lab, COLOR_BLACK, col, 1);
    }
}

static void draw_bullets(void) {
    for (int i=0;i<MAX_BULLETS;i++)
        if (bullets[i].active) renderer_fill_rect(bullets[i].x, bullets[i].y, 3, 2, COLOR_WHITE);
}

static void draw_bombs(void) {
    for (int i=0;i<MAX_BOMBS;i++)
        if (bombs[i].active) renderer_fill_rect(bombs[i].x, bombs[i].y, 3, 3, COLOR_YELLOW);
}

static void draw_enemy_projectiles(void) {
    for (int i=0;i<MAX_ENEMY_PROJ;i++)
        if (enemy_proj[i].active) renderer_fill_rect(enemy_proj[i].x, enemy_proj[i].y, 2, 4, COLOR_RED);
}

static void draw_ship(void) {
    int x0=ship_x, x1=ship_x+SHIP_W;
    int ytop=ship_y, ybot=ship_y+SHIP_H, ymid=ship_y+SHIP_H/2;
    bool hide = (ship_inv_ticks>0) && ((blink/4)%2==0);
    uint16_t col = hide ? COLOR_BLACK : COLOR_WHITE;
    draw_line(x0,ytop,x1,ymid,col);
    draw_line(x0,ybot,x1,ymid,col);
    draw_line(x0,ytop,x0,ybot,col);
    draw_line(x0-4,ymid-3,x0,ytop+2,col);
    draw_line(x0-4,ymid+3,x0,ybot-2,col);
}

static const char *zone_short_names[NUM_ZONES] = { "MONTANAS", "COMBUSTIBLE", "CUEVA", "BASE" };

static void draw_hud(void) {
    char buf[24];
    renderer_fill_rect(PLAY_X+1, PLAY_Y+1, PLAY_W-2, HUD_H, COLOR_BLACK);

    snprintf(buf, sizeof(buf), "%d", score);
    renderer_draw_text(PLAY_X+3, PLAY_Y+3, buf, COLOR_WHITE, COLOR_BLACK, 1);

    snprintf(buf, sizeof(buf), "V:%d", lives);
    renderer_draw_text(PLAY_X+90, PLAY_Y+3, buf, COLOR_WHITE, COLOR_BLACK, 1);

    renderer_draw_text(centered_x(zone_short_names[zone],1), PLAY_Y+3,
                        zone_short_names[zone], COLOR_WHITE, COLOR_BLACK, 1);

    int bar_w=60, bar_h=8;
    int bar_x = PLAY_X+PLAY_W-bar_w-4, bar_y = PLAY_Y+4;
    renderer_draw_text(bar_x-26, PLAY_Y+3, "FUEL", COLOR_WHITE, COLOR_BLACK, 1);
    renderer_fill_rect(bar_x, bar_y, bar_w, bar_h, COLOR_BLACK);
    int fill_w = bar_w*fuel/FUEL_MAX;
    uint16_t fcol = (fuel>50) ? COLOR_GREEN : (fuel>20 ? COLOR_YELLOW : COLOR_RED);
    if (fill_w > 0) renderer_fill_rect(bar_x, bar_y, fill_w, bar_h, fcol);
}

static void draw_playing_frame(void) {
    draw_terrain();
    draw_objects();
    draw_bullets();
    draw_bombs();
    draw_enemy_projectiles();
    draw_ship();
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
    renderer_draw_text(centered_x("SCRAMBLE",3), CY-70, "SCRAMBLE", COLOR_CYAN, COLOR_BLACK, 3);
    renderer_draw_text(centered_x("PULSA PARA JUGAR",2), CY-20, "PULSA PARA JUGAR", COLOR_WHITE, COLOR_BLACK, 2);
    renderer_draw_text(centered_x("GIRA: SUBIR/BAJAR",1), CY+16,
                        "GIRA: SUBIR/BAJAR", COLOR_WHITE, COLOR_BLACK, 1);
    renderer_draw_text(centered_x("A: DISPARO   B: BOMBA",1), CY+32,
                        "A: DISPARO   B: BOMBA", COLOR_WHITE, COLOR_BLACK, 1);
    renderer_draw_text(centered_x("CUIDADO CON EL COMBUSTIBLE",1), CY+48,
                        "CUIDADO CON EL COMBUSTIBLE", COLOR_YELLOW, COLOR_BLACK, 1);
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
        update_bullets();
        update_bombs();
        update_enemy_proj();
        update_fuel();
        update_ship_collision();

        if (zone_complete_pending && state == SCR_PLAYING) {
            sound_effect_success();
            score += SCR_ZONE_BONUS * level;
            zone_complete_pending = false;
            pause_cnt = TICKS_S*2;
            state = SCR_LEVELUP;
            break;
        }

        if (state == SCR_PLAYING) draw_playing_frame();
        break;

    case SCR_DEAD:
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

    case SCR_LEVELUP:
        if (--pause_cnt <= 0) {
            level++;
            zone = (zone+1) % NUM_ZONES;
            zone_start();
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

    if (demo) {
        lives = 3; level = 1; zone = ZONE_MOUNTAINS; score = 0;
        fuel = FUEL_MAX; fuel_cd = fuel_ticks_for_level();
        rng_state_v = (uint32_t)time_us_32();
        zone_start();
        draw_field_static();
        state = SCR_PLAYING;
    } else {
        state = SCR_TITLE;
        draw_title_screen();
        sound_start_menu_music();
    }

    while (!g_done) {
        controls_update();
        scr_tick();
        sound_update();
        sleep_ms(8);
    }

    highscores_flush();
}
