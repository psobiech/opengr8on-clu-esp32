/**
 * @file ms_vmod.h
 * @brief MySensors sensors as emulated TF-Bus modules (component-private).
 *
 * Presented sensors are bound to channels ("slots") of emulated modules. Each module belongs to one
 * node: a node's sensors of a kind fill its own modules in child-id order (DOUT 1 = its lowest
 * output child, ...). Modules are created on demand, when a node needs one more (an emptied one is
 * reused first), up to ms_vmod_cfg_t::max; OM sees a new one after its next discovery.
 * Bindings and module serials are kept in NVS, so OM sees the same module and channel for a
 * sensor after every reboot. The modules answer the bus master through a
 * tfbus_uart local endpoint (tfbus_vmod.h).
 *
 * Module types and what they take:
 *   IO_MODULE_DIN_8 (8 DIN + 8 DOUT)
 *     DIN  <- S_DOOR, S_MOTION, S_SMOKE, S_WATER_LEAK, S_SOUND, S_VIBRATION, S_MOISTURE (V_TRIPPED)
 *     DOUT <- S_BINARY / S_LIGHT, S_SPRINKLER (V_STATUS), S_LOCK (V_LOCK_STATUS)
 * Planned: DIMMER (S_DIMMER), ROLLER_SHUTTER (S_COVER), LED_RGBW (S_RGB_LIGHT / S_RGBW_LIGHT): a
 * new slot kind, a rule per S_ type and a module type with its tfbus_vmod_ops_t.
 *
 * Threading: ms_vmod_present / ms_vmod_value from the gateway task, ms_vmod_frame from the bus
 * task, the list / unmap calls from Lua; one mutex. The send callback is never called with it held.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Max emulated modules (all types). */
#define MS_VMOD_MAX 8
/** @brief Slots per module (IO_MODULE_DIN_8: 8 DIN + 8 DOUT). */
#define MS_VMOD_SLOTS 16

/** @brief Slot kinds. */
typedef enum {
    MS_SLOT_DIN = 0,   /**< Binary input. */
    MS_SLOT_DOUT = 1,  /**< Binary output. */
    MS_SLOT_KINDS
} ms_slot_kind_t;

/** @brief Configuration. */
typedef struct {
    int max;   /**< Max emulated modules created on demand (0..MS_VMOD_MAX; 0 = none). */
    /** Send a message to a node (C_SET of an output). */
    bool (*send)(uint8_t node, uint8_t child, uint8_t cmd, uint8_t vtype, const char *payload);
} ms_vmod_cfg_t;

/** @brief A bound slot (ms_vmod_list()). */
typedef struct {
    int module;              /**< 0-based module index. */
    uint32_t sn;             /**< Module serial. */
    uint8_t hw_type;         /**< Module hwType. */
    ms_slot_kind_t kind;
    int channel;             /**< 0-based channel within the kind. */
    uint8_t node, child, vtype;
} ms_vmod_slot_t;

/**
 * @brief Load the modules created so far and their bindings (NVS), install the bus endpoint.
 * @param cfg Configuration (copied).
 * @return Number of emulated modules now (more may follow as sensors present themselves).
 */
int ms_vmod_init(const ms_vmod_cfg_t *cfg);

/**
 * @brief A sensor was presented: bind it to a free slot if its S_ type has a module channel.
 * @param desc Presentation description (may be NULL); see the INPUT workaround in ms_vmod.c.
 */
void ms_vmod_present(uint8_t node, uint8_t child, uint8_t stype, const char *desc);

/** @brief A value of a node (C_SET from it, or sent to it): update bound inputs / outputs. */
void ms_vmod_value(uint8_t node, uint8_t child, uint8_t vtype, const char *payload);

/** @brief Bus endpoint (::tfbus_uart_local_fn_t). */
int ms_vmod_frame(const uint8_t *tx, size_t txlen, uint8_t *rx, size_t rxcap);

/**
 * @brief Bound slots.
 * @return Number written to @p out.
 */
int ms_vmod_list(ms_vmod_slot_t *out, int max);

/** @brief Node module @p module (0-based) belongs to; 0 = none (free / mixed), -1 = no such module. */
int ms_vmod_owner(int module);

/**
 * @brief Free the slots of a node (child 255 = all its children); saved.
 * @return Number of slots freed.
 */
int ms_vmod_unmap(uint8_t node, uint8_t child);

/**
 * @brief Remove what belongs to nodes no longer there: their channels, then every module left
 * without channels (later modules move down; their serials stay). Saved. Call only right before a
 * bus RESET (discovery): the modules are re-addressed after it.
 * @param alive Bit n set: node n was heard (exists).
 * @return Number of modules removed.
 */
int ms_vmod_prune(const uint8_t alive[32]);

#ifdef __cplusplus
}
#endif
