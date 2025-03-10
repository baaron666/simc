#ifndef DATA_ENUMS_HH
#define DATA_ENUMS_HH

#include <cstdint>

enum spell_type
{
  SPELL_TYPE_NONE = 0u,
  SPELL_TYPE_MAGIC,
  SPELL_TYPE_MELEE,
  SPELL_TYPE_RANGED
};

// Types for CombatRatingsMultByILvl.txt. The ordering is defined by dbc_extract outputter (see
// dbc_extract.py 'scale' output option), however it follows the current game table ordering.
enum combat_rating_multiplier_type
{
  CR_MULTIPLIER_INVALID = -1,
  CR_MULTIPLIER_ARMOR,
  CR_MULTIPLIER_WEAPON,
  CR_MULTIPLIER_TRINKET,
  CR_MULTIPLIER_JEWLERY,
  CR_MULTIPLIER_MAX
};

enum spell_mechanic {
  MECHANIC_CHARM = 1,
  MECHANIC_DISORIENT,
  MECHANIC_DISARM,
  MECHANIC_DISTRACT,
  MECHANIC_FLEE,
  MECHANIC_KNOCKBACK,
  MECHANIC_ROOT,
  MECHANIC_SLOW,
  MECHANIC_SILENCE,
  MECHANIC_SLEEP,
  MECHANIC_SNARE,
  MECHANIC_STUN,
  MECHANIC_FREEZE,
  MECHANIC_INCAPACITATE,
  MECHANIC_BLEED,
  MECHANIC_HEAL,
  MECHANIC_POLYMORPH,
  MECHANIC_BANISH,
  MECHANIC_SHIELD,
  MECHANIC_SHACKLE,
  MECHANIC_MOUNT,
  MECHANIC_INFECT,
  MECHANIC_TURN,
  MECHANIC_HORRIFY,
  MECHANIC_INVULNERABLE_2,
  MECHANIC_INTERRUPT,
  MECHANIC_DAZE,
  MECHANIC_DISCOVER,
  MECHANIC_INVULNERABLE,
  MECHANIC_SAP,
  MECHANIC_ENRAGE,
  MECHANIC_WOUND,
  MECHANIC_INFECT_2,
  MECHANIC_INFECT_3,
  MECHANIC_INFECT_4,
  MECHANIC_TAUNT
};

enum rppm_modifier_type_e
{
  RPPM_MODIFIER_HASTE = 1,
  RPPM_MODIFIER_CRIT,
  RPPM_MODIFIER_CLASS,
  RPPM_MODIFIER_SPEC,
  RPPM_MODIFIER_RACE,
  RPPM_MODIFIER_ILEVEL,
  RPPM_MODIFIER_UNK_ADJUST
};

enum item_bonus_type
{
  ITEM_BONUS_ILEVEL          = 1,
  ITEM_BONUS_MOD             = 2,
  ITEM_BONUS_QUALITY         = 3,
  ITEM_BONUS_DESC            = 4,
  ITEM_BONUS_SUFFIX          = 5,
  ITEM_BONUS_SOCKET          = 6,
  ITEM_BONUS_REQ_LEVEL       = 8,
  ITEM_BONUS_SCALING         = 11,  // Scaling based on ScalingStatDistribution.db2
  ITEM_BONUS_SCALING_2       = 13,  // Scaling based on ScalingStatDistribution.db2
  ITEM_BONUS_SET_ILEVEL      = 14,
  ITEM_BONUS_ADD_RANK        = 17,  // Add artifact power rank to a specific trait
  ITEM_BONUS_ADD_ITEM_EFFECT = 23,
  ITEM_BONUS_MOD_ITEM_STAT   = 25,  // Modify item stat to type
  ITEM_BONUS_ILEVEL_IN_PVP   = 36,  // Item has a higher level in PvP context
  ITEM_BONUS_SET_ILEVEL_2    = 42   // Used in some DF (10.0) crafted items
};

enum proc_types
{
  PROC1_HEARTBEAT = 0,
  PROC1_KILLING_BLOW,
  PROC1_MELEE,
  PROC1_MELEE_TAKEN,
  PROC1_MELEE_ABILITY,
  PROC1_MELEE_ABILITY_TAKEN,
  PROC1_RANGED,
  PROC1_RANGED_TAKEN,
  PROC1_RANGED_ABILITY,
  PROC1_RANGED_ABILITY_TAKEN,
  PROC1_NONE_HEAL,
  PROC1_NONE_HEAL_TAKEN,
  PROC1_NONE_SPELL,
  PROC1_NONE_SPELL_TAKEN,
  PROC1_MAGIC_HEAL,
  PROC1_MAGIC_HEAL_TAKEN,
  PROC1_MAGIC_SPELL,
  PROC1_MAGIC_SPELL_TAKEN,
  PROC1_PERIODIC,
  PROC1_PERIODIC_TAKEN,
  PROC1_ANY_DAMAGE_TAKEN,
  PROC1_HELPFUL_PERIODIC,
  PROC1_MAINHAND_ATTACK,
  PROC1_OFFHAND_ATTACK,
  // Relevant blizzard flags end here

  // These are Blizzard types, but special handling is required
  PROC1_HELPFUL_PERIODIC_TAKEN,  // enum is 24, but bit is 31
  PROC1_CAST_SUCCESSFUL,

  PROC1_TYPE_MAX,

  // Helper types to loop around stuff
  PROC1_TYPE_MIN = 0,
  PROC1_TYPE_BLIZZARD_MAX = PROC1_HELPFUL_PERIODIC_TAKEN,
  PROC1_INVALID = -1
};

enum proc_types2
{
  PROC2_HIT = 0,                // Any hit damage/heal result
  PROC2_CRIT,                   // Critical damage/heal result
  PROC2_GLANCE,                 // Glance damage result
  PROC2_DODGE,
  PROC2_PARRY,

  PROC2_MISS,

  PROC2_LANDED,                 // Any "positive" execute result
  PROC2_CAST_GENERIC,           // Generic (not damage nor heal) cast finished
  PROC2_CAST_DAMAGE,            // Damaging proc_types1 cast finished
  PROC2_CAST_HEAL,              // Healing proc_types1 cast finished
  PROC2_CAST_INTERRUPT,         // actions with 'is_interrupt' set to true
  PROC2_TYPE_MAX,

  // Pseudo types
  PROC2_PERIODIC_HEAL,          // Tick healing, when only PROC1_PERIODIC is defined
  PROC2_PERIODIC_DAMAGE,        // Tick damage, when only PROC1_PERIODIC is defined

  PROC2_TYPE_MIN = 0,
  PROC2_INVALID = -1
};

enum item_raid_type
{
  RAID_TYPE_NORMAL   = 0x00,
  RAID_TYPE_LFR      = 0x01,
  RAID_TYPE_HEROIC   = 0x02,
  RAID_TYPE_WARFORGED = 0x04,
  RAID_TYPE_MYTHIC    = 0x10,
};

// Mangos data types for various DBC-related enumerations
enum proc_flag : uint64_t
{
  PF_HEARTBEAT              = uint64_t( 1 ) << PROC1_HEARTBEAT,
  PF_KILLING_BLOW           = uint64_t( 1 ) << PROC1_KILLING_BLOW,
  PF_MELEE                  = uint64_t( 1 ) << PROC1_MELEE,
  PF_MELEE_TAKEN            = uint64_t( 1 ) << PROC1_MELEE_TAKEN,
  PF_MELEE_ABILITY          = uint64_t( 1 ) << PROC1_MELEE_ABILITY,
  PF_MELEE_ABILITY_TAKEN    = uint64_t( 1 ) << PROC1_MELEE_ABILITY_TAKEN,
  PF_RANGED                 = uint64_t( 1 ) << PROC1_RANGED,
  PF_RANGED_TAKEN           = uint64_t( 1 ) << PROC1_RANGED_TAKEN,
  PF_RANGED_ABILITY         = uint64_t( 1 ) << PROC1_RANGED_ABILITY,
  PF_RANGED_ABILITY_TAKEN   = uint64_t( 1 ) << PROC1_RANGED_ABILITY_TAKEN,
  PF_NONE_HEAL              = uint64_t( 1 ) << PROC1_NONE_HEAL,
  PF_NONE_HEAL_TAKEN        = uint64_t( 1 ) << PROC1_NONE_HEAL_TAKEN,
  PF_NONE_SPELL             = uint64_t( 1 ) << PROC1_NONE_SPELL,
  PF_NONE_SPELL_TAKEN       = uint64_t( 1 ) << PROC1_NONE_SPELL_TAKEN,
  PF_MAGIC_HEAL             = uint64_t( 1 ) << PROC1_MAGIC_HEAL,
  PF_MAGIC_HEAL_TAKEN       = uint64_t( 1 ) << PROC1_MAGIC_HEAL_TAKEN,
  PF_MAGIC_SPELL            = uint64_t( 1 ) << PROC1_MAGIC_SPELL, // Any "negative" spell
  PF_MAGIC_SPELL_TAKEN      = uint64_t( 1 ) << PROC1_MAGIC_SPELL_TAKEN,
  PF_PERIODIC               = uint64_t( 1 ) << PROC1_PERIODIC,
  PF_PERIODIC_TAKEN         = uint64_t( 1 ) << PROC1_PERIODIC_TAKEN,
  PF_ANY_DAMAGE_TAKEN       = uint64_t( 1 ) << PROC1_ANY_DAMAGE_TAKEN,
  PF_HELPFUL_PERIODIC       = uint64_t( 1 ) << PROC1_HELPFUL_PERIODIC,
  PF_MAINHAND               = uint64_t( 1 ) << PROC1_MAINHAND_ATTACK,
  PF_OFFHAND                = uint64_t( 1 ) << PROC1_OFFHAND_ATTACK,

  PF_DEATH                  = 0x01000000,  // unused
  PF_JUMP                   = 0x02000000,  // unused
  PF_CLONE_SPELL            = 0x04000000,  // unused
  PF_ENTER_COMBAT           = 0x08000000,  // unused
  PF_ENCOUNTER_START        = 0x10000000,  // unused
  PF_CAST_ENDED             = 0x20000000,  // unused
  PF_LOOTED                 = 0x40000000,  // unused
  PF_HELPFUL_PERIODIC_TAKEN = 0x80000000,
  PF_TARGET_DIES            = 0x100000000, // unused
  PF_KNOCKBACK              = 0x200000000, // unused
  PF_CAST_SUCCESSFUL        = 0x400000000, // TODO: This is relatively commonly used and should be supported.

  PF_UNKNOWN_36             = 0x800000000,
  PF_UNKNOWN_37             = 0x1000000000,
  PF_UNKNOWN_38             = 0x2000000000,
  PF_UNKNOWN_39             = 0x4000000000,

  // Helper types
  PF_ALL_DAMAGE             = PF_MELEE | PF_MELEE_ABILITY |
                              PF_RANGED | PF_RANGED_ABILITY |
                              PF_NONE_SPELL | PF_MAGIC_SPELL,
  PF_ALL_HEAL               = PF_NONE_HEAL | PF_MAGIC_HEAL,

  PF_DAMAGE_TAKEN           = PF_MELEE_TAKEN | PF_MELEE_ABILITY_TAKEN |
                              PF_RANGED_TAKEN | PF_RANGED_ABILITY_TAKEN |
                              PF_NONE_SPELL_TAKEN | PF_MAGIC_SPELL_TAKEN,
  PF_ALL_HEAL_TAKEN         = PF_NONE_HEAL_TAKEN | PF_MAGIC_HEAL_TAKEN,
};

// Qualifier on what result / advanced type allows a proc trigger
enum proc_flag2
{
  PF2_HIT                     = 1 << PROC2_HIT,
  PF2_CRIT                    = 1 << PROC2_CRIT, // Automatically implies an amount
  PF2_GLANCE                  = 1 << PROC2_GLANCE, // Automatically implies damage
  PF2_DODGE                   = 1 << PROC2_DODGE,
  PF2_PARRY                   = 1 << PROC2_PARRY,

  PF2_MISS                    = 1 << PROC2_MISS,

  PF2_LANDED                  = 1 << PROC2_LANDED,
  PF2_CAST_GENERIC            = 1 << PROC2_CAST_GENERIC,
  PF2_CAST_DAMAGE             = 1 << PROC2_CAST_DAMAGE,
  PF2_CAST_HEAL               = 1 << PROC2_CAST_HEAL,
  PF2_CAST_INTERRUPT          = 1 << PROC2_CAST_INTERRUPT,

  // Pseudo types
  PF2_PERIODIC_HEAL           = 1 << PROC2_PERIODIC_HEAL,
  PF2_PERIODIC_DAMAGE         = 1 << PROC2_PERIODIC_DAMAGE,
  PF2_ALL_HIT                 = PF2_HIT | PF2_CRIT | PF2_GLANCE, // All damaging/healing "hit" results
  PF2_ALL_CAST                = PF2_CAST_GENERIC | PF2_CAST_DAMAGE | PF2_CAST_HEAL, // All successful cast results
};

enum item_flag
{
    ITEM_FLAG_UNK0                            = 0x00000001, // not used
    ITEM_FLAG_CONJURED                        = 0x00000002,
    ITEM_FLAG_LOOTABLE                        = 0x00000004, // affect only non container items that can be "open" for loot. It or lockid set enable for client show "Right click to open". See also ITEM_DYNFLAG_UNLOCKED
    ITEM_FLAG_HEROIC                          = 0x00000008, // heroic item version
    ITEM_FLAG_UNK4                            = 0x00000010, // can't repeat old note: appears red icon (like when item durability==0)
    ITEM_FLAG_INDESTRUCTIBLE                  = 0x00000020, // used for totem. Item can not be destroyed, except by using spell (item can be reagent for spell and then allowed)
    ITEM_FLAG_UNK6                            = 0x00000040, // ? old note: usable
    ITEM_FLAG_NO_EQUIP_COOLDOWN               = 0x00000080,
    ITEM_FLAG_UNK8                            = 0x00000100, // saw this on item 47115, 49295...
    ITEM_FLAG_WRAPPER                         = 0x00000200, // used or not used wrapper
    ITEM_FLAG_IGNORE_BAG_SPACE                = 0x00000400, // ignore bag space at new item creation?
    ITEM_FLAG_PARTY_LOOT                      = 0x00000800, // determines if item is party loot or not
    ITEM_FLAG_REFUNDABLE                      = 0x00001000, // item cost can be refunded within 2 hours after purchase
    ITEM_FLAG_CHARTER                         = 0x00002000, // arena/guild charter
    ITEM_FLAG_UNK14                           = 0x00004000,
    ITEM_FLAG_UNK15                           = 0x00008000, // a lot of items have this
    ITEM_FLAG_UNK16                           = 0x00010000, // a lot of items have this
    ITEM_FLAG_UNK17                           = 0x00020000,
    ITEM_FLAG_PROSPECTABLE                    = 0x00040000, // item can have prospecting loot (in fact some items expected have empty loot)
    ITEM_FLAG_UNIQUE_EQUIPPED                 = 0x00080000,
    ITEM_FLAG_UNK20                           = 0x00100000,
    ITEM_FLAG_USEABLE_IN_ARENA                = 0x00200000,
    ITEM_FLAG_THROWABLE                       = 0x00400000, // Only items of ITEM_SUBCLASS_WEAPON_THROWN have it but not all, so can't be used as in game check
    ITEM_FLAG_SPECIALUSE                      = 0x00800000, // last used flag in 2.3.0
    ITEM_FLAG_UNK24                           = 0x01000000,
    ITEM_FLAG_UNK25                           = 0x02000000,
    ITEM_FLAG_UNK26                           = 0x04000000,
    ITEM_FLAG_BOA                             = 0x08000000, // bind on account (set in template for items that can binded in like way)
    ITEM_FLAG_ENCHANT_SCROLL                  = 0x10000000, // for enchant scrolls
    ITEM_FLAG_MILLABLE                        = 0x20000000, // item can have milling loot
    ITEM_FLAG_UNK30                           = 0x04000000,
    ITEM_FLAG_BOP_TRADEABLE                   = 0x80000000, // bound item that can be traded
};

enum item_flag2
{
    ITEM_FLAG2_HORDE_ONLY                     = 0x00000001, // drop in loot, sell by vendor and equipping only for horde
    ITEM_FLAG2_ALLIANCE_ONLY                  = 0x00000002, // drop in loot, sell by vendor and equipping only for alliance
    ITEM_FLAG2_EXT_COST_REQUIRES_GOLD         = 0x00000004, // item cost include gold part in case extended cost use also
    ITEM_FLAG2_UNK4                           = 0x00000008,
    ITEM_FLAG2_UNK5                           = 0x00000010,
    ITEM_FLAG2_UNK6                           = 0x00000020,
    ITEM_FLAG2_UNK7                           = 0x00000040,
    ITEM_FLAG2_UNK8                           = 0x00000080,
    ITEM_FLAG2_NEED_ROLL_DISABLED             = 0x00000100, // need roll during looting is not allowed for this item
    ITEM_FLAG2_CASTER_WEAPON                  = 0x00000200, // uses caster specific dbc file for DPS calculations
    ITEM_FLAG2_HEROIC                         = 0x40000000, // MoP Heroic item flag
};

enum item_class
{
    ITEM_CLASS_CONSUMABLE                     = 0,
    ITEM_CLASS_CONTAINER                      = 1,
    ITEM_CLASS_WEAPON                         = 2,
    ITEM_CLASS_GEM                            = 3,
    ITEM_CLASS_ARMOR                          = 4,
    ITEM_CLASS_REAGENT                        = 5,
    ITEM_CLASS_PROJECTILE                     = 6,
    ITEM_CLASS_TRADE_GOODS                    = 7,
    ITEM_CLASS_GENERIC                        = 8,
    ITEM_CLASS_RECIPE                         = 9,
    ITEM_CLASS_MONEY                          = 10,
    ITEM_CLASS_QUIVER                         = 11,
    ITEM_CLASS_QUEST                          = 12,
    ITEM_CLASS_KEY                            = 13,
    ITEM_CLASS_PERMANENT                      = 14,
    ITEM_CLASS_MISC                           = 15,
    ITEM_CLASS_GLYPH                          = 16
};

enum item_subclass_weapon
{
    ITEM_SUBCLASS_WEAPON_AXE                  = 0,
    ITEM_SUBCLASS_WEAPON_AXE2                 = 1,
    ITEM_SUBCLASS_WEAPON_BOW                  = 2,
    ITEM_SUBCLASS_WEAPON_GUN                  = 3,
    ITEM_SUBCLASS_WEAPON_MACE                 = 4,
    ITEM_SUBCLASS_WEAPON_MACE2                = 5,
    ITEM_SUBCLASS_WEAPON_POLEARM              = 6,
    ITEM_SUBCLASS_WEAPON_SWORD                = 7,
    ITEM_SUBCLASS_WEAPON_SWORD2               = 8,
    ITEM_SUBCLASS_WEAPON_WARGLAIVE            = 9,
    ITEM_SUBCLASS_WEAPON_STAFF                = 10,
    ITEM_SUBCLASS_WEAPON_EXOTIC               = 11,
    ITEM_SUBCLASS_WEAPON_EXOTIC2              = 12,
    ITEM_SUBCLASS_WEAPON_FIST                 = 13,
    ITEM_SUBCLASS_WEAPON_MISC                 = 14,
    ITEM_SUBCLASS_WEAPON_DAGGER               = 15,
    ITEM_SUBCLASS_WEAPON_THROWN               = 16,
    ITEM_SUBCLASS_WEAPON_SPEAR                = 17,
    ITEM_SUBCLASS_WEAPON_CROSSBOW             = 18,
    ITEM_SUBCLASS_WEAPON_WAND                 = 19,
    ITEM_SUBCLASS_WEAPON_FISHING_POLE         = 20,
    ITEM_SUBCLASS_WEAPON_INVALID              = 31
};

enum item_subclass_armor
{
    ITEM_SUBCLASS_ARMOR_MISC                  = 0,
    ITEM_SUBCLASS_ARMOR_CLOTH                 = 1,
    ITEM_SUBCLASS_ARMOR_LEATHER               = 2,
    ITEM_SUBCLASS_ARMOR_MAIL                  = 3,
    ITEM_SUBCLASS_ARMOR_PLATE                 = 4,
    ITEM_SUBCLASS_ARMOR_COSMETIC              = 5,
    ITEM_SUBCLASS_ARMOR_SHIELD                = 6,
    ITEM_SUBCLASS_ARMOR_LIBRAM                = 7,
    ITEM_SUBCLASS_ARMOR_IDOL                  = 8,
    ITEM_SUBCLASS_ARMOR_TOTEM                 = 9,
    ITEM_SUBCLASS_ARMOR_SIGIL                 = 10,
    ITEM_SUBCLASS_ARMOR_RELIC                 = 11
};

enum item_subclass_consumable
{
    ITEM_SUBCLASS_CONSUMABLE                    = 0,
    ITEM_SUBCLASS_POTION                        = 1,
    ITEM_SUBCLASS_ELIXIR                        = 2,
    ITEM_SUBCLASS_FLASK                         = 3,
    ITEM_SUBCLASS_SCROLL                        = 4,
    ITEM_SUBCLASS_FOOD                          = 5,
    ITEM_SUBCLASS_ITEM_ENHANCEMENT              = 6,
    ITEM_SUBCLASS_BANDAGE                       = 7,
    ITEM_SUBCLASS_CONSUMABLE_OTHER              = 8
};

enum inventory_type
{
    INVTYPE_NON_EQUIP                         = 0,
    INVTYPE_HEAD                              = 1,
    INVTYPE_NECK                              = 2,
    INVTYPE_SHOULDERS                         = 3,
    INVTYPE_BODY                              = 4,
    INVTYPE_CHEST                             = 5,
    INVTYPE_WAIST                             = 6,
    INVTYPE_LEGS                              = 7,
    INVTYPE_FEET                              = 8,
    INVTYPE_WRISTS                            = 9,
    INVTYPE_HANDS                             = 10,
    INVTYPE_FINGER                            = 11,
    INVTYPE_TRINKET                           = 12,
    INVTYPE_WEAPON                            = 13,
    INVTYPE_SHIELD                            = 14,
    INVTYPE_RANGED                            = 15,
    INVTYPE_CLOAK                             = 16,
    INVTYPE_2HWEAPON                          = 17,
    INVTYPE_BAG                               = 18,
    INVTYPE_TABARD                            = 19,
    INVTYPE_ROBE                              = 20,
    INVTYPE_WEAPONMAINHAND                    = 21,
    INVTYPE_WEAPONOFFHAND                     = 22,
    INVTYPE_HOLDABLE                          = 23,
    INVTYPE_AMMO                              = 24,
    INVTYPE_THROWN                            = 25,
    INVTYPE_RANGEDRIGHT                       = 26,
    INVTYPE_QUIVER                            = 27,
    INVTYPE_RELIC                             = 28,
    INVTYPE_MAX                               = 29
};

enum item_quality
{
  ITEM_QUALITY_NONE      = -1,
  ITEM_QUALITY_POOR      = 0,
  ITEM_QUALITY_COMMON    = 1,
  ITEM_QUALITY_UNCOMMON  = 2,
  ITEM_QUALITY_RARE      = 3,
  ITEM_QUALITY_EPIC      = 4,
  ITEM_QUALITY_LEGENDARY = 5,
  ITEM_QUALITY_ARTIFACT  = 6,
  ITEM_QUALITY_MAX       = 7
};

enum item_enchantment
{
    ITEM_ENCHANTMENT_NONE             = 0,
    ITEM_ENCHANTMENT_COMBAT_SPELL     = 1,
    ITEM_ENCHANTMENT_DAMAGE           = 2,
    ITEM_ENCHANTMENT_EQUIP_SPELL      = 3,
    ITEM_ENCHANTMENT_RESISTANCE       = 4,
    ITEM_ENCHANTMENT_STAT             = 5,
    ITEM_ENCHANTMENT_TOTEM            = 6,
    ITEM_ENCHANTMENT_USE_SPELL        = 7,
    ITEM_ENCHANTMENT_PRISMATIC_SOCKET = 8,
    ITEM_ENCHANTMENT_RELIC_RANK       = 9,
    ITEM_ENCHANTMENT_APPLY_BONUS      = 11,
    ITEM_ENCHANTMENT_RELIC_EVIL       = 12, // Scaling relic +ilevel, see enchant::initialize_relic
};

enum item_spell_trigger_type
{
    ITEM_SPELLTRIGGER_ON_USE          = 0, // use after equip cooldown
    ITEM_SPELLTRIGGER_ON_EQUIP        = 1,
    ITEM_SPELLTRIGGER_CHANCE_ON_HIT   = 2,
    ITEM_SPELLTRIGGER_SOULSTONE       = 4,
    ITEM_SPELLTRIGGER_ON_NO_DELAY_USE = 5, // no equip cooldown
    ITEM_SPELLTRIGGER_LEARN_SPELL_ID  = 6
};

/* If you add socket colors here, update dbc::valid_gem_color() too */
enum item_socket_color
{
  SOCKET_COLOR_NONE                 = 0x00000000,
  SOCKET_COLOR_META                 = 0x00000001,
  SOCKET_COLOR_RED                  = 0x00000002,
  SOCKET_COLOR_YELLOW               = 0x00000004,
  SOCKET_COLOR_BLUE                 = 0x00000008,
  SOCKET_COLOR_ORANGE               = SOCKET_COLOR_RED | SOCKET_COLOR_YELLOW,
  SOCKET_COLOR_PURPLE               = SOCKET_COLOR_RED | SOCKET_COLOR_BLUE,
  SOCKET_COLOR_GREEN                = SOCKET_COLOR_BLUE | SOCKET_COLOR_YELLOW,
  SOCKET_COLOR_HYDRAULIC            = 0x00000010,
  SOCKET_COLOR_PRISMATIC            = SOCKET_COLOR_RED | SOCKET_COLOR_YELLOW | SOCKET_COLOR_BLUE,
  SOCKET_COLOR_COGWHEEL             = 0x00000020,
  // Legion
  SOCKET_COLOR_IRON                 = 0x00000040,
  SOCKET_COLOR_BLOOD                = 0x00000080,
  SOCKET_COLOR_SHADOW               = 0x00000100,
  SOCKET_COLOR_FEL                  = 0x00000200,
  SOCKET_COLOR_ARCANE               = 0x00000400,
  SOCKET_COLOR_FROST                = 0x00000800,
  SOCKET_COLOR_FIRE                 = 0x00001000,
  SOCKET_COLOR_WATER                = 0x00002000,
  SOCKET_COLOR_LIFE                 = 0x00004000,
  SOCKET_COLOR_WIND                 = 0x00008000,
  SOCKET_COLOR_HOLY                 = 0x00010000,
  // BFA
  SOCKET_COLOR_RED_PUNCHCARD        = 0x00020000,
  SOCKET_COLOR_YELLOW_PUNCHCARD     = 0x00040000,
  SOCKET_COLOR_BLUE_PUNCHCARD       = 0x00080000,
  // Shadowlands
  SOCKET_COLOR_SHARD_OF_DOMINATION  = 0x00100000,
  SOCKET_COLOR_CRYSTALLIC           = 0x00800000,
  // Dragonflight
  SOCKET_COLOR_TINKER               = 0x01000000,
  SOCKET_COLOR_PRIMORDIAL           = 0x02000000,
  // The War Within
  SOCKET_COLOR_FRAGRANCE            = 0x04000000,
  SOCKET_COLOR_THUNDER_CITRINE      = 0x08000000,
  SOCKET_COLOR_SEA_CITRINE          = 0x10000000,
  SOCKET_COLOR_WIND_CITRINE         = 0x20000000,
  SOCKET_COLOR_MAX,
  SOCKET_COLOR_RELIC                = SOCKET_COLOR_IRON | SOCKET_COLOR_BLOOD  | SOCKET_COLOR_SHADOW |
                                      SOCKET_COLOR_FEL  | SOCKET_COLOR_ARCANE | SOCKET_COLOR_FROST |
                                      SOCKET_COLOR_FIRE | SOCKET_COLOR_WATER  | SOCKET_COLOR_LIFE |
                                      SOCKET_COLOR_WIND | SOCKET_COLOR_HOLY
};

enum item_bind_type
{
    NO_BIND                           = 0,
    BIND_WHEN_PICKED_UP               = 1,
    BIND_WHEN_EQUIPPED                = 2,
    BIND_WHEN_USE                     = 3,
    BIND_QUEST_ITEM                   = 4,
    BIND_QUEST_ITEM1                  = 5         // not used in game
};

enum item_mod_type {
  ITEM_MOD_NONE                     = -1,
  ITEM_MOD_MANA                     = 0,
  ITEM_MOD_HEALTH                   = 1,
  ITEM_MOD_AGILITY                  = 3,
  ITEM_MOD_STRENGTH                 = 4,
  ITEM_MOD_INTELLECT                = 5,
  ITEM_MOD_SPIRIT                   = 6,
  ITEM_MOD_STAMINA                  = 7,
  ITEM_MOD_DEFENSE_SKILL_RATING     = 12,
  ITEM_MOD_DODGE_RATING             = 13,
  ITEM_MOD_PARRY_RATING             = 14,
  ITEM_MOD_BLOCK_RATING             = 15,
  ITEM_MOD_HIT_MELEE_RATING         = 16,
  ITEM_MOD_HIT_RANGED_RATING        = 17,
  ITEM_MOD_HIT_SPELL_RATING         = 18,
  ITEM_MOD_CRIT_MELEE_RATING        = 19,
  ITEM_MOD_CRIT_RANGED_RATING       = 20,
  ITEM_MOD_CRIT_SPELL_RATING        = 21,
  ITEM_MOD_CORRUPTION               = 22,
  ITEM_MOD_CORRUPTION_RESISTANCE    = 23, // TODO: Guess
  ITEM_MOD_BONUS_STAT_1             = 24,
  ITEM_MOD_BONUS_STAT_2             = 25,
  ITEM_MOD_CRIT_TAKEN_RANGED_RATING = 26,
  ITEM_MOD_CRIT_TAKEN_SPELL_RATING  = 27,
  ITEM_MOD_HASTE_MELEE_RATING       = 28,
  ITEM_MOD_HASTE_RANGED_RATING      = 29,
  ITEM_MOD_HASTE_SPELL_RATING       = 30,
  ITEM_MOD_HIT_RATING               = 31,
  ITEM_MOD_CRIT_RATING              = 32,
  ITEM_MOD_HIT_TAKEN_RATING         = 33,
  ITEM_MOD_CRIT_TAKEN_RATING        = 34,
  ITEM_MOD_RESILIENCE_RATING        = 35,
  ITEM_MOD_HASTE_RATING             = 36,
  ITEM_MOD_EXPERTISE_RATING         = 37,
  ITEM_MOD_ATTACK_POWER             = 38,
  ITEM_MOD_RANGED_ATTACK_POWER      = 39,
  ITEM_MOD_VERSATILITY_RATING       = 40,
  ITEM_MOD_SPELL_HEALING_DONE       = 41,                 // deprecated
  ITEM_MOD_SPELL_DAMAGE_DONE        = 42,                 // deprecated
  ITEM_MOD_MANA_REGENERATION        = 43,
  ITEM_MOD_ARMOR_PENETRATION_RATING = 44,
  ITEM_MOD_SPELL_POWER              = 45,
  ITEM_MOD_HEALTH_REGEN             = 46,
  ITEM_MOD_SPELL_PENETRATION        = 47,
  ITEM_MOD_BLOCK_VALUE              = 48,
  ITEM_MOD_MASTERY_RATING           = 49,
  ITEM_MOD_EXTRA_ARMOR              = 50,
  ITEM_MOD_FIRE_RESISTANCE          = 51,
  ITEM_MOD_FROST_RESISTANCE         = 52,
  ITEM_MOD_HOLY_RESISTANCE          = 53,
  ITEM_MOD_SHADOW_RESISTANCE        = 54,
  ITEM_MOD_NATURE_RESISTANCE        = 55,
  ITEM_MOD_ARCANE_RESISTANCE        = 56,
  ITEM_MOD_PVP_POWER                = 57,
  ITEM_MOD_MULTISTRIKE_RATING       = 59,
  ITEM_MOD_READINESS_RATING         = 60,
  ITEM_MOD_SPEED_RATING             = 61,
  ITEM_MOD_LEECH_RATING             = 62,
  ITEM_MOD_AVOIDANCE_RATING         = 63,
  ITEM_MOD_INDESTRUCTIBLE           = 64,
  ITEM_MOD_WOD_5                    = 65,
  ITEM_MOD_WOD_6                    = 66,
  ITEM_MOD_STRENGTH_AGILITY_INTELLECT = 71,
  ITEM_MOD_STRENGTH_AGILITY         = 72,
  ITEM_MOD_AGILITY_INTELLECT        = 73,
  ITEM_MOD_STRENGTH_INTELLECT       = 74,
};

enum item_cooldown_group {
  ITEM_COOLDOWN_GROUP_NONE          = 0,
  ITEM_COOLDOWN_GROUP_POTION        = 4,
};

enum rating_mod_type {
  RATING_MOD_DODGE        = 0x00000004,
  RATING_MOD_PARRY        = 0x00000008,
  RATING_MOD_HIT_MELEE    = 0x00000020,
  RATING_MOD_HIT_RANGED   = 0x00000040,
  RATING_MOD_HIT_SPELL    = 0x00000080,
  RATING_MOD_CRIT_MELEE   = 0x00000100,
  RATING_MOD_CRIT_RANGED  = 0x00000200,
  RATING_MOD_CRIT_SPELL   = 0x00000400,
  RATING_MOD_MULTISTRIKE  = 0x00000800,
  RATING_MOD_READINESS    = 0x00001000,
  RATING_MOD_SPEED        = 0x00002000,
  RATING_MOD_RESILIENCE   = 0x00008000,
  RATING_MOD_LEECH        = 0x00010000,
  RATING_MOD_HASTE_MELEE  = 0x00020000,
  RATING_MOD_HASTE_RANGED = 0x00040000,
  RATING_MOD_HASTE_SPELL  = 0x00080000,
  RATING_MOD_AVOIDANCE    = 0x00100000,
  RATING_MOD_EXPERTISE    = 0x00800000,
  RATING_MOD_MASTERY      = 0x02000000,
  RATING_MOD_PVP_POWER    = 0x04000000,

  RATING_MOD_VERS_DAMAGE  = 0x10000000,
  RATING_MOD_VERS_HEAL    = 0x20000000,
  RATING_MOD_VERS_MITIG   = 0x40000000,

};

// Property (misc_value) types for
// A_ADD_PCT_MODIFIER, A_ADD_FLAT_MODIFIER
enum property_type_t {
  P_GENERIC             = 0,
  P_DURATION            = 1,
  P_THREAT              = 2,
  P_EFFECT_1            = 3,
  P_STACK               = 4,
  P_RANGE               = 5,
  P_RADIUS              = 6,
  P_CRIT                = 7,
  P_EFFECTS             = 8,
  P_PUSHBACK            = 9,
  P_CAST_TIME           = 10,
  P_COOLDOWN            = 11,
  P_EFFECT_2            = 12,
  P_RESISTANCE          = 13,
  P_RESOURCE_COST       = 14,
  P_CRIT_BONUS          = 15,
  P_PENETRATION         = 16,
  P_TARGET              = 17, // Chain Targets
  P_PROC_CHANCE         = 18,
  P_TICK_TIME           = 19,
  P_TARGET_BONUS        = 20, // Improved Revenge
  P_GCD                 = 21, // Only used for flat modifiers?
  P_TICK_DAMAGE         = 22,
  P_EFFECT_3            = 23,
  P_SPELL_POWER         = 24,
  P_TRIGGER_DAMAGE      = 25,
  P_PROC_FREQUENCY      = 26,
  P_DAMAGE_TAKEN        = 27,
  P_DISPEL_CHANCE       = 28,
  P_COST_ON_MISS        = 30,
  P_EFFECT_4            = 32,
  P_EFFECT_5            = 33,
  P_RESOURCE_COST_1     = 34,
  P_CHAIN_TARGET_RANGE  = 35,
  P_MAX_STACKS          = 37,
  P_PROC_COOLDOWN       = 38,
  P_RESOURCE_COST_2     = 39,
  P_MAX
};

enum effect_type_t : unsigned {
    E_NONE = 0,
    E_INSTAKILL = 1,
    E_SCHOOL_DAMAGE = 2,
    E_DUMMY = 3,
    E_PORTAL_TELEPORT = 4,
    E_TELEPORT_UNITS = 5,
    E_APPLY_AURA = 6,
    E_ENVIRONMENTAL_DAMAGE = 7,
    E_POWER_DRAIN = 8,
    E_HEALTH_LEECH = 9,
    E_HEAL = 10,
    E_BIND = 11,
    E_PORTAL = 12,
    E_RITUAL_BASE = 13,
    E_RITUAL_SPECIALIZE = 14,
    E_RITUAL_ACTIVATE_PORTAL = 15,
    E_QUEST_COMPLETE = 16,
    E_WEAPON_DAMAGE_NOSCHOOL = 17,
    E_RESURRECT = 18,
    E_ADD_EXTRA_ATTACKS = 19,
    E_DODGE = 20,
    E_EVADE = 21,
    E_PARRY = 22,
    E_BLOCK = 23,
    E_CREATE_ITEM = 24,
    E_WEAPON = 25,
    E_DEFENSE = 26,
    E_PERSISTENT_AREA_AURA = 27,
    E_SUMMON = 28,
    E_LEAP = 29,
    E_ENERGIZE = 30,
    E_WEAPON_PERCENT_DAMAGE = 31,
    E_TRIGGER_MISSILE = 32,
    E_OPEN_LOCK = 33,
    E_SUMMON_CHANGE_ITEM = 34,
    E_APPLY_AREA_AURA_PARTY = 35,
    E_LEARN_SPELL = 36,
    E_SPELL_DEFENSE = 37,
    E_DISPEL = 38,
    E_LANGUAGE = 39,
    E_DUAL_WIELD = 40,
    E_JUMP = 41,
    E_JUMP2 = 42,
    E_TELEPORT_UNITS_FACE_CASTER= 43,
    E_SKILL_STEP = 44,
    E_ADD_HONOR = 45,
    E_SPAWN = 46,
    E_TRADE_SKILL = 47,
    E_STEALTH = 48,
    E_DETECT = 49,
    E_TRANS_DOOR = 50,
    E_FORCE_CRITICAL_HIT = 51,
    E_GUARANTEE_HIT = 52,
    E_ENCHANT_ITEM = 53,
    E_ENCHANT_ITEM_TEMPORARY = 54,
    E_TAMECREATURE = 55,
    E_SUMMON_PET = 56,
    E_LEARN_PET_SPELL = 57,
    E_WEAPON_DAMAGE = 58,
    E_CREATE_RANDOM_ITEM = 59,
    E_PROFICIENCY = 60,
    E_SEND_EVENT = 61,
    E_POWER_BURN = 62,
    E_THREAT = 63,
    E_TRIGGER_SPELL = 64,
    E_APPLY_AREA_AURA_RAID = 65,
    E_RESTORE_ITEM_CHARGES = 66,
    E_HEAL_MAX_HEALTH = 67,
    E_INTERRUPT_CAST = 68,
    E_DISTRACT = 69,
    E_PULL = 70,
    E_PICKPOCKET = 71,
    E_ADD_FARSIGHT = 72,
    E_UNTRAIN_TALENTS = 73,
    E_APPLY_GLYPH = 74,
    E_HEAL_MECHANICAL = 75,
    E_SUMMON_OBJECT_WILD = 76,
    E_SCRIPT_EFFECT = 77,
    E_ATTACK = 78,
    E_SANCTUARY = 79,
    E_ADD_COMBO_POINTS = 80,
    E_CREATE_HOUSE = 81,
    E_BIND_SIGHT = 82,
    E_DUEL = 83,
    E_STUCK = 84,
    E_SUMMON_PLAYER = 85,
    E_ACTIVATE_OBJECT = 86,
    E_WMO_DAMAGE = 87,
    E_WMO_REPAIR = 88,
    E_WMO_CHANGE = 89,
    E_KILL_CREDIT = 90,
    E_THREAT_ALL = 91,
    E_ENCHANT_HELD_ITEM = 92,
    E_BREAK_PLAYER_TARGETING = 93,
    E_SELF_RESURRECT = 94,
    E_SKINNING = 95,
    E_CHARGE = 96,
    E_SUMMON_ALL_TOTEMS = 97,
    E_KNOCK_BACK = 98,
    E_DISENCHANT = 99,
    E_INEBRIATE = 100,
    E_FEED_PET = 101,
    E_DISMISS_PET = 102,
    E_REPUTATION = 103,
    E_SUMMON_OBJECT_SLOT1 = 104,
    E_SUMMON_OBJECT_SLOT2 = 105,
    E_SUMMON_OBJECT_SLOT3 = 106,
    E_SUMMON_OBJECT_SLOT4 = 107,
    E_DISPEL_MECHANIC = 108,
    E_SUMMON_DEAD_PET = 109,
    E_DESTROY_ALL_TOTEMS = 110,
    E_DURABILITY_DAMAGE = 111,
    E_112 = 112, // old E_SUMMON_DEMON
    E_RESURRECT_NEW = 113,
    E_ATTACK_ME = 114,
    E_DURABILITY_DAMAGE_PCT = 115,
    E_SKIN_PLAYER_CORPSE = 116,
    E_SPIRIT_HEAL = 117,
    E_SKILL = 118,
    E_APPLY_AREA_AURA_PET = 119,
    E_TELEPORT_GRAVEYARD = 120,
    E_NORMALIZED_WEAPON_DMG = 121,
    E_122 = 122,
    E_SEND_TAXI = 123,
    E_PLAYER_PULL = 124,
    E_MODIFY_THREAT_PERCENT = 125,
    E_STEAL_BENEFICIAL_BUFF = 126,
    E_PROSPECTING = 127,
    E_APPLY_AREA_AURA_FRIEND = 128,
    E_APPLY_AREA_AURA_ENEMY = 129,
    E_REDIRECT_THREAT = 130,
    E_131 = 131,
    E_PLAY_MUSIC = 132,
    E_UNLEARN_SPECIALIZATION = 133,
    E_KILL_CREDIT2 = 134,
    E_CALL_PET = 135,
    E_HEAL_PCT = 136,
    E_ENERGIZE_PCT = 137,
    E_LEAP_BACK = 138,
    E_CLEAR_QUEST = 139,
    E_FORCE_CAST = 140,
    E_141 = 141,
    E_TRIGGER_SPELL_WITH_VALUE = 142,
    E_APPLY_AREA_AURA_OWNER = 143,
    E_144 = 144,
    E_145 = 145,
    E_ACTIVATE_RUNE = 146,
    E_QUEST_FAIL = 147,
    E_148 = 148,
    E_149 = 149,
    E_150 = 150,
    E_TRIGGER_SPELL_2 = 151,
    E_152 = 152,
    E_153 = 153,
    E_TEACH_TAXI_NODE = 154,
    E_TITAN_GRIP = 155,
    E_ENCHANT_ITEM_PRISMATIC = 156,
    E_CREATE_ITEM_2 = 157,
    E_MILLING = 158,
    E_ALLOW_RENAME_PET = 159,
    E_160 = 160,
    E_TALENT_SPEC_COUNT = 161,
    E_TALENT_SPEC_SELECT = 162,
    E_163 = 163,
    E_CANCEL_AURA = 164,
    E_DAMAGE_TAKEN_PCT_MAX_HEALTH = 165,
    E_166 = 166,
    E_167 = 167,
    E_168 = 168,
    E_169 = 169,
    E_170 = 170,
    E_171 = 171,
    E_172 = 172,
    E_173 = 173,
    E_APPLY_AURA_PET = 174,
    E_175 = 175,
    E_176 = 176,
    E_179 = 179,
    E_181 = 181,
    E_188 = 188,
    E_189 = 189,
    E_196 = 196,
    E_197 = 197,
    E_198 = 198,
    E_199 = 199,
    E_APPLY_AURA_PLAYER_AND_PET = 202,
    E_203 = 203,
    E_205 = 205,
    E_206 = 206,
    E_213 = 213,
    E_223 = 223,
    E_225 = 225,
    E_226 = 226,
    E_230 = 230,
    E_231 = 231,
    E_236 = 236,
    E_237 = 237,
    E_238 = 238,
    E_240 = 240,
    E_241 = 241,
    E_242 = 242,
    E_243 = 243,
    E_245 = 245,
    E_247 = 247,
    E_248 = 248,
    E_249 = 249,
    E_252 = 252,
    E_254 = 254,
    E_255 = 255,
    E_256 = 256,
    E_REDUCE_REMAINING_COOLDOWN = 290,
    E_MAX
};


enum effect_subtype_t : unsigned {
    A_NONE = 0,
    A_BIND_SIGHT = 1,
    A_MOD_POSSESS = 2,
    A_PERIODIC_DAMAGE = 3,
    A_DUMMY = 4,
    A_MOD_CONFUSE = 5,
    A_MOD_CHARM = 6,
    A_MOD_FEAR = 7,
    A_PERIODIC_HEAL = 8,
    A_MOD_ATTACKSPEED_NORMALIZED = 9,
    A_MOD_THREAT = 10,
    A_MOD_TAUNT = 11,
    A_MOD_STUN = 12,
    A_MOD_DAMAGE_DONE = 13,
    A_MOD_DAMAGE_TAKEN = 14,
    A_DAMAGE_SHIELD = 15,
    A_MOD_STEALTH = 16,
    A_MOD_STEALTH_DETECT = 17,
    A_MOD_INVISIBILITY = 18,
    A_MOD_INVISIBILITY_DETECTION = 19,
    A_PERIODIC_HEAL_PCT = 20,
    A_OBS_MOD_MANA = 21, // 21 unofficial
    A_MOD_RESISTANCE = 22,
    A_PERIODIC_TRIGGER_SPELL = 23,
    A_PERIODIC_ENERGIZE = 24,
    A_MOD_PACIFY = 25,
    A_MOD_ROOT = 26,
    A_MOD_SILENCE = 27,
    A_REFLECT_SPELLS = 28,
    A_MOD_STAT = 29,
    A_MOD_SKILL = 30,
    A_MOD_INCREASE_SPEED = 31,
    A_MOD_INCREASE_MOUNTED_SPEED = 32,
    A_MOD_DECREASE_SPEED = 33,
    A_MOD_INCREASE_HEALTH = 34,
    A_MOD_INCREASE_RESOURCE = 35,
    A_MOD_SHAPESHIFT = 36,
    A_EFFECT_IMMUNITY = 37,
    A_STATE_IMMUNITY = 38,
    A_SCHOOL_IMMUNITY = 39,
    A_DAMAGE_IMMUNITY = 40,
    A_DISPEL_IMMUNITY = 41,
    A_PROC_TRIGGER_SPELL = 42,
    A_PROC_TRIGGER_DAMAGE = 43,
    A_TRACK_CREATURES = 44,
    A_TRACK_RESOURCES = 45,
    A_46 = 46, // Ignore all Gear test spells
    A_MOD_PARRY_PERCENT = 47,
    A_48 = 48, // One periodic spell
    A_MOD_DODGE_PERCENT = 49,
    A_MOD_CRITICAL_HEALING_AMOUNT = 50,
    A_MOD_BLOCK_PERCENT = 51,
    A_MOD_CRIT_PERCENT = 52,
    A_PERIODIC_LEECH = 53,
    A_MOD_HIT_CHANCE = 54,
    A_MOD_SPELL_HIT_CHANCE = 55,
    A_TRANSFORM = 56,
    A_MOD_SPELL_CRIT_CHANCE = 57,
    A_MOD_INCREASE_SWIM_SPEED = 58,
    A_MOD_DAMAGE_DONE_CREATURE = 59,
    A_MOD_PACIFY_SILENCE = 60,
    A_MOD_SCALE = 61,
    A_PERIODIC_HEALTH_FUNNEL = 62,
    A_MOD_MAX_RESOURCE_COST = 63, // old A_PERIODIC_MANA_FUNNEL
    A_PERIODIC_MANA_LEECH = 64,
    A_MOD_CASTING_SPEED_NOT_STACK = 65,
    A_FEIGN_DEATH = 66,
    A_MOD_DISARM = 67,
    A_MOD_STALKED = 68,
    A_SCHOOL_ABSORB = 69,
    A_EXTRA_ATTACKS = 70,
    A_MOD_SPELL_CRIT_CHANCE_SCHOOL = 71,
    A_MOD_POWER_COST_SCHOOL_PCT = 72,
    A_MOD_POWER_COST_SCHOOL = 73,
    A_REFLECT_SPELLS_SCHOOL = 74,
    A_MOD_LANGUAGE = 75,
    A_FAR_SIGHT = 76,
    A_MECHANIC_IMMUNITY = 77,
    A_MOUNTED = 78,
    A_MOD_DAMAGE_PERCENT_DONE = 79,
    A_MOD_PERCENT_STAT = 80,
    A_SPLIT_DAMAGE_PCT = 81,
    A_WATER_BREATHING = 82,
    A_MOD_BASE_RESISTANCE = 83,
    A_RESTORE_HEALTH = 84,
    A_RESTORE_POWER = 85,
    A_CHANNEL_DEATH_ITEM = 86,
    A_MOD_DAMAGE_PERCENT_TAKEN = 87,
    A_MOD_HEALTH_REGEN_PERCENT = 88,
    A_PERIODIC_DAMAGE_PERCENT = 89,
    A_90 = 90, // old A_MOD_RESIST_CHANCE
    A_MOD_DETECT_RANGE = 91,
    A_PREVENTS_FLEEING = 92,
    A_MOD_UNATTACKABLE = 93,
    A_INTERRUPT_REGEN = 94,
    A_GHOST = 95,
    A_SPELL_MAGNET = 96,
    A_MANA_SHIELD = 97,
    A_MOD_SKILL_TALENT = 98,
    A_MOD_ATTACK_POWER = 99,
    A_AURAS_VISIBLE = 100,
    A_MOD_RESISTANCE_PCT = 101,
    A_MOD_MELEE_ATTACK_POWER_VERSUS = 102,
    A_MOD_TOTAL_THREAT = 103,
    A_WATER_WALK = 104,
    A_FEATHER_FALL = 105,
    A_HOVER = 106,
    A_ADD_FLAT_MODIFIER = 107,
    A_ADD_PCT_MODIFIER = 108,
    A_ADD_TARGET_TRIGGER = 109,
    A_MOD_POWER_REGEN_PERCENT = 110,
    A_ADD_CASTER_HIT_TRIGGER = 111,
    A_OVERRIDE_CLASS_SCRIPTS = 112,
    A_MOD_RANGED_DAMAGE_TAKEN = 113,
    A_MOD_RANGED_DAMAGE_TAKEN_PCT = 114,
    A_MOD_HEALING = 115,
    A_MOD_REGEN_DURING_COMBAT = 116,
    A_MOD_MECHANIC_RESISTANCE = 117,
    A_MOD_HEALING_RECEIVED_PCT = 118,
    A_119 = 119, // old A_SHARE_PET_TRACKING
    A_UNTRACKABLE = 120,
    A_EMPATHY = 121,
    A_MOD_OFFHAND_DAMAGE_PCT = 122,
    A_MOD_TARGET_RESISTANCE = 123,
    A_MOD_RANGED_ATTACK_POWER = 124,
    A_MOD_MELEE_DAMAGE_TAKEN = 125,
    A_MOD_MELEE_DAMAGE_TAKEN_PCT = 126,
    A_RANGED_ATTACK_POWER_ATTACKER_BONUS = 127,
    A_MOD_POSSESS_PET = 128,
    A_MOD_SPEED_ALWAYS = 129,
    A_MOD_MOUNTED_SPEED_ALWAYS = 130,
    A_MOD_RANGED_ATTACK_POWER_VERSUS = 131,
    A_MOD_INCREASE_ENERGY_PERCENT = 132,
    A_MOD_INCREASE_HEALTH_PERCENT = 133,
    A_MOD_MANA_REGEN_INTERRUPT = 134,
    A_MOD_HEALING_DONE = 135,
    A_MOD_HEALING_DONE_PERCENT = 136,
    A_MOD_TOTAL_STAT_PERCENTAGE = 137,
    A_MOD_MELEE_HASTE = 138,
    A_FORCE_REACTION = 139,
    A_MOD_RANGED_HASTE = 140,
    A_MOD_RANGED_AMMO_HASTE = 141,
    A_MOD_BASE_RESISTANCE_PCT = 142,
    A_MOD_RECHARGE_RATE_LABEL = 143, // Modifies recharge "rate" of a cooldown by label - rate /= (1 + pct)
    A_SAFE_FALL = 144,
    A_MOD_PET_TALENT_POINTS = 145,
    A_ALLOW_TAME_PET_TYPE = 146,
    A_MECHANIC_IMMUNITY_MASK = 147,
    A_MOD_CHARGE_RECHARGE_RATE = 148, // recharge rate for all charges of a category
    A_REDUCE_PUSHBACK = 149, // Reduce Pushback
    A_MOD_SHIELD_BLOCKVALUE_PCT = 150,
    A_TRACK_STEALTHED = 151, // Track Stealthed
    A_MOD_DETECTED_RANGE = 152, // Mod Detected Range
    A_MOD_AUTO_ATTACK_RANGE = 153,
    A_MOD_STEALTH_LEVEL = 154, // Stealth Level Modifier
    A_MOD_WATER_BREATHING = 155, // Mod Water Breathing
    A_MOD_REPUTATION_GAIN = 156, // Mod Reputation Gain
    A_PET_DAMAGE_MULTI = 157, // Mod Pet Damage
    A_MOD_SHIELD_BLOCKVALUE = 158,
    A_NO_PVP_CREDIT = 159,
    A_MOD_AOE_AVOIDANCE = 160,
    A_MOD_HEALTH_REGEN_IN_COMBAT = 161,
    A_POWER_BURN_MANA = 162,
    A_MOD_CRIT_DAMAGE_BONUS = 163,
    A_164 = 164,
    A_MELEE_ATTACK_POWER_ATTACKER_BONUS = 165,
    A_MOD_ATTACK_POWER_PCT = 166,
    A_MOD_RANGED_ATTACK_POWER_PCT = 167,
    A_MOD_DAMAGE_DONE_VERSUS = 168,
    A_MOD_CRIT_PERCENT_VERSUS = 169,
    A_DETECT_AMORE = 170,
    A_MOD_SPEED_NOT_STACK = 171,
    A_MOD_MOUNTED_SPEED_NOT_STACK = 172,
    A_MOD_CHARGE_TYPE_RECHARGE_RATE = 173, // recharge rate for all charges of a category type
    A_MOD_SPELL_DAMAGE_OF_STAT_PERCENT = 174, // by defeult intelect, dependent from A_MOD_SPELL_HEALING_OF_STAT_PERCENT
    A_MOD_SPELL_HEALING_OF_STAT_PERCENT = 175,
    A_SPIRIT_OF_REDEMPTION = 176,
    A_AOE_CHARM = 177,
    A_MOD_MAX_MANA_PCT = 178,
    A_MOD_ATTACKER_SPELL_CRIT_CHANCE = 179,
    A_MOD_FLAT_SPELL_DAMAGE_VERSUS = 180,
    A_181 = 181, // old A_MOD_FLAT_SPELL_CRIT_DAMAGE_VERSUS - possible flat spell crit damage versus
    A_MOD_RESISTANCE_OF_STAT_PERCENT = 182,
    A_MOD_CRITICAL_THREAT = 183,
    A_MOD_ATTACKER_MELEE_HIT_CHANCE = 184,
    A_MOD_ATTACKER_RANGED_HIT_CHANCE= 185,
    A_MOD_ATTACKER_SPELL_HIT_CHANCE = 186,
    A_MOD_ATTACKER_MELEE_CRIT_CHANCE = 187,
    A_MOD_ATTACKER_RANGED_CRIT_CHANCE = 188,
    A_MOD_RATING = 189,
    A_MOD_FACTION_REPUTATION_GAIN = 190,
    A_USE_NORMAL_MOVEMENT_SPEED = 191,
    A_MOD_MELEE_RANGED_HASTE = 192,
    A_HASTE_ALL = 193,
    A_MOD_IGNORE_ABSORB_SCHOOL = 194,
    A_MOD_IGNORE_ABSORB_FOR_SPELL = 195,
    A_MOD_COOLDOWN = 196, // only 24818 Noxious Breath
    A_MOD_ATTACKER_SPELL_AND_WEAPON_CRIT_CHANCE = 197,
    A_PCT_RATING_ADDED_TO_RATING = 198,
    A_MOD_INCREASES_SPELL_PCT_TO_HIT = 199,
    A_MOD_KILL_XP_PCT = 200,
    A_FLY = 201,
    A_IGNORE_COMBAT_RESULT = 202,
    A_MOD_ATTACKER_MELEE_CRIT_DAMAGE = 203,
    A_MOD_ATTACKER_RANGED_CRIT_DAMAGE = 204,
    A_MOD_ATTACKER_SPELL_CRIT_DAMAGE = 205,
    A_MOD_FLIGHT_SPEED = 206,
    A_MOD_FLIGHT_SPEED_MOUNTED = 207,
    A_MOD_FLIGHT_SPEED_STACKING = 208,
    A_MOD_FLIGHT_SPEED_MOUNTED_STACKING = 209,
    A_MOD_FLIGHT_SPEED_NOT_STACKING = 210,
    A_MOD_FLIGHT_SPEED_MOUNTED_NOT_STACKING = 211,
    A_MOD_RANGED_ATTACK_POWER_OF_STAT_PERCENT = 212,
    A_MOD_RAGE_FROM_DAMAGE_DEALT = 213,
    A_214 = 214,
    A_ARENA_PREPARATION = 215,
    A_HASTE_SPELLS = 216,
    A_217 = 217,
    A_ADD_PCT_LABEL_MODIFIER = 218,
    A_ADD_FLAT_LABEL_MODIFIER = 219,
    A_MODIFY_SCHOOL = 220,
    A_221 = 221,
    A_222 = 222,
    A_223 = 223,
    A_224 = 224,
    A_PRAYER_OF_MENDING = 225,
    A_PERIODIC_DUMMY = 226,
    A_PERIODIC_TRIGGER_SPELL_WITH_VALUE = 227,
    A_DETECT_STEALTH = 228,
    A_MOD_AOE_DAMAGE_AVOIDANCE = 229,
    A_230 = 230,
    A_PROC_TRIGGER_SPELL_WITH_VALUE = 231,
    A_MECHANIC_DURATION_MOD = 232,
    A_233 = 233,
    A_MECHANIC_DURATION_MOD_NOT_STACK = 234,
    A_MOD_DISPEL_RESIST = 235,
    A_CONTROL_VEHICLE = 236,
    A_MOD_SPELL_DAMAGE_OF_ATTACK_POWER = 237,
    A_MOD_SPELL_HEALING_OF_ATTACK_POWER = 238,
    A_MOD_SCALE_2 = 239,
    A_MOD_EXPERTISE = 240,
    A_FORCE_MOVE_FORWARD = 241,
    A_MOD_SPELL_DAMAGE_FROM_HEALING = 242,
    A_243 = 243,
    A_COMPREHEND_LANGUAGE = 244,
    A_MOD_DURATION_OF_MAGIC_EFFECTS = 245,
    A_MOD_DURATION_OF_EFFECTS_BY_DISPEL = 246,
    A_247 = 247,
    A_MOD_COMBAT_RESULT_CHANCE = 248,
    A_CONVERT_RUNE = 249,
    A_MOD_INCREASE_HEALTH_2 = 250,
    A_MOD_ENEMY_DODGE = 251,
    A_SLOW_ALL = 252,
    A_MOD_BLOCK_CRIT_CHANCE = 253,
    A_MOD_DISARM_SHIELD = 254,
    A_MOD_MECHANIC_DAMAGE_TAKEN_PERCENT = 255,
    A_NO_REAGENT_USE = 256,
    A_MOD_TARGET_RESIST_BY_SPELL_CLASS = 257,
    A_258 = 258,
    A_259 = 259,
    A_SCREEN_EFFECT = 260,
    A_PHASE = 261,
    A_262 = 262,
    A_ALLOW_ONLY_ABILITY = 263,
    A_264 = 264,
    A_265 = 265,
    A_266 = 266,
    A_MOD_IMMUNE_A_APPLY_SCHOOL = 267,
    A_MOD_ARMOR_BY_PRIMARY_STAT_PCT = 268,
    A_MOD_DAMAGE_TO_CASTER = 269,
    A_MOD_DAMAGE_FROM_CASTER = 270,
    A_MOD_DAMAGE_FROM_CASTER_SPELLS = 271,
    A_MAELSTROM_WEAPON = 272,
    A_X_RAY = 273,
    A_274 = 274,
    A_MOD_IGNORE_SHAPESHIFT = 275,
    A_276 = 276, // Only "Test Mod Damage % Mechanic" spell, possible mod damage done
    A_MOD_MAX_AFFECTED_TARGETS = 277,
    A_MOD_DISARM_RANGED = 278,
    A_279 = 279,
    A_MOD_TARGET_ARMOR_PCT = 280,
    A_MOD_HONOR_GAIN = 281,
    A_MOD_BASE_HEALTH_PCT = 282,
    A_MOD_HEALING_RECEIVED_FROM_SPELL = 283, // Possibly only for some spell family class spells
    A_284 = 284,
    A_285 = 285, // Some type of trigger buff, possibly on equip/learn
    A_MOD_RECHARGE_RATE = 286,
    A_DEFLECT_SPELLS = 287,
    A_288 = 288,
    A_289 = 289,
    A_MOD_ALL_CRIT_CHANCE = 290,
    A_MOD_QUEST_XP_PCT = 291,
    A_OPEN_STABLE = 292,
    A_293 = 293,
    A_294 = 294,
    A_295 = 295,
    A_296 = 296,
    A_297 = 297,
    A_298 = 298,
    A_299 = 299,
    A_300 = 300,
    A_301 = 301,
    A_302 = 302,
    A_303 = 303,
    A_304 = 304,
    A_MOD_MINIMUM_SPEED = 305,
    A_MOD_CRIT_CHANCE_FROM_CASTER = 306,
    A_307 = 307,
    A_MOD_CRIT_CHANCE_FROM_CASTER_SPELLS = 308,
    A_309 = 309,
    A_310 = 310,
    A_311 = 311,
    A_312 = 312,
    A_313 = 313,
    A_314 = 314,
    A_315 = 315,
    A_316 = 316,
    A_317 = 317,
    A_MOD_MASTERY_PCT = 318,
    A_MOD_MELEE_AUTO_ATTACK_SPEED = 319,
    A_320 = 320,
    A_321 = 321,
    A_322 = 322,
    A_323 = 323,
    A_324 = 324,
    A_325 = 325,
    A_326 = 326,
    A_327 = 327,
    A_328 = 328,
    A_329 = 329,
    A_CAST_WHILE_MOVING_WHITELIST = 330,
    A_331 = 331,
    A_OVERRIDE_ACTION_SPELL = 332,
    A_333 = 333,
    A_334 = 334,
    A_335 = 335,
    A_336 = 336,
    A_337 = 337,
    A_338 = 338,
    A_339 = 339,
    A_340 = 340,
    A_MODIFY_CATEGORY_COOLDOWN = 341,
    A_MOD_RANGED_AND_MELEE_AUTO_ATTACK_SPEED = 342,
    A_MOD_AUTO_ATTACK_FROM_CASTER = 343,
    A_MOD_AUTO_ATTACK_PCT = 344,
    A_MOD_IGNORE_ARMOR_PCT = 345,
    A_346 = 346,
    A_347 = 347,
    A_348 = 348,
    A_349 = 349,
    A_350 = 350,
    A_351 = 351,
    A_352 = 352,
    A_353 = 353,
    A_354 = 354,
    A_355 = 355,
    A_356 = 356,
    A_357 = 357,
    A_358 = 358,
    A_359 = 359,
    A_360 = 360,
    A_OVERIDE_AUTO_ATTACK = 361,
    A_362 = 362,
    A_363 = 363,
    A_364 = 364,
    A_365 = 365,
    A_OVERRIDE_SP_PER_AP = 366,
    A_367 = 367,
    A_371 = 371,
    A_372 = 372,
    A_373 = 373,
    A_376 = 376,
    A_377 = 377,
    A_378 = 378,
    A_MOD_MANA_REGEN_PCT = 379,
    A_MOD_DAMAGE_FROM_CASTER_GUARDIAN = 380,
    A_MOD_DAMAGE_FROM_CASTER_PET = 381,
    A_MOD_PET_STAT = 382,
    A_IGNORE_SPELL_COOLDOWN = 383,
    A_385 = 385,
    A_389 = 389,
    A_392 = 392,
    A_393 = 393,
    A_395 = 395,
    A_396 = 396,
    A_MOD_TIME_RATE = 399,
    A_400 = 400,
    A_402 = 402,
    A_403 = 403,
    A_OVERRIDE_AP_PER_SP = 404,
    A_MOD_RATING_MULTIPLIER = 405,
    A_406 = 406,
    A_407 = 407,
    A_408 = 408,
    A_409 = 409,
    A_410 = 410,
    A_MOD_MAX_CHARGES = 411,
    A_412 = 412,
    A_HASTED_COOLDOWN = 416,
    A_HASTED_GCD = 417,
    A_MOD_MAX_RESOURCE = 418,
    A_MOD_MANA_POOL_PCT = 419,
    A_420 = 420,
    A_MOD_ABSORB_DONE_PERCENT = 421,
    A_MOD_ABSORB_RECEIVED_PERCENT = 422,
    A_423 = 423,
    A_424 = 424,
    A_426 = 426,
    A_428 = 428,
    A_MOD_PET_DAMAGE_DONE = 429,
    A_430 = 430,
    A_440 = 440,
    A_441 = 441,
    A_MOD_LEECH_PERCENT = 443,
    A_446 = 446,
    A_447 = 447,
    A_448 = 448,
    A_451 = 451,
    A_MOD_RECHARGE_TIME_CATEGORY = 453,
    A_MOD_RECHARGE_TIME_PCT_CATEGORY = 454,
    A_455 = 455,
    A_HASTED_CATEGORY = 457,
    A_458 = 458,
    A_MOD_PARRY_FROM_CRIT_RATING = 463,
    A_464 = 464,
    A_465 = 465,
    A_466 = 466,
    A_467 = 467,
    A_468 = 468,
    A_MOD_TIME_RATE_BY_SPELL_LABEL = 470,
    A_MOD_VERSATILITY_PCT = 471,
    A_478 = 478,
    A_481 = 481,
    A_483 = 483,
    A_492 = 492,
    A_MOD_CRIT_DAMAGE_PCT_FROM_CASTER_SPELLS = 501,
    A_MOD_HEALING_RECEIVED_FROM_CASTER = 504,
    A_MOD_DAMAGE_FROM_SPELLS_LABEL = 507,
    A_MOD_GUARDIAN_DAMAGE_DONE = 531,
    A_MOD_DAMAGE_FROM_CASTER_SPELLS_LABEL = 537,
    A_MOD_SUPPORT_STAT = 540,
    A_MAX
};


// New-style spell attributes, indicates bit position
enum spell_attribute : unsigned
{
  SX_RANGED_ABILITY                 = 1u,
  SX_ABILITY                        = 4u,
  SX_TRADESKILL_ABILITY             = 5u,
  SX_PASSIVE                        = 6u,
  SX_HIDDEN                         = 7u,
  SX_REQ_STEALTH                    = 17u,
  SX_CANCEL_AUTO_ATTACK             = 20u,
  SX_NO_D_P_B                       = 21u,
  SX_NO_COMBAT                      = 22u,
  SX_NO_CANCEL                      = 31u,
  SX_CHANNELED                      = 34u,
  SX_NO_STEALTH_BREAK               = 37u,
  SX_CHANNELED_2                    = 38u,
  SX_MELEE_COMBAT_START             = 41u,
  SX_NO_THREAT                      = 42u,
  SX_DISCOUNT_ON_MISS               = 59u,
  SX_DONT_DISPLAY_IN_AURA_BAR       = 60u,
  SX_CANNOT_CRIT                    = 93u,
  SX_FOOD_AURA                      = 95u,
  SX_NOT_A_PROC                     = 105u,
  SX_REQ_MAIN_HAND                  = 106u,
  SX_SUPPRESS_CASTER_PROCS          = 112u,
  SX_SUPPRESS_TARGET_PROCS          = 113u,
  SX_ALWAYS_HIT                     = 114u,
  SX_REQ_OFF_HAND                   = 120u,
  SX_TREAT_AS_PERIODIC              = 121u,
  SX_CAN_PROC_FROM_PROCS            = 122u,
  SX_DISABLE_TARGET_MULT            = 136u,
  SX_DISABLE_WEAPON_PROCS           = 151u,
  SX_TICK_ON_APPLICATION            = 169u,
  SX_DOT_HASTED                     = 173u,
  SX_TREAT_AS_AREA_EFFECT           = 175u,
  SX_REQ_LINE_OF_SIGHT              = 186u,
  SX_IGNORE_FOR_MOD_TIME_RATE       = 196u,
  SX_DISABLE_PLAYER_MULT            = 221u,
  SX_NO_DODGE                       = 247u,
  SX_NO_PARRY                       = 248u,
  SX_NO_MISS                        = 249u,
  SX_CAN_PROC_FROM_SUPPRESSED_TGT   = 254u,
  SX_NO_BLOCK                       = 256u,
  SX_TICK_MAY_CRIT                  = 265u,
  SX_DURATION_HASTED                = 273u,
  SX_REQUIRES_EQUIPPED_ARMOR_TYPE   = 276u,
  SX_DOT_HASTED_MELEE               = 278u,
  SX_MASTERY_AFFECTS_POINTS         = 285u,
  SX_FIXED_TRAVEL_TIME              = 292u,
  SX_DISABLE_PLAYER_HEALING_MULT    = 312u,
  SX_DISABLE_TARGET_POSITIVE_MULT   = 321u,
  SX_TARGET_SPECIFIC_COOLDOWN       = 330u,
  SX_ROLLING_PERIODIC               = 334u,
  SX_SCALE_ILEVEL                   = 354u,
  SX_ENABLE_PROCS_FROM_SUPPRESSED   = 384u,  // requires SX_CAN_PROC_FROM_SUPPRESSED on driver
  SX_CAN_PROC_FROM_SUPPRESSED       = 385u,  // requires SX_ENABLE_PROCS_FROM_SUPPRESSED on action
  SX_ONLY_PROC_FROM_CLASS_ABILITIES = 415u,
  SX_ALLOW_CLASS_ABILITY_PROCS      = 416u,
  SX_REFRESH_EXTENDS_DURATION       = 436u,
};

// spelleffect attributes, indicates bit position
enum spelleffect_attribute : unsigned
{
  EX_NO_IMMUNITY             = 1u,
  EX_EXCLUDE_PARTY           = 5u,
  EX_ALWAYS_AOE_LOS          = 6u,
  EX_SUPPRESS_STACKING       = 7u,
  EX_CHAIN_FROM_INITIAL      = 8u,
  EX_AURA_STACK              = 10u,
  EX_ADD_TARGET_REACH_TO_AOE = 12u,
  EX_IS_HARMFUL              = 13u,
  EX_PLAYERS_ONLY            = 15u,
  EX_COMPUTE_ON_CAST         = 16u,
  EX_ENFORCE_LOS_ON_CHAIN    = 17u,
  EX_AOE_USE_TARGET_RADIUS   = 18u,
  EX_ALWAYS_HIT              = 23u,
  EX_ONLY_AFFECTS_ABSORBS    = 27u,
};

enum spell_label : unsigned
{
  LABEL_COVENANT                   = 976u,
  LABEL_SHARD_OF_DOMINATION_BLOOD  = 1305u,
  LABEL_SHARD_OF_DOMINATION_FROST  = 1306u,
  LABEL_SHARD_OF_DOMINATION_UNHOLY = 1307u,
  LABEL_PRIMORDIAL_STONE           = 2513u,
  LABEL_SINGING_CITRINE            = 3922u,
};

enum difficulty_e : unsigned
{
  NONE = 0,
  DUNGEON = 8,
  NORMAL = 14,
  HEROIC = 15,
  MYTHIC = 16,
  LFR = 17,
};

#endif
