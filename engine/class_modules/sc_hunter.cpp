//==========================================================================
// Dedmonwakeen's DPS-DPM Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include <memory>

#include "simulationcraft.hpp"
#include "player/pet_spawner.hpp"
#include "class_modules/apl/apl_hunter.hpp"

namespace
{ // UNNAMED NAMESPACE

// helper smartpointer-like struct for spell data pointers
struct spell_data_ptr_t
{
  spell_data_ptr_t():
    data_( spell_data_t::not_found() ) {}

  spell_data_ptr_t( const spell_data_t* s ):
    data_( s ? s : spell_data_t::not_found() ) {}

  spell_data_ptr_t& operator=( const spell_data_t* s )
  {
    data_ = s ? s : spell_data_t::not_found();
    return *this;
  }

  const spell_data_t* operator->() const { return data_; }

  operator const spell_data_t*() const { return data_; }

  bool ok() const { return data_ -> ok(); }

  const spell_data_t* data_;
};

static void print_affected_by( const action_t* a, const spelleffect_data_t& effect, util::string_view label = {} )
{
  fmt::memory_buffer out;
  const spell_data_t& spell = *effect.spell();
  const auto& spell_text = a->player->dbc->spell_text( spell.id() );

  fmt::format_to( std::back_inserter(out), "{} {} is affected by {}", *a->player, *a, spell.name_cstr() );
  if ( spell_text.rank() )
    fmt::format_to( std::back_inserter(out), " (desc={})", spell_text.rank() );
  fmt::format_to( std::back_inserter(out), " (id={}) effect#{}", spell.id(), effect.spell_effect_num() + 1 );
  if ( !label.empty() )
    fmt::format_to( std::back_inserter(out), ": {}", label );

  a -> sim -> print_debug( "{}", util::string_view( out.data(), out.size() ) );
}

static bool check_affected_by( action_t* a, const spelleffect_data_t& effect )
{
  bool affected = a->data().affected_by( effect ) || a->data().affected_by_label( effect );
  if ( affected && a->sim->debug )
    print_affected_by( a, effect );
  return affected;
}

struct damage_affected_by {
  uint8_t direct = 0;
  uint8_t tick = 0;
};

static damage_affected_by parse_damage_affecting_aura( action_t* a, spell_data_ptr_t spell )
{
  damage_affected_by affected_by;
  for ( const spelleffect_data_t& effect : spell -> effects() )
  {
    if ( effect.type() != E_APPLY_AURA )
      continue;

    if ( ( effect.subtype() == A_MOD_DAMAGE_FROM_CASTER_SPELLS && a->data().affected_by( effect ) ) ||
         ( effect.subtype() == A_MOD_DAMAGE_FROM_CASTER_SPELLS_LABEL && a->data().affected_by_label( effect ) ) )
    {
      affected_by.direct = as<uint8_t>( effect.spell_effect_num() + 1 );
      affected_by.tick   = as<uint8_t>( effect.spell_effect_num() + 1 );
      print_affected_by( a, effect, "spell damage taken increase" );
      return affected_by;
    }
    
    if ( effect.subtype() != A_ADD_PCT_MODIFIER || !a->data().affected_by( effect ) )
      continue;

    if ( effect.misc_value1() == P_GENERIC )
    {
      affected_by.direct = as<uint8_t>( effect.spell_effect_num() + 1 );
      print_affected_by( a, effect, "direct damage increase" );
    }
    else if ( effect.misc_value1() == P_TICK_DAMAGE )
    {
      affected_by.tick = as<uint8_t>( effect.spell_effect_num() + 1 );
      print_affected_by( a, effect, "tick damage increase" );
    }
  }
  return affected_by;
}

namespace cdwaste {

struct action_data_t
{
  simple_sample_data_with_min_max_t exec;
  simple_sample_data_with_min_max_t cumulative;
  timespan_t iter_sum;

  void update_ready( const action_t* action, timespan_t cd )
  {
    const cooldown_t* cooldown = action -> cooldown;
    sim_t* sim = action -> sim;
    if ( ( cd > 0_ms || ( cd <= 0_ms && cooldown -> duration > 0_ms ) ) &&
         cooldown -> current_charge == cooldown -> charges && cooldown -> last_charged > 0_ms &&
         cooldown -> last_charged < sim -> current_time() )
    {
      timespan_t time_ = sim -> current_time() - cooldown -> last_charged;
      if ( sim -> debug )
      {
        sim -> out_debug.print( "{} {} cooldown waste tracking waste={} exec_time={}",
                                action -> player -> name(), action -> name(),
                                time_, action -> time_to_execute );
      }
      time_ -= action -> time_to_execute;

      if ( time_ > 0_ms )
      {
        exec.add( time_.total_seconds() );
        iter_sum += time_;
      }
    }
  }
};

struct player_data_t
{
  using record_t = std::pair<std::string, std::unique_ptr<action_data_t>>;
  std::vector<record_t> data_;

  action_data_t* get( const action_t* a )
  {
    auto it = range::find( data_, a -> name_str, &record_t::first );
    if ( it != data_.cend() )
      return it -> second.get();

    data_.emplace_back( a -> name_str, std::make_unique<action_data_t>( ) );
    return data_.back().second.get();
  }

  void merge( const player_data_t& other )
  {
    for ( size_t i = 0, end = data_.size(); i < end; i++ )
    {
      data_[ i ].second -> exec.merge( other.data_[ i ].second -> exec );
      data_[ i ].second -> cumulative.merge( other.data_[ i ].second -> cumulative );
    }
  }

  void datacollection_begin()
  {
    for ( auto& rec : data_ )
      rec.second -> iter_sum = 0_ms;
  }

  void datacollection_end()
  {
    for ( auto& rec : data_ )
      rec.second -> cumulative.add( rec.second -> iter_sum.total_seconds() );
  }
};

void print_html_report( const player_t& player, const player_data_t& data, report::sc_html_stream& os )
{
  if ( data.data_.empty() )
    return;

  os << "<h3 class='toggle open'>Cooldown waste details</h3>\n"
     << "<div class='toggle-content'>\n";

  os << "<table class='sc' style='float: left;margin-right: 10px;'>\n"
     << "<tr>"
     << "<th></th>"
     << "<th colspan='3'>Seconds per Execute</th>"
     << "<th colspan='3'>Seconds per Iteration</th>"
     << "</tr>\n"
     << "<tr>"
     << "<th>Ability</th>"
     << "<th>Average</th><th>Minimum</th><th>Maximum</th>"
     << "<th>Average</th><th>Minimum</th><th>Maximum</th>"
     << "</tr>\n";

  size_t n = 0;
  for ( const auto& rec : data.data_ )
  {
    const auto& entry = rec.second -> exec;
    if ( entry.count() == 0 )
      continue;

    const auto& iter_entry = rec.second -> cumulative;
    const action_t* a = player.find_action( rec.first );

    ++n;
    fmt::print( os,
      "<tr{}>"
      "<td class='left'>{}</td>"
      "<td class='right'>{:.3f}</td><td class='right'>{:.3f}</td><td class='right'>{:.3f}</td>"
      "<td class='right'>{:.3f}</td><td class='right'>{:.3f}</td><td class='right'>{:.3f}</td>"
      "</tr>\n",
      n & 1 ? " class='odd'" : "",
      a ? report_decorators::decorated_action( *a ) : util::encode_html( rec.first ),
      entry.mean(), entry.min(), entry.max(),
      iter_entry.mean(), iter_entry.min(), iter_entry.max()
    );
  }

  os << "</table>\n"
     << "</div>\n"
     << "<div class='clear'></div>\n";
}

} // end namespace cd_waste

// ==========================================================================
// Hunter
// ==========================================================================

// in-game the buffs are actually 8 distinct spells, so the player can't get more than 8 simultaneously
constexpr unsigned BARBED_SHOT_BUFFS_MAX = 8;

enum howl_of_the_pack_leader_beast
{
  WYVERN,
  BOAR,
  BEAR
};

struct maybe_bool {

  enum class value_e : uint8_t {
    None, True, False
  };

  constexpr maybe_bool() = default;

  constexpr maybe_bool& operator=( bool val ) {
    set( val );
    return *this;
  }

  constexpr void set( bool val ) {
    value_ = val ? value_e::True : value_e::False;
  }

  constexpr bool is_none() const { return value_ == value_e::None; }

  constexpr operator bool() const { return value_ == value_e::True; }

  value_e value_ = value_e::None;
};

template <typename Data, typename Base = action_state_t>
struct hunter_action_state_t : public Base, public Data
{
  static_assert( std::is_base_of_v<action_state_t, Base> );
  static_assert( std::is_default_constructible_v<Data> ); // required for initialize
  static_assert( std::is_copy_assignable_v<Data> ); // required for copy_state

  using Base::Base;

  void initialize() override
  {
    Base::initialize();
    *static_cast<Data*>( this ) = Data{};
  }

  std::ostringstream& debug_str( std::ostringstream& s ) override
  {
    Base::debug_str( s );
    if constexpr ( fmt::is_formattable<Data>::value )
      fmt::print( s, " {}", *static_cast<const Data*>( this ) );
    return s;
  }

  void copy_state( const action_state_t* o ) override
  {
    Base::copy_state( o );
    *static_cast<Data*>( this ) = *static_cast<const Data*>( debug_cast<const hunter_action_state_t*>( o ) );
  }
};

struct pet_amount_expr_t : public expr_t
{
public:
  action_t& action;
  action_t& pet_action;
  action_state_t* state;

  pet_amount_expr_t( util::string_view name, action_t& a, action_t& pet_a )
    : expr_t( name ), action( a ), pet_action( pet_a ), state( pet_a.get_state() )
  {
    state->n_targets = 1;
    state->chain_target = 0;
    state->result = RESULT_HIT;
  }

  double evaluate() override
  {
    state->target = action.target;
    pet_action.snapshot_state( state, result_amount_type::DMG_DIRECT );

    state->result_amount = pet_action.calculate_direct_amount( state );
    state->target->target_mitigation( action.get_school(), result_amount_type::DMG_DIRECT, state );

    return state->result_amount;
  }

  ~pet_amount_expr_t() override
  {
    delete state;
  }
};

struct hunter_t;

namespace pets
{
struct dire_critter_t;
struct dire_beast_t;
struct dark_hound_t;
struct fenryr_t;
struct hati_t;
struct bear_t;
struct stable_pet_t;
struct call_of_the_wild_pet_t;
struct hunter_main_pet_base_t;
struct animal_companion_t;
struct hunter_main_pet_t;
}

namespace events
{
struct tar_trap_aoe_t;
}

struct hunter_td_t: public actor_target_data_t
{
  bool damaged = false;
  bool sentinel_imploding = false;

  struct cooldowns_t
  {
    cooldown_t* overwatch;
  } cooldowns;

  struct debuffs_t
  {
    buff_t* cull_the_herd;
    buff_t* wild_instincts;
    buff_t* outland_venom;

    buff_t* spotters_mark;
    buff_t* shrapnel_shot;
    buff_t* kill_zone;
    buff_t* ohnahran_winds;

    buff_t* sentinel;
    buff_t* crescent_steel;
    buff_t* lunar_storm;
  } debuffs;

  struct dots_t
  {
    dot_t* explosive_shot;
    dot_t* serpent_sting;
    dot_t* a_murder_of_crows;
    
    dot_t* barbed_shot;
    dot_t* laceration;

    dot_t* wildfire_bomb;
    dot_t* merciless_blow;
    dot_t* spearhead;

    dot_t* black_arrow;
  } dots;

  hunter_td_t( player_t* target, hunter_t* p );

  void target_demise();
};

struct hunter_t final : public player_t
{
public:

  struct pets_t
  {
    pets::hunter_main_pet_t* main = nullptr;
    pets::animal_companion_t* animal_companion = nullptr;
    spawner::pet_spawner_t<pets::dire_beast_t, hunter_t> dire_beast;
    spawner::pet_spawner_t<pets::dark_hound_t, hunter_t> dark_hound;
    spawner::pet_spawner_t<pets::fenryr_t, hunter_t> fenryr;
    spawner::pet_spawner_t<pets::hati_t, hunter_t> hati;
    spawner::pet_spawner_t<pets::bear_t, hunter_t> bear;
    spawner::pet_spawner_t<pets::call_of_the_wild_pet_t, hunter_t> cotw_stable_pet;

    pets_t( hunter_t* p ) : 
      dire_beast( "dire_beast", p ),
      dark_hound( "dark_hound", p ),
      fenryr( "fenryr", p ),
      hati( "hati", p ),
      bear( "bear", p ),
      cotw_stable_pet( "call_of_the_wild_pet", p )
    {
    }
  } pets;

  struct tier_sets_t
  {
    // TWW Season 1 - Nerub'ar Palace
    spell_data_ptr_t tww_s1_bm_2pc;
    spell_data_ptr_t tww_s1_bm_4pc;
    spell_data_ptr_t tww_s1_mm_2pc;
    spell_data_ptr_t tww_s1_mm_4pc;
    spell_data_ptr_t tww_s1_sv_2pc;
    spell_data_ptr_t tww_s1_sv_4pc;

    // TWW Season 2 - Liberation of Undermine
    spell_data_ptr_t tww_s2_bm_2pc;
    spell_data_ptr_t tww_s2_bm_4pc;
    spell_data_ptr_t tww_s2_mm_2pc;
    spell_data_ptr_t tww_s2_mm_4pc;
    spell_data_ptr_t tww_s2_sv_2pc;
    spell_data_ptr_t tww_s2_sv_4pc;
  } tier_set;

  struct buffs_t
  {
    // Hunter Tree
    buff_t* deathblow;

    // Marksmanship Tree
    buff_t* precise_shots;
    buff_t* streamline;
    buff_t* trick_shots;
    buff_t* lock_and_load;
    buff_t* in_the_rhythm;
    buff_t* on_target;
    buff_t* trueshot;
    buff_t* moving_target;
    buff_t* precision_detonation_hidden;
    buff_t* razor_fragments;
    buff_t* bullseye;
    buff_t* bulletstorm;
    buff_t* volley;
    buff_t* double_tap;

    // Beast Mastery Tree
    std::array<buff_t*, BARBED_SHOT_BUFFS_MAX> barbed_shot;
    buff_t* thrill_of_the_hunt;
    buff_t* dire_beast;
    buff_t* bestial_wrath;
    buff_t* call_of_the_wild;
    buff_t* beast_cleave; 
    buff_t* serpentine_rhythm;
    buff_t* serpentine_blessing;
    buff_t* a_murder_of_crows;
    buff_t* huntmasters_call;
    buff_t* summon_fenryr;
    buff_t* summon_hati;

    // Survival Tree
    buff_t* tip_of_the_spear;
    buff_t* tip_of_the_spear_explosive;
    buff_t* tip_of_the_spear_fote;
    buff_t* mongoose_fury;
    buff_t* wildfire_arsenal;
    buff_t* frenzy_strikes;
    buff_t* sulfurlined_pockets_building;
    buff_t* sulfurlined_pockets_ready;
    buff_t* bloodseeker;
    buff_t* aspect_of_the_eagle;
    buff_t* terms_of_engagement;
    buff_t* coordinated_assault;
    buff_t* ruthless_marauder;
    buff_t* relentless_primal_ferocity;
    buff_t* bombardier;

    // Pet family buffs
    buff_t* endurance_training;
    buff_t* pathfinding;
    buff_t* predators_thirst;

    // Tier Set Bonuses
    // TWW - S1
    buff_t* harmonize; // BM 4pc
    // TWW - S2
    buff_t* jackpot; // MM 2pc
    buff_t* winning_streak; // SV 2pc - Wildfire Bomb damage stacking buff
    buff_t* strike_it_rich; // SV 4pc - Mongoose Bite damage buff, consuming it reduces Wildfire Bomb cooldown

    // Hero Talents 

    // Pack Leader
    buff_t* howl_of_the_pack_leader_wyvern;
    buff_t* howl_of_the_pack_leader_boar;
    buff_t* howl_of_the_pack_leader_bear;
    buff_t* howl_of_the_pack_leader_cooldown;
    buff_t* wyverns_cry;
    buff_t* hogstrider;
    buff_t* lead_from_the_front;

    // Sentinel
    buff_t* eyes_closed;
    buff_t* lunar_storm_ready;
    buff_t* lunar_storm_cooldown;

    // Dark Ranger
    buff_t* withering_fire;
  } buffs;

  struct cooldowns_t
  {
    cooldown_t* kill_shot;
    cooldown_t* explosive_shot;
    
    cooldown_t* aimed_shot;
    cooldown_t* rapid_fire;
    cooldown_t* trueshot;
    cooldown_t* target_acquisition;
    cooldown_t* volley;
    cooldown_t* salvo;
    
    cooldown_t* kill_command;

    cooldown_t* barbed_shot;
    cooldown_t* bestial_wrath;

    cooldown_t* wildfire_bomb;
    cooldown_t* butchery;
    cooldown_t* harpoon;
    cooldown_t* flanking_strike;
    cooldown_t* fury_of_the_eagle;
    cooldown_t* ruthless_marauder;
    cooldown_t* coordinated_assault;

    cooldown_t* no_mercy;

    cooldown_t* black_arrow;
    cooldown_t* bleak_powder;
    cooldown_t* banshees_mark;
  } cooldowns;

  struct gains_t
  {
    gain_t* barbed_shot;
    gain_t* dire_beast;

    gain_t* terms_of_engagement;

    gain_t* invigorating_pulse;
  } gains;

  struct procs_t
  {
    proc_t* snakeskin_quiver;
    proc_t* wild_call;
    proc_t* wild_instincts;
    proc_t* dire_command;

    proc_t* deathblow;

    proc_t* precision_detonation;
    proc_t* tww_s2_mm_4pc_explosive;

    proc_t* sentinel_stacks;
    proc_t* sentinel_implosions;
    proc_t* extrapolated_shots_stacks;
    proc_t* release_and_reload_stacks;
    proc_t* crescent_steel_stacks;
    proc_t* overwatch_implosions;
  } procs;

  struct rppm_t
  {
    real_ppm_t* shadow_hounds;
    real_ppm_t* shadow_surge;
  } rppm;

  struct talents_t
  {
    // Hunter Tree
    spell_data_ptr_t kill_shot;

    spell_data_ptr_t deathblow; 
    spell_data_ptr_t deathblow_buff;

    spell_data_ptr_t tar_trap;

    spell_data_ptr_t counter_shot;
    spell_data_ptr_t muzzle;

    spell_data_ptr_t lone_survivor;
    spell_data_ptr_t specialized_arsenal;

    spell_data_ptr_t explosive_shot;
    spell_data_ptr_t explosive_shot_cast;
    spell_data_ptr_t explosive_shot_damage;

    spell_data_ptr_t bursting_shot;
    spell_data_ptr_t trigger_finger;
    spell_data_ptr_t blackrock_munitions; 
    spell_data_ptr_t keen_eyesight;

    spell_data_ptr_t serrated_tips;
    spell_data_ptr_t born_to_be_wild;
    spell_data_ptr_t improved_traps;

    spell_data_ptr_t high_explosive_trap;
    spell_data_ptr_t implosive_trap;
    spell_data_ptr_t explosive_trap_damage;

    spell_data_ptr_t unnatural_causes;
    spell_data_ptr_t unnatural_causes_debuff;
    
    // Beast Mastery/Survival
    spell_data_ptr_t kill_command;
    spell_data_ptr_t kill_command_pet;
    spell_data_ptr_t alpha_predator;

    // Marksmanship Tree
    spell_data_ptr_t aimed_shot;

    spell_data_ptr_t rapid_fire;
    spell_data_ptr_t rapid_fire_tick;
    spell_data_ptr_t rapid_fire_energize;
    spell_data_ptr_t precise_shots;
    spell_data_ptr_t precise_shots_buff;

    spell_data_ptr_t streamline;
    spell_data_ptr_t streamline_buff;
    spell_data_ptr_t trick_shots;
    spell_data_ptr_t trick_shots_data;
    spell_data_ptr_t trick_shots_buff;
    spell_data_ptr_t aspect_of_the_hydra;
    spell_data_ptr_t ammo_conservation;
    
    spell_data_ptr_t penetrating_shots;
    spell_data_ptr_t improved_spotters_mark;
    spell_data_ptr_t unbreakable_bond;
    spell_data_ptr_t lock_and_load;
    
    spell_data_ptr_t in_the_rhythm;
    spell_data_ptr_t in_the_rhythm_buff;
    spell_data_ptr_t surging_shots;
    spell_data_ptr_t master_marksman;
    spell_data_ptr_t master_marksman_bleed;
    spell_data_ptr_t quickdraw;

    spell_data_ptr_t improved_deathblow;
    spell_data_ptr_t obsidian_arrowhead;
    spell_data_ptr_t on_target;
    spell_data_ptr_t on_target_buff;
    spell_data_ptr_t trueshot;
    spell_data_ptr_t moving_target;
    spell_data_ptr_t moving_target_buff;
    spell_data_ptr_t precision_detonation;
    spell_data_ptr_t precision_detonation_buff;

    spell_data_ptr_t razor_fragments;
    spell_data_ptr_t razor_fragments_bleed;
    spell_data_ptr_t razor_fragments_buff;
    spell_data_ptr_t headshot;
    spell_data_ptr_t deadeye;
    spell_data_ptr_t no_scope;
    spell_data_ptr_t feathered_frenzy;
    spell_data_ptr_t target_acquisition;
    spell_data_ptr_t shrapnel_shot;
    spell_data_ptr_t shrapnel_shot_debuff;
    spell_data_ptr_t magnetic_gunpowder;
    
    spell_data_ptr_t eagles_accuracy;
    spell_data_ptr_t calling_the_shots;
    spell_data_ptr_t bullseye;
    spell_data_ptr_t bullseye_buff;

    spell_data_ptr_t improved_streamline;
    spell_data_ptr_t focused_aim;
    spell_data_ptr_t killer_mark;
    spell_data_ptr_t bulletstorm;
    spell_data_ptr_t bulletstorm_buff;
    spell_data_ptr_t tensile_bowstring;
    spell_data_ptr_t volley;
    spell_data_ptr_t volley_data;
    spell_data_ptr_t volley_dmg;
    spell_data_ptr_t ohnahran_winds;
    spell_data_ptr_t ohnahran_winds_debuff;
    spell_data_ptr_t small_game_hunter;

    spell_data_ptr_t windrunner_quiver;
    spell_data_ptr_t incendiary_ammunition;
    spell_data_ptr_t double_tap;
    spell_data_ptr_t double_tap_buff;
    spell_data_ptr_t unerring_vision;
    spell_data_ptr_t kill_zone;
    spell_data_ptr_t kill_zone_debuff;
    spell_data_ptr_t salvo;
    spell_data_ptr_t bullet_hell;

    // Beast Mastery Tree
    spell_data_ptr_t cobra_shot;
    spell_data_ptr_t animal_companion;
    spell_data_ptr_t solitary_companion;
    spell_data_ptr_t barbed_shot;

    spell_data_ptr_t pack_tactics;
    spell_data_ptr_t aspect_of_the_beast;
    spell_data_ptr_t war_orders;
    spell_data_ptr_t thrill_of_the_hunt;

    spell_data_ptr_t go_for_the_throat;
    spell_data_ptr_t multishot_bm;
    spell_data_ptr_t laceration;
    spell_data_ptr_t laceration_driver;
    spell_data_ptr_t laceration_bleed;

    spell_data_ptr_t barbed_scales;
    spell_data_ptr_t snakeskin_quiver;
    spell_data_ptr_t cobra_senses;
    spell_data_ptr_t beast_cleave;
    spell_data_ptr_t wild_call;
    spell_data_ptr_t hunters_prey;
    spell_data_ptr_t hunters_prey_hidden_buff;
    spell_data_ptr_t poisoned_barbs;

    spell_data_ptr_t stomp;
    spell_data_ptr_t stomp_primary;
    spell_data_ptr_t stomp_cleave;
    spell_data_ptr_t serpentine_rhythm;
    spell_data_ptr_t kill_cleave;
    spell_data_ptr_t training_expert;
    spell_data_ptr_t dire_beast;

    spell_data_ptr_t a_murder_of_crows;
    spell_data_ptr_t a_murder_of_crows_dot;
    spell_data_ptr_t barrage;
    spell_data_ptr_t savagery;
    spell_data_ptr_t bestial_wrath;
    spell_data_ptr_t dire_command;
    spell_data_ptr_t huntmasters_call;
    spell_data_ptr_t dire_cleave;

    spell_data_ptr_t killer_instinct;
    spell_data_ptr_t master_handler;
    spell_data_ptr_t barbed_wrath;
    spell_data_ptr_t thundering_hooves;
    spell_data_ptr_t dire_frenzy;
    
    spell_data_ptr_t call_of_the_wild;
    spell_data_ptr_t killer_cobra;
    spell_data_ptr_t scent_of_blood;
    spell_data_ptr_t brutal_companion;
    spell_data_ptr_t bloodshed;
    spell_data_ptr_t bloodshed_dot;
    spell_data_ptr_t bloodshed_debuff;

    spell_data_ptr_t wild_instincts;
    spell_data_ptr_t bloody_frenzy;
    spell_data_ptr_t piercing_fangs;
    spell_data_ptr_t venomous_bite;
    spell_data_ptr_t venomous_bite_debuff;
    spell_data_ptr_t shower_of_blood;

    // Survival Tree
    spell_data_ptr_t wildfire_bomb;
    spell_data_ptr_t wildfire_bomb_data;
    spell_data_ptr_t wildfire_bomb_dmg;
    spell_data_ptr_t wildfire_bomb_dot;
    spell_data_ptr_t raptor_strike;
    spell_data_ptr_t raptor_strike_eagle;

    spell_data_ptr_t guerrilla_tactics;
    spell_data_ptr_t tip_of_the_spear;
    spell_data_ptr_t tip_of_the_spear_buff;
    spell_data_ptr_t tip_of_the_spear_explosive_buff;
    spell_data_ptr_t tip_of_the_spear_fote_buff;

    spell_data_ptr_t lunge;
    spell_data_ptr_t quick_shot;
    spell_data_ptr_t mongoose_bite;
    spell_data_ptr_t mongoose_bite_eagle;
    spell_data_ptr_t mongoose_fury;
    spell_data_ptr_t flankers_advantage;

    spell_data_ptr_t wildfire_infusion;
    spell_data_ptr_t wildfire_arsenal;
    spell_data_ptr_t wildfire_arsenal_buff;
    spell_data_ptr_t sulfurlined_pockets;
    spell_data_ptr_t sulfurlined_pockets_building_buff;
    spell_data_ptr_t sulfurlined_pockets_ready_buff;
    spell_data_ptr_t butchery;
    spell_data_ptr_t flanking_strike;
    spell_data_ptr_t flanking_strike_player;
    spell_data_ptr_t flanking_strike_pet;
    spell_data_ptr_t bloody_claws;
    spell_data_ptr_t ranger;

    spell_data_ptr_t grenade_juggler;
    spell_data_ptr_t cull_the_herd;
    spell_data_ptr_t cull_the_herd_debuff;
    spell_data_ptr_t frenzy_strikes;
    spell_data_ptr_t frenzy_strikes_buff;
    spell_data_ptr_t merciless_blow;
    spell_data_ptr_t merciless_blow_flanking_bleed;
    spell_data_ptr_t merciless_blow_butchery_bleed;
    spell_data_ptr_t vipers_venom;
    spell_data_ptr_t bloodseeker;

    spell_data_ptr_t terms_of_engagement;
    spell_data_ptr_t terms_of_engagement_dmg;
    spell_data_ptr_t terms_of_engagement_buff;
    spell_data_ptr_t born_to_kill;
    spell_data_ptr_t tactical_advantage;
    spell_data_ptr_t sic_em;
    spell_data_ptr_t contagious_reagents;
    spell_data_ptr_t outland_venom;
    spell_data_ptr_t outland_venom_debuff;
    
    spell_data_ptr_t explosives_expert;
    spell_data_ptr_t sweeping_spear;
    spell_data_ptr_t killer_companion;
    
    spell_data_ptr_t fury_of_the_eagle;
    spell_data_ptr_t coordinated_assault;
    spell_data_ptr_t coordinated_assault_dmg;
    spell_data_ptr_t spearhead;
    spell_data_ptr_t spearhead_bleed;
    spell_data_ptr_t spearhead_debuff;

    spell_data_ptr_t ruthless_marauder;
    spell_data_ptr_t ruthless_marauder_buff;
    spell_data_ptr_t symbiotic_adrenaline;
    spell_data_ptr_t relentless_primal_ferocity;
    spell_data_ptr_t relentless_primal_ferocity_buff;
    spell_data_ptr_t bombardier;
    spell_data_ptr_t bombardier_buff;
    spell_data_ptr_t deadly_duo;

    // Dark Ranger
    spell_data_ptr_t black_arrow;
    spell_data_ptr_t black_arrow_spell;
    spell_data_ptr_t black_arrow_dot;
    
    spell_data_ptr_t bleak_arrows;
    spell_data_ptr_t bleak_arrows_spell;
    spell_data_ptr_t shadow_hounds;
    spell_data_ptr_t shadow_hounds_summon;
    spell_data_ptr_t soul_drinker;
    spell_data_ptr_t the_bell_tolls;

    spell_data_ptr_t phantom_pain;
    spell_data_ptr_t phantom_pain_spell;
    spell_data_ptr_t ebon_bowstring;

    spell_data_ptr_t banshees_mark; 
    spell_data_ptr_t shadow_surge;
    spell_data_ptr_t shadow_surge_spell;
    spell_data_ptr_t bleak_powder;
    spell_data_ptr_t bleak_powder_spell;

    spell_data_ptr_t withering_fire;
    spell_data_ptr_t withering_fire_black_arrow;
    spell_data_ptr_t withering_fire_buff;


    // Pack Leader
    spell_data_ptr_t howl_of_the_pack_leader;
    spell_data_ptr_t howl_of_the_pack_leader_wyvern_ready_buff;
    spell_data_ptr_t howl_of_the_pack_leader_boar_ready_buff;
    spell_data_ptr_t howl_of_the_pack_leader_bear_ready_buff;
    spell_data_ptr_t howl_of_the_pack_leader_cooldown_buff;
    spell_data_ptr_t howl_of_the_pack_leader_wyvern_summon;
    spell_data_ptr_t howl_of_the_pack_leader_wyvern_buff;
    spell_data_ptr_t howl_of_the_pack_leader_boar_charge_trigger;
    spell_data_ptr_t howl_of_the_pack_leader_boar_charge_impact;
    spell_data_ptr_t howl_of_the_pack_leader_boar_charge_cleave;
    spell_data_ptr_t howl_of_the_pack_leader_bear_summon;
    spell_data_ptr_t howl_of_the_pack_leader_bear_buff;
    spell_data_ptr_t howl_of_the_pack_leader_bear_bleed;

    spell_data_ptr_t pack_mentality;
    spell_data_ptr_t dire_summons;
    spell_data_ptr_t better_together;

    spell_data_ptr_t ursine_fury;
    spell_data_ptr_t ursine_fury_chance;
    spell_data_ptr_t envenomed_fangs;
    spell_data_ptr_t envenomed_fangs_spell;
    spell_data_ptr_t fury_of_the_wyvern;
    spell_data_ptr_t fury_of_the_wyvern_proc;
    spell_data_ptr_t hogstrider;
    spell_data_ptr_t hogstrider_buff;

    spell_data_ptr_t no_mercy;
    
    spell_data_ptr_t lead_from_the_front;
    spell_data_ptr_t lead_from_the_front_buff;

    // Sentinel
    // TODO chance to stack and chance to implode were and are still pretty rough estimates
    spell_data_ptr_t sentinel;
    spell_data_ptr_t sentinel_debuff;
    spell_data_ptr_t sentinel_tick;

    spell_data_ptr_t extrapolated_shots;
    spell_data_ptr_t sentinel_precision;

    spell_data_ptr_t release_and_reload;
    // TODO the alleged decreased chance is unknown and not modeled
    spell_data_ptr_t invigorating_pulse;

    spell_data_ptr_t sentinel_watch;
    spell_data_ptr_t eyes_closed;
    spell_data_ptr_t symphonic_arsenal;
    spell_data_ptr_t symphonic_arsenal_spell;
    spell_data_ptr_t overwatch;
    spell_data_ptr_t crescent_steel;
    spell_data_ptr_t crescent_steel_debuff;

    spell_data_ptr_t lunar_storm;
    spell_data_ptr_t lunar_storm_initial_spell;
    spell_data_ptr_t lunar_storm_periodic_trigger;
    spell_data_ptr_t lunar_storm_periodic_spell;
    spell_data_ptr_t lunar_storm_ready_buff;
    spell_data_ptr_t lunar_storm_cooldown_buff;
  } talents;

  // Specialization Spells
  struct specs_t
  {
    spell_data_ptr_t critical_strikes;
    spell_data_ptr_t hunter;
    spell_data_ptr_t beast_mastery_hunter;
    spell_data_ptr_t marksmanship_hunter;
    spell_data_ptr_t survival_hunter;

    spell_data_ptr_t auto_shot;
    spell_data_ptr_t freezing_trap;
    spell_data_ptr_t arcane_shot;
    spell_data_ptr_t steady_shot;
    spell_data_ptr_t steady_shot_energize;
    spell_data_ptr_t flare;
    spell_data_ptr_t serpent_sting;
    spell_data_ptr_t call_pet;

    // SV
    spell_data_ptr_t harpoon;

    // MM
    spell_data_ptr_t multishot;
    spell_data_ptr_t eyes_in_the_sky;
    spell_data_ptr_t spotters_mark_debuff;
    spell_data_ptr_t harriers_cry;
  } specs;

  struct mastery_spells_t
  {
    spell_data_ptr_t master_of_beasts; // BM
    spell_data_ptr_t sniper_training; // MM
    spell_data_ptr_t spirit_bond; // SV
    spell_data_ptr_t spirit_bond_buff;
  } mastery;

  struct {
    action_t* barbed_shot = nullptr;
    action_t* snakeskin_quiver = nullptr;
    action_t* dire_command = nullptr;
    action_t* laceration = nullptr;

    action_t* a_murder_of_crows = nullptr;

    action_t* boar_charge = nullptr;

    action_t* sentinel = nullptr;
    action_t* symphonic_arsenal = nullptr;
    action_t* lunar_storm_initial = nullptr;
    action_t* lunar_storm_periodic = nullptr;

    action_t* phantom_pain = nullptr;
  } actions;

  cdwaste::player_data_t cd_waste;

  struct {
    events::tar_trap_aoe_t* tar_trap_aoe = nullptr;
    timespan_t tensile_bowstring_extension = 0_s;
    event_t* current_volley = nullptr;
    action_t* traveling_explosive = nullptr;
    timespan_t sentinel_watch_reduction = 0_s;
    howl_of_the_pack_leader_beast howl_of_the_pack_leader_next_beast = WYVERN;
    timespan_t fury_of_the_wyvern_extension = 0_s;
  } state;

  struct options_t {
    std::string summon_pet_str = "duck";
    timespan_t pet_attack_speed = 2_s;
    timespan_t pet_basic_attack_delay = 0.15_s;
  } options;

  hunter_t( sim_t* sim, util::string_view name, race_e r = RACE_NONE ) :
    player_t( sim, HUNTER, name, r ),
    pets( this ),
    buffs(),
    cooldowns(),
    gains(),
    procs()
  {
    cooldowns.kill_shot       = get_cooldown( "kill_shot" );
    cooldowns.explosive_shot  = get_cooldown( "explosive_shot" );

    cooldowns.aimed_shot                = get_cooldown( "aimed_shot" );
    cooldowns.rapid_fire                = get_cooldown( "rapid_fire" );
    cooldowns.trueshot                  = get_cooldown( "trueshot" );
    cooldowns.target_acquisition        = get_cooldown( "target_acquisition_icd" );
    cooldowns.volley                    = get_cooldown( "volley" );
    cooldowns.salvo                     = get_cooldown( "salvo_icd" );
    
    cooldowns.kill_command = get_cooldown( "kill_command" );
    cooldowns.barbed_shot   = get_cooldown( "barbed_shot" );
    cooldowns.bestial_wrath = get_cooldown( "bestial_wrath" );

    cooldowns.wildfire_bomb       = get_cooldown( "wildfire_bomb" );
    cooldowns.butchery            = get_cooldown( "butchery" );
    cooldowns.harpoon             = get_cooldown( "harpoon" );
    cooldowns.flanking_strike     = get_cooldown( "flanking_strike");
    cooldowns.fury_of_the_eagle   = get_cooldown( "fury_of_the_eagle" );
    cooldowns.ruthless_marauder   = get_cooldown( "ruthless_marauder" );
    cooldowns.coordinated_assault = get_cooldown( "coordinated_assault" );

    cooldowns.no_mercy = get_cooldown( "no_mercy" );

    cooldowns.black_arrow = get_cooldown( "black_arrow" );
    cooldowns.bleak_powder = get_cooldown( "bleak_powder_icd" );
    cooldowns.banshees_mark = get_cooldown( "banshees_mark" );

    base_gcd = 1.5_s;

    resource_regeneration = regen_type::DYNAMIC;
    regen_caches[ CACHE_HASTE ] = true;
    regen_caches[ CACHE_ATTACK_HASTE ] = true;
  }

  // Character Definition
  void init() override;
  void init_spells() override;
  void init_base_stats() override;
  void create_actions() override;
  void create_buffs() override;
  void init_gains() override;
  void init_position() override;
  void init_procs() override;
  void init_rng() override;
  void init_scaling() override;
  void init_assessors() override;
  void init_action_list() override;
  void init_special_effects() override;
  void init_finished() override;
  void reset() override;
  void merge( player_t& other ) override;
  void arise() override;
  void combat_begin() override;

  void datacollection_begin() override;
  void datacollection_end() override;

  double composite_melee_crit_chance() const override;
  double composite_spell_crit_chance() const override;
  double composite_rating_multiplier( rating_e ) const override;
  double composite_melee_auto_attack_speed() const override;
  double composite_player_critical_damage_multiplier( const action_state_t* ) const override;
  double composite_player_multiplier( school_e school ) const override;
  double composite_player_target_multiplier( player_t* target, school_e school ) const override;
  double composite_player_pet_damage_multiplier( const action_state_t*, bool ) const override;
  double composite_player_target_pet_damage_multiplier( player_t* target, bool guardian ) const override;
  double composite_leech() const override;
  double matching_gear_multiplier( attribute_e attr ) const override;
  double stacking_movement_modifier() const override;
  void invalidate_cache( cache_e ) override;
  void regen( timespan_t periodicity ) override;
  double resource_gain( resource_e resource_type, double amount, gain_t* g = nullptr, action_t* a = nullptr ) override;
  void create_options() override;
  std::unique_ptr<expr_t> create_expression( util::string_view expression_str ) override;
  std::unique_ptr<expr_t> create_action_expression( action_t&, util::string_view expression_str ) override;
  action_t* create_action( util::string_view name, util::string_view options ) override;
  pet_t* create_pet( util::string_view name, util::string_view type ) override;
  void create_pets() override;
  double resource_loss( resource_e resource_type, double amount, gain_t* g = nullptr, action_t* a = nullptr ) override;
  resource_e primary_resource() const override { return RESOURCE_FOCUS; }
  role_e primary_role() const override { return ROLE_ATTACK; }
  stat_e convert_hybrid_stat( stat_e s ) const override;
  std::string create_profile( save_e ) override;
  void copy_from( player_t* source ) override;
  void moving( ) override;

  std::string default_potion() const override { return hunter_apl::potion( this ); }
  std::string default_flask() const override { return hunter_apl::flask( this ); }
  std::string default_food() const override { return hunter_apl::food( this ); }
  std::string default_rune() const override { return hunter_apl::rune( this ); }
  std::string default_temporary_enchant() const override { return hunter_apl::temporary_enchant( this ); }

  void apply_affecting_auras( action_t& ) override;

  target_specific_t<hunter_td_t> target_data;

  const hunter_td_t* find_target_data( const player_t* target ) const override
  {
    return target_data[ target ];
  }

  hunter_td_t* get_target_data( player_t* target ) const override
  {
    hunter_td_t*& td = target_data[target];
    if ( !td ) td = new hunter_td_t( target, const_cast<hunter_t*>( this ) );
    return td;
  }

  std::vector<action_t*> background_actions;

  template <typename T, typename... Ts>
  T* get_background_action( util::string_view n, Ts&&... args )
  {
    auto it = range::find( background_actions, n, &action_t::name_str );
    if ( it != background_actions.cend() )
      return dynamic_cast<T*>( *it );

    auto action = new T( n, this, std::forward<Ts>( args )... );
    action -> background = true;
    background_actions.push_back( action );
    return action;
  }

  void trigger_bloodseeker_update();
  int ticking_dots( hunter_td_t* td );
  void trigger_outland_venom_update();
  void consume_trick_shots();
  void trigger_deathblow( bool activated = false );
  void trigger_sentinel( player_t* target, bool force = false, proc_t* proc = nullptr );
  void trigger_sentinel_implosion( hunter_td_t* td );
  void trigger_symphonic_arsenal();
  void trigger_lunar_storm( player_t* target );
  void consume_precise_shots();
  void trigger_spotters_mark( player_t* target, bool force = false );
  double calculate_tip_of_the_spear_value( double base_value ) const;
  bool consume_howl_of_the_pack_leader( player_t* target );
  void trigger_howl_of_the_pack_leader();
};

// Template for common hunter action code.
template <class Base>
struct hunter_action_t: public Base
{
private:
  using ab = Base;
public:

  bool track_cd_waste;
  maybe_bool decrements_tip_of_the_spear;

  struct {
    // Hunter
    // TODO 7/2/25:
    // - seems to work off a target debuff 459529 applied after hitting a target for the first time; occasionally 
    //   you can notice a lack of bonus damage from abilities that are modified by his on its first occurance on a new target
    // - the fact there are two separate effects leads to some abilities being affected up to three times
    // - this debuff seems to be modified once a target falls into execute range to implement the 50% bonus
    // - residual bleeds set to ignore damage taken debuffs on the target should mean they miss that bonus, and
    //   no double application is seen on those types of damage, but an execute bonus can actually be seen at an
    //   amount of about 14.4% rather than an expected 15%, leading to the question of whether the non execute 10% we do
    //   see comes from the passive talent aura (and some server scripting attempting to implement the execute bonus with the wrong %) 
    //   or the debuff (yet still with the wrong % somehow)
    // - for our implementation, assume the passive talent aura gives everything the baseline 10%, then apply an extra 4% execute mod for 
    //   residual bleeds separately
    bool unnatural_causes_debuff = false; // 10-15% dmg taken from caster spells from 459529 effect 1
    bool unnatural_causes_debuff_label = false; // 10-15% dmg taken from caster spells (label) from 459529 effect 2

    // Beast Mastery
    bool thrill_of_the_hunt = false;
    damage_affected_by bestial_wrath;
    damage_affected_by master_of_beasts;

    // Marksmanship
    bool trueshot_crit_damage_bonus = false;
    bool bullseye_crit_chance = false;
    damage_affected_by lone_wolf;
    damage_affected_by sniper_training;

    // Survival
    bool cull_the_herd = false;
    bool outland_venom = false;
    bool spearhead = false;
    bool deadly_duo = false;
    damage_affected_by wildfire_arsenal;
    damage_affected_by spirit_bond;
    damage_affected_by tip_of_the_spear;
    damage_affected_by coordinated_assault;

    // Pack Leader
    damage_affected_by wyverns_cry;
    damage_affected_by lead_from_the_front;    

    // Tier Set
    damage_affected_by tww_s2_mm_2pc;
    damage_affected_by tww_s2_sv_2pc;
  } affected_by;

  cdwaste::action_data_t* cd_waste = nullptr;

  hunter_action_t( util::string_view n, hunter_t* p, const spell_data_t* s ):
    ab( n, p, s ),
    track_cd_waste( s -> cooldown() > 0_ms || s -> charge_cooldown() > 0_ms )
  {
    ab::special = true;

    for ( size_t i = 1; i <= ab::data().effect_count(); i++ )
      if ( ab::data().mechanic() == MECHANIC_BLEED || ab::data().effectN( i ).mechanic() == MECHANIC_BLEED )
        affected_by.cull_the_herd = true;

    if ( p->talents.unnatural_causes.ok() )
    {
      affected_by.unnatural_causes_debuff = check_affected_by( this, p->talents.unnatural_causes_debuff->effectN( 1 ) );
      affected_by.unnatural_causes_debuff_label = check_affected_by( this, p->talents.unnatural_causes_debuff->effectN( 2 ) );
    }
    
    affected_by.sniper_training = parse_damage_affecting_aura( this, p->mastery.sniper_training );
    affected_by.trueshot_crit_damage_bonus = check_affected_by( this, p->talents.trueshot->effectN( 5 ) );
    affected_by.bullseye_crit_chance = check_affected_by( this, p->talents.bullseye->effectN( 1 ).trigger()->effectN( 1 ) );

    affected_by.thrill_of_the_hunt = check_affected_by( this, p->talents.thrill_of_the_hunt->effectN( 1 ).trigger()->effectN( 1 ) );
    affected_by.bestial_wrath = parse_damage_affecting_aura( this, p->talents.bestial_wrath );
    affected_by.master_of_beasts = parse_damage_affecting_aura( this, p->mastery.master_of_beasts );

    affected_by.spirit_bond = parse_damage_affecting_aura( this, p->mastery.spirit_bond );
    affected_by.tip_of_the_spear = parse_damage_affecting_aura( this, p->talents.tip_of_the_spear_buff );
    affected_by.outland_venom = check_affected_by( this, p->talents.outland_venom_debuff->effectN( 1 ) );
    affected_by.coordinated_assault = parse_damage_affecting_aura( this, p->talents.coordinated_assault );
    affected_by.spearhead = check_affected_by( this, p->talents.spearhead_bleed->effectN( 2 ) );
    affected_by.deadly_duo = check_affected_by( this, p->talents.spearhead_bleed->effectN( 3 ) );
    affected_by.wildfire_arsenal = parse_damage_affecting_aura( this, p->talents.wildfire_arsenal_buff );

    affected_by.wyverns_cry = parse_damage_affecting_aura( this, p->talents.howl_of_the_pack_leader_wyvern_buff );
    affected_by.lead_from_the_front = parse_damage_affecting_aura( this, p->talents.lead_from_the_front_buff );

    affected_by.tww_s2_mm_2pc = parse_damage_affecting_aura( this, p->tier_set.tww_s2_mm_2pc->effectN( 2 ).trigger() );
    affected_by.tww_s2_sv_2pc = parse_damage_affecting_aura( this, p->tier_set.tww_s2_sv_2pc->effectN( 1 ).trigger() );

    // Hunter Tree passives
    ab::apply_affecting_aura( p->talents.specialized_arsenal );
    ab::apply_affecting_aura( p->talents.improved_traps );
    ab::apply_affecting_aura( p->talents.born_to_be_wild );
    ab::apply_affecting_aura( p->talents.blackrock_munitions );
    ab::apply_affecting_aura( p->talents.lone_survivor );
    ab::apply_affecting_aura( p->talents.unnatural_causes );

    // Marksmanship Tree passives
    ab::apply_affecting_aura( p->talents.streamline );
    ab::apply_affecting_aura( p->talents.ammo_conservation );
    ab::apply_affecting_aura( p->talents.surging_shots );
    ab::apply_affecting_aura( p->talents.improved_deathblow );
    ab::apply_affecting_aura( p->talents.obsidian_arrowhead );
    ab::apply_affecting_aura( p->talents.deadeye );
    ab::apply_affecting_aura( p->talents.eagles_accuracy );
    ab::apply_affecting_aura( p->talents.small_game_hunter );

    // Beast Mastery Tree passives
    ab::apply_affecting_aura( p->talents.aspect_of_the_beast );
    ab::apply_affecting_aura( p->talents.war_orders );
    ab::apply_affecting_aura( p->talents.cobra_senses );
    ab::apply_affecting_aura( p->talents.savagery );

    // Survival Tree passives
    ab::apply_affecting_aura( p->talents.guerrilla_tactics );
    ab::apply_affecting_aura( p->talents.ranger );
    ab::apply_affecting_aura( p->talents.grenade_juggler );
    ab::apply_affecting_aura( p->talents.frenzy_strikes );
    ab::apply_affecting_aura( p->talents.vipers_venom );
    ab::apply_affecting_aura( p->talents.terms_of_engagement );
    ab::apply_affecting_aura( p->talents.tactical_advantage );
    ab::apply_affecting_aura( p->talents.explosives_expert );
    ab::apply_affecting_aura( p->talents.sweeping_spear );
    ab::apply_affecting_aura( p->talents.ruthless_marauder );
    ab::apply_affecting_aura( p->talents.symbiotic_adrenaline );
    ab::apply_affecting_aura( p->talents.deadly_duo );

    // Set Bonus passives
    ab::apply_affecting_aura( p->tier_set.tww_s1_mm_2pc );
    ab::apply_affecting_aura( p->tier_set.tww_s1_mm_4pc );
    ab::apply_affecting_aura( p->tier_set.tww_s1_sv_2pc );

    // Hero Tree passives
    ab::apply_affecting_aura( p->talents.sentinel_precision );
    ab::apply_affecting_aura( p->talents.black_arrow );
    ab::apply_affecting_aura( p->talents.banshees_mark );
  }

  hunter_t* p()             { return static_cast<hunter_t*>( ab::player ); }
  const hunter_t* p() const { return static_cast<hunter_t*>( ab::player ); }

  hunter_td_t* td( player_t* t ) { return p() -> get_target_data( t ); }
  const hunter_td_t* td( player_t* t ) const { return p() -> get_target_data( t ); }
  const hunter_td_t* find_td( const player_t* t ) const { return p() -> find_target_data( t ); }

  void init() override
  {
    ab::init();

    if ( track_cd_waste )
      cd_waste = p() -> cd_waste.get( this );

    if ( p()->talents.tip_of_the_spear.ok() )
    {
      if ( decrements_tip_of_the_spear.is_none() )
        decrements_tip_of_the_spear = affected_by.tip_of_the_spear.direct > 0;
    }
    else
    {
      decrements_tip_of_the_spear = false;
    }

    if ( decrements_tip_of_the_spear )
      ab::sim->print_debug( "{} action {} set to decrement Tip of the Spear", ab::player->name(), ab::name() );
  }

  timespan_t gcd() const override
  {
    timespan_t g = ab::gcd();

    if ( g == 0_ms )
      return g;

    if ( g < ab::min_gcd )
      g = ab::min_gcd;

    return g;
  }

  void execute() override
  {
    ab::execute();

    if ( decrements_tip_of_the_spear )
      p()->buffs.tip_of_the_spear->decrement();
  }

  void impact( action_state_t* s ) override
  {
    ab::impact( s );
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double am = ab::composite_da_multiplier( s );

    if ( affected_by.bestial_wrath.direct )
      am *= 1 + p()->buffs.bestial_wrath->check_value();

    if ( affected_by.master_of_beasts.direct )
      am *= 1 + p()->cache.mastery() * p()->mastery.master_of_beasts->effectN( affected_by.master_of_beasts.direct ).mastery_value();

    if ( affected_by.sniper_training.direct )
      am *= 1 + p()->cache.mastery() * p()->mastery.sniper_training->effectN( affected_by.sniper_training.direct ).mastery_value();

    if ( affected_by.spirit_bond.direct )
    {
      double bonus = p()->cache.mastery() * p()->mastery.spirit_bond->effectN( affected_by.spirit_bond.direct ).mastery_value();
      bonus *= 1 + p()->mastery.spirit_bond_buff->effectN( 1 ).percent();
      am *= 1 + bonus;
    }

    if ( affected_by.coordinated_assault.direct && p()->buffs.coordinated_assault->check() )
      am *= 1 + p()->talents.coordinated_assault->effectN( affected_by.coordinated_assault.direct ).percent();

    if ( affected_by.tip_of_the_spear.direct && p()->buffs.tip_of_the_spear->check() )
      am *= 1 + p()->calculate_tip_of_the_spear_value( p()->talents.tip_of_the_spear->effectN( affected_by.tip_of_the_spear.direct ).percent() );

    if ( affected_by.tww_s2_mm_2pc.direct )
      am *= 1 + p()->buffs.jackpot->value();

    if ( affected_by.wyverns_cry.direct )
      am *= 1 + p()->buffs.wyverns_cry->check_stack_value();

    if ( affected_by.lead_from_the_front.direct && p()->buffs.lead_from_the_front->check() )
      am *= 1 + p()->talents.lead_from_the_front_buff->effectN( affected_by.lead_from_the_front.direct ).percent();

    if ( affected_by.wildfire_arsenal.direct )
      am *= 1 + p()->buffs.wildfire_arsenal->stack_value();

    if ( affected_by.tww_s2_sv_2pc.direct )
      am *= 1 + p()->buffs.winning_streak->stack_value();

    return am;
  }

  double composite_ta_multiplier( const action_state_t* s ) const override
  {
    double am = ab::composite_ta_multiplier( s );

    if ( affected_by.bestial_wrath.tick )
      am *= 1 + p()->buffs.bestial_wrath->check_value();

    if ( affected_by.sniper_training.tick )
      am *= 1 + p()->cache.mastery() * p()->mastery.sniper_training->effectN( affected_by.sniper_training.tick ).mastery_value();

    if ( affected_by.spirit_bond.tick )
    {
      double bonus = p()->cache.mastery() * p()->mastery.spirit_bond->effectN( affected_by.spirit_bond.tick ).mastery_value();
      bonus *= 1 + p()->mastery.spirit_bond_buff->effectN( 3 ).percent();
      am *= 1 + bonus;
    }

    if ( affected_by.coordinated_assault.tick && p()->buffs.coordinated_assault->check() )
      am *= 1 + p()->talents.coordinated_assault->effectN( affected_by.coordinated_assault.tick ).percent();

    if ( affected_by.wyverns_cry.tick )
      am *= 1 + p()->buffs.wyverns_cry->check_stack_value();

    if ( affected_by.lead_from_the_front.tick && p()->buffs.lead_from_the_front->check() )
      am *= 1 + p()->talents.lead_from_the_front_buff->effectN( affected_by.lead_from_the_front.tick ).percent();

    if ( affected_by.tww_s2_sv_2pc.tick )
      am *= 1 + p()->buffs.winning_streak->stack_value();

    return am;
  }

  double composite_crit_chance() const override
  {
    double cc = ab::composite_crit_chance();

    if ( affected_by.thrill_of_the_hunt )
      cc += p()->buffs.thrill_of_the_hunt->check_stack_value();

    if ( affected_by.bullseye_crit_chance )
      cc += p()->buffs.bullseye->check_stack_value();

    return cc;
  }

  double composite_crit_damage_bonus_multiplier() const override
  {
    double cm = ab::composite_crit_damage_bonus_multiplier();

    if ( affected_by.trueshot_crit_damage_bonus && p()->buffs.trueshot->check() )
      cm *= 1 + p()->talents.trueshot->effectN( 5 ).percent() + p()->talents.unerring_vision->effectN( 2 ).percent();

    return cm;
  }

  double composite_target_crit_chance( player_t* target ) const override
  {
    double c = ab::composite_target_crit_chance( target );

    if ( affected_by.spearhead && td( target )->dots.spearhead->is_ticking() )
      c += p()->talents.spearhead_bleed->effectN( 2 ).percent();

    return c;
  }

  double composite_target_crit_damage_bonus_multiplier( player_t* target ) const override
  {
    double cm = ab::composite_target_crit_damage_bonus_multiplier( target );

    if ( affected_by.deadly_duo && p()->talents.deadly_duo.ok() && td( target )->dots.spearhead->is_ticking() )
      cm *= 1.0 + p()->talents.deadly_duo->effectN( 2 ).percent();

    if ( affected_by.outland_venom )
      cm *= 1 + td( target )->debuffs.outland_venom->check_stack_value();

    return cm;
  }

  double composite_target_da_multiplier( player_t* target ) const override
  {
    double da = ab::composite_target_da_multiplier( target );

    // just assumed the debuff will always be applied
    if ( affected_by.unnatural_causes_debuff || affected_by.unnatural_causes_debuff_label )
    {
      double execute_mod = 0.0;
      if ( target->health_percentage() < p()->talents.unnatural_causes->effectN( 3 ).base_value() )
        execute_mod = p()->talents.unnatural_causes->effectN( 2 ).percent();

      if ( affected_by.unnatural_causes_debuff )
        da *= 1 + p()->talents.unnatural_causes_debuff->effectN( 1 ).percent() * ( 1 + execute_mod );

      if ( affected_by.unnatural_causes_debuff_label )
        da *= 1 + p()->talents.unnatural_causes_debuff->effectN( 2 ).percent() * ( 1 + execute_mod );
    }

    return da;
  }

  double composite_target_ta_multiplier( player_t* target ) const override
  {
    double ta = ab::composite_target_ta_multiplier( target );

    if ( affected_by.cull_the_herd )
      ta *= 1 + p()->get_target_data( target )->debuffs.cull_the_herd->check_value();

    // just assumed the debuff will always be applied
    if ( affected_by.unnatural_causes_debuff || affected_by.unnatural_causes_debuff_label )
    {
      double execute_mod = 0.0;
      if ( target->health_percentage() < p()->talents.unnatural_causes->effectN( 3 ).base_value() )
        execute_mod = p()->talents.unnatural_causes->effectN( 2 ).percent();

      if ( affected_by.unnatural_causes_debuff )
        ta *= 1 + p()->talents.unnatural_causes_debuff->effectN( 1 ).percent() * ( 1 + execute_mod );

      if ( affected_by.unnatural_causes_debuff_label )
        ta *= 1 + p()->talents.unnatural_causes_debuff->effectN( 2 ).percent() * ( 1 + execute_mod );
    }

    return ta;
  }

  void update_ready( timespan_t cd ) override
  {
    if ( cd_waste )
      cd_waste -> update_ready( this, cd );

    ab::update_ready( cd );
  }

  virtual double energize_cast_regen( const action_state_t* s ) const
  {
    const int num_targets = this -> n_targets();
    size_t targets_hit = 1;
    if ( ab::energize_type == action_energize::PER_HIT && ( num_targets == -1 || num_targets > 0 ) )
    {
      size_t tl_size = this -> target_list().size();
      targets_hit = ( num_targets < 0 ) ? tl_size : std::min( tl_size, as<size_t>( num_targets ) );
    }
    return targets_hit * this -> composite_energize_amount( s );
  }

  virtual double cast_regen( const action_state_t* s ) const
  {
    const timespan_t execute_time = this -> execute_time();
    const timespan_t cast_time = std::max( execute_time, this -> gcd() );
    const double regen = p() -> resource_regen_per_second( RESOURCE_FOCUS );

    double total_regen = regen * cast_time.total_seconds();
    double total_energize = energize_cast_regen( s );

    return total_regen + floor( total_energize );
  }

  // action list expressions
  std::unique_ptr<expr_t> create_expression( util::string_view name ) override
  {
    if ( util::str_compare_ci( name, "cast_regen" ) )
    {
      // Return the focus that will be regenerated during the cast time or GCD of the target action.
      return make_fn_expr( "cast_regen",
        [ this, state = std::unique_ptr<action_state_t>( this -> get_state() ) ] {
          this -> snapshot_state( state.get(), result_amount_type::NONE );
          state -> target = this -> target;
          return this -> cast_regen( state.get() );
        } );
    }

    // fudge wildfire bomb dot name
    auto splits = util::string_split<util::string_view>( name, "." );
    if ( splits.size() == 3 && splits[ 0 ] == "dot" && splits[ 1 ] == "wildfire_bomb" )
      return ab::create_expression( fmt::format( "dot.wildfire_bomb_dot.{}", splits[ 2 ] ) );

    return ab::create_expression( name );
  }

  void add_pet_stats( pet_t* pet, std::initializer_list<util::string_view> names )
  {
    if ( ! pet )
      return;

    for ( const auto& n : names )
    {
      stats_t* s = pet -> find_stats( n );
      if ( s )
        ab::stats -> add_child( s );
    }
  }

  bool trigger_buff( buff_t *const buff, timespan_t precast_time, timespan_t duration = timespan_t::min() ) const
  {
    const bool in_combat = ab::player -> in_combat;
    const bool triggered = buff -> trigger(duration);
    if ( triggered && ab::is_precombat && !in_combat && precast_time > 0_ms )
    {
      buff -> extend_duration( ab::player, -std::min( precast_time, buff -> buff_duration() ) );
      buff -> cooldown -> adjust( -precast_time );
    }
    return triggered;
  }

  void adjust_precast_cooldown( timespan_t precast_time ) const
  {
    const bool in_combat = ab::player -> in_combat;
    if ( ab::is_precombat && !in_combat && precast_time > 0_ms )
      ab::cooldown -> adjust( -precast_time );
  }
};

struct hunter_ranged_attack_t : public hunter_action_t<ranged_attack_t>
{
  hunter_ranged_attack_t( util::string_view n, hunter_t* p, const spell_data_t* s = spell_data_t::nil() ) : hunter_action_t( n, p, s )
  {
  }

  bool usable_moving() const override
  {
    return true;
  }
};

struct hunter_melee_attack_t : public hunter_action_t<melee_attack_t>
{
  hunter_melee_attack_t( util::string_view n, hunter_t* p, const spell_data_t* s = spell_data_t::nil() ) : hunter_action_t( n, p, s )
  {
  }

  void init() override
  {
    hunter_action_t::init();

    if ( weapon && weapon -> group() != WEAPON_2H )
      background = true;
  }
};

using hunter_spell_t = hunter_ranged_attack_t;

namespace pets
{
// ==========================================================================
// Hunter Pet
// ==========================================================================

struct hunter_pet_t: public pet_t
{
  struct buffs_t
  {
    buff_t* beast_cleave = nullptr;
  } buffs;

  struct actions_t
  {
    action_t* beast_cleave = nullptr;
  } actions;

  hunter_pet_t( hunter_t* owner, util::string_view pet_name, pet_e pt = PET_HUNTER, bool guardian = false, bool dynamic = false ) :
    pet_t( owner -> sim, owner, pet_name, pt, guardian, dynamic )
  {
    owner_coeff.ap_from_ap = 0.15;

    main_hand_weapon.type       = WEAPON_BEAST;
    main_hand_weapon.swing_time = 2_s;
  }

  void apply_affecting_auras( action_t& action )
  {
    pet_t::apply_affecting_auras( action );

    action.apply_affecting_aura( o()->specs.hunter );
    action.apply_affecting_aura( o()->specs.beast_mastery_hunter );
    action.apply_affecting_aura( o()->specs.marksmanship_hunter );
    action.apply_affecting_aura( o()->specs.survival_hunter );
  }

  void schedule_ready( timespan_t delta_time, bool waiting ) override
  {
    if ( main_hand_attack && !main_hand_attack->execute_event )
      main_hand_attack->schedule_execute();

    pet_t::schedule_ready( delta_time, waiting );
  }

  double composite_melee_attack_power() const
  {
    double ap = pet_t::composite_melee_attack_power();

    // TODO does nothing in game
    // ap *= 1 + o()->talents.better_together->effectN( 2 ).percent();

    return ap;
  }

  void create_buffs() override
  {
    pet_t::create_buffs();

    buffs.beast_cleave =
      make_buff( this, "beast_cleave", find_spell( 118455 ) )
      -> set_default_value( o()->talents.beast_cleave.ok() ? o() -> talents.beast_cleave -> effectN( 1 ).percent() : 1.0 )
      -> apply_affecting_effect( o() -> talents.beast_cleave -> effectN( 2 ) );
  }

  hunter_t* o()             { return static_cast<hunter_t*>( owner ); }
  const hunter_t* o() const { return static_cast<hunter_t*>( owner ); }

  void init_spells() override;
};

static std::pair<timespan_t, int> dire_beast_duration( hunter_t* p )
{
  // Dire beast gets a chance for an extra attack based on haste
  // rather than discrete plateaus.  At integer numbers of attacks,
  // the beast actually has a 50% chance of n-1 attacks and 50%
  // chance of n.  It (apparently) scales linearly between n-0.5
  // attacks to n+0.5 attacks.  This uses beast duration to
  // effectively alter the number of attacks as the duration itself
  // isn't important and combat log testing shows some variation in
  // attack speeds.  This is not quite perfect but more accurate
  // than plateaus.
  const timespan_t base_duration    = p->buffs.dire_beast->buff_duration();
  const timespan_t swing_time       = 2_s * p->cache.auto_attack_speed();
  double partial_attacks_per_summon = base_duration / swing_time;
  int base_attacks_per_summon       = static_cast<int>( partial_attacks_per_summon );
  partial_attacks_per_summon -= static_cast<double>( base_attacks_per_summon );

  if ( p->rng().roll( partial_attacks_per_summon ) )
    base_attacks_per_summon += 1;

  return { base_attacks_per_summon * swing_time, base_attacks_per_summon };
}

// ==========================================================================
// Shadow Hounds
// ==========================================================================

struct dark_hound_t final : public hunter_pet_t
{
  dark_hound_t( hunter_t* owner, util::string_view n = "dark_hound" )
    : hunter_pet_t( owner, n, PET_HUNTER, true /* GUARDIAN */, true /* dynamic */ )
  {
    resource_regeneration  = regen_type::DISABLED;
    owner_coeff.ap_from_ap = 3;
  }

  void summon( timespan_t duration = 0_ms ) override
  {
    hunter_pet_t::summon( duration );

    if ( main_hand_attack )
      main_hand_attack->execute();
  }
  
  void init_spells() override;
};

// ==========================================================================
// Dire Critter
// ==========================================================================

struct dire_critter_t : public hunter_pet_t
{
  dire_critter_t( hunter_t* owner, util::string_view n = "dire_beast" )
    : hunter_pet_t( owner, n, PET_HUNTER, true /* GUARDIAN */, true /* dynamic */ )
  {
    resource_regeneration = regen_type::DISABLED;
  }

  void summon( timespan_t duration = 0_ms ) override
  {
    hunter_pet_t::summon( duration );

    if( o()->talents.dire_cleave.ok() )
      buffs.beast_cleave->trigger( o()->talents.dire_cleave->effectN( 2 ).time_value() );
    
    if ( main_hand_attack )
      main_hand_attack->execute();
  }

  double composite_player_multiplier( school_e school ) const override
  {
    double m = hunter_pet_t::composite_player_multiplier( school );

    m *= 1 + o()->talents.dire_frenzy->effectN( 2 ).percent();

    return m;
  }

  void init_spells() override;
};

// ==========================================================================
// Dire Beast
// ==========================================================================

struct dire_beast_t final : public dire_critter_t
{
  const spell_data_t* energize = find_spell( 281036 );

  dire_beast_t( hunter_t* owner, util::string_view n = "dire_beast" ) : dire_critter_t( owner, n )
  {
    // 11-10-22 Dire Beast - Damage increased by 400%. (15% -> 60%)
    // 13-10-22 Dire Beast damage increased by 50%. (60% -> 90%)
    // 22-7-24 Dire Beast damage increased by 10% (90% -> 100%)
    owner_coeff.ap_from_ap = 1;
  }

  void summon( timespan_t duration = 0_ms ) override
  {
    dire_critter_t::summon( duration );

    o()->buffs.dire_beast->trigger( duration );
    o()->resource_gain( RESOURCE_FOCUS, energize->effectN( 2 ).base_value(), o()->gains.dire_beast );
  }
};

// =========================================================================
// Fenryr
// =========================================================================

struct fenryr_td_t final : public actor_target_data_t
{
public:
  struct dots_t
  {
    dot_t* ravenous_leap = nullptr;
  } dots;

  fenryr_td_t( player_t* target, fenryr_t* p );
};

struct fenryr_t final : public dire_critter_t
{
  struct actions_t
  {
    action_t* ravenous_leap = nullptr;
  } actions;

  target_specific_t<fenryr_td_t> target_data;

  fenryr_t( hunter_t* owner, util::string_view n = "fenryr" ) : dire_critter_t( owner, n )
  {
    owner_coeff.ap_from_ap = 0.37;
    auto_attack_multiplier = 7;
    main_hand_weapon.swing_time = 1.5_s;
  }

  void summon( timespan_t duration = 0_ms ) override
  {
    dire_critter_t::summon( duration );

    actions.ravenous_leap->execute_on_target( target );
  }

  const fenryr_td_t* find_target_data( const player_t* target ) const override
  {
    return target_data[ target ];
  }

  fenryr_td_t* get_target_data( player_t* target ) const override
  {
    fenryr_td_t*& td = target_data[target];
    if ( !td )
      td = new fenryr_td_t( target, const_cast<fenryr_t*>( this ) );
    return td;
  }

  void init_spells() override;
};

// ==========================================================================
// Hati
// ==========================================================================

struct hati_t final : public dire_critter_t
{
  hati_t( hunter_t* owner, util::string_view n = "hati" ) : dire_critter_t( owner, n )
  {
    owner_coeff.ap_from_ap = 0.37;
    auto_attack_multiplier = 7;
    main_hand_weapon.swing_time = 1.5_s;
  }
};

// ==========================================================================
// Bear
// ==========================================================================

struct bear_td_t final : public actor_target_data_t
{
public:
  struct dots_t
  {
    dot_t* rend_flesh = nullptr;
  } dots;

  bear_td_t( player_t* target, bear_t* p );
};

struct bear_t final : public dire_critter_t
{
  struct buffs_t
  {
    buff_t* bear_summon;
  } buffs;

  struct actions_t
  {
    action_t* rend_flesh = nullptr;
  } actions;
  
  target_specific_t<bear_td_t> target_data;

  bear_t( hunter_t* owner, util::string_view n = "bear" ) : dire_critter_t( owner, n )
  {
    owner_coeff.ap_from_ap = 0.7;
    auto_attack_multiplier = 8;
    main_hand_weapon.swing_time = 1.5_s;
  }

  void summon( timespan_t duration = 0_ms ) override
  {
    dire_critter_t::summon( duration );
    
    buffs.bear_summon->trigger( -1, buffs.bear_summon->default_value + ( o()->buffs.lead_from_the_front->check() ? o()->talents.lead_from_the_front_buff->effectN( 3 ).percent() : 0 ) );

    if ( actions.rend_flesh )
      actions.rend_flesh->execute_on_target( target );
  }

  double composite_player_multiplier( school_e school ) const override
  {
    double m = dire_critter_t::composite_player_multiplier( school );

    if ( buffs.bear_summon->has_common_school( school ) )
      m *= 1 + buffs.bear_summon->check_value();
    
    return m;
  }

  void create_buffs() override
  {
    dire_critter_t::create_buffs();

    buffs.bear_summon = make_buff( this, "bear_summon", o()->talents.howl_of_the_pack_leader_bear_buff )
      ->set_default_value_from_effect( 1 )
      ->apply_affecting_aura( o()->specs.beast_mastery_hunter );
  }

  const bear_td_t* find_target_data( const player_t* target ) const override
  {
    return target_data[ target ];
  }

  bear_td_t* get_target_data( player_t* target ) const override
  {
    bear_td_t*& td = target_data[target];
    if ( !td )
      td = new bear_td_t( target, const_cast<bear_t*>( this ) );
    return td;
  }

  void init_spells() override;
};

// Base class for pets from player stable
struct stable_pet_t : public hunter_pet_t
{
  struct actions_t
  {
    action_t* stomp = nullptr;
    action_t* thundering_hooves = nullptr;
  } actions;

  stable_pet_t( hunter_t* owner, util::string_view pet_name, pet_e pet_type ):
    hunter_pet_t( owner, pet_name, pet_type, false /* GUARDIAN */, true /* dynamic */ )
  {
    stamina_per_owner = 0.7;
    owner_coeff.ap_from_ap = 0.6;

    initial.armor_multiplier *= 1.05;

    main_hand_weapon.swing_time = owner -> options.pet_attack_speed;
  }
  
  void init_spells() override;
};

// ==========================================================================
// Call of the Wild
// ==========================================================================

struct call_of_the_wild_pet_t final : public stable_pet_t
{
  call_of_the_wild_pet_t( hunter_t* owner ) : stable_pet_t( owner, "call_of_the_wild_pet", PET_HUNTER )
  {
    resource_regeneration = regen_type::DISABLED;
  }

  void summon( timespan_t duration = 0_ms ) override
  {
    stable_pet_t::summon( duration );

    if ( main_hand_attack )
      main_hand_attack->execute();
  }
};

// Base class for main pet & Animal Companion
struct hunter_main_pet_base_td_t : public actor_target_data_t
{
public:
  struct debuffs_t
  {
    buff_t* venomous_bite = nullptr;
  } debuffs;
  
  hunter_main_pet_base_td_t( player_t* target, hunter_main_pet_base_t* p );
};

struct hunter_main_pet_base_t : public stable_pet_t
{
  struct actions_t
  {
    action_t* kill_command = nullptr;
    action_t* kill_cleave = nullptr;
    action_t* bestial_wrath = nullptr;
  } actions;

  struct buffs_t
  {
    buff_t* frenzy = nullptr;
    buff_t* thrill_of_the_hunt = nullptr;
    buff_t* bestial_wrath = nullptr;
    buff_t* piercing_fangs = nullptr;
  } buffs;

  target_specific_t<hunter_main_pet_base_td_t> target_data;

  hunter_main_pet_base_t( hunter_t* owner, util::string_view pet_name, pet_e pet_type ) : stable_pet_t( owner, pet_name, pet_type ) {}

  void create_buffs() override
  {
    stable_pet_t::create_buffs();

    buffs.frenzy =
      make_buff( this, "frenzy", o() -> find_spell( 272790 ) )
      -> set_default_value_from_effect( 1 )
      -> modify_default_value( o() -> tier_set.tww_s1_bm_2pc -> effectN( 1 ).percent() )
      -> apply_affecting_aura( o() -> talents.savagery )
      -> add_invalidate( CACHE_AUTO_ATTACK_SPEED );

    buffs.thrill_of_the_hunt =
      make_buff( this, "thrill_of_the_hunt", find_spell( 312365 ) )
        -> set_default_value_from_effect( 1 )
        -> apply_affecting_aura( o() -> talents.savagery )
        -> set_max_stack( std::max( 1, as<int>( o() -> talents.thrill_of_the_hunt -> effectN( 2 ).base_value() ) ) )
        -> set_chance( o() -> talents.thrill_of_the_hunt.ok() );

    buffs.bestial_wrath =
      make_buff( this, "bestial_wrath", find_spell( 186254 ) )
        -> set_default_value_from_effect( 1 )
        -> set_cooldown( 0_ms )
        -> set_stack_change_callback( [ this ]( buff_t*, int old, int cur ) {
          if ( cur == 0 )
          {
            buffs.piercing_fangs -> expire();
          }
          else if (old == 0) {
            buffs.piercing_fangs -> trigger();
          }
        } );

    buffs.piercing_fangs =
      make_buff( this, "piercing_fangs", o() -> find_spell( 392054 ) )
        -> set_default_value_from_effect( 1 )
        -> set_chance( o() -> talents.piercing_fangs.ok() );
  }

  double composite_melee_auto_attack_speed() const override
  {
    double ah = stable_pet_t::composite_melee_auto_attack_speed();

    if ( buffs.frenzy -> check() )
      ah /= 1 + buffs.frenzy -> check_stack_value();

    return ah;
  }

  double composite_player_multiplier( school_e school ) const override
  {
    double m = stable_pet_t::composite_player_multiplier( school );

    if ( buffs.bestial_wrath -> has_common_school( school ) )
      m *= 1 + buffs.bestial_wrath -> check_value();
    
    return m;
  }

  double composite_melee_crit_chance() const override
  {
    double cc = stable_pet_t::composite_melee_crit_chance();

    cc += buffs.thrill_of_the_hunt -> check_stack_value();

    return cc;
  }

  double composite_player_critical_damage_multiplier( const action_state_t* s ) const override
  {
    double m = stable_pet_t::composite_player_critical_damage_multiplier( s );

    if ( buffs.piercing_fangs -> data().effectN( 1 ).has_common_school( s -> action -> school ) )
      m *= 1 + buffs.piercing_fangs -> check_value();

    return m;
  }

  const hunter_main_pet_base_td_t* find_target_data( const player_t* target ) const override
  {
    return target_data[ target ];
  }

  hunter_main_pet_base_td_t* get_target_data( player_t* target ) const override
  {
    hunter_main_pet_base_td_t*& td = target_data[target];
    if ( !td )
      td = new hunter_main_pet_base_td_t( target, const_cast<hunter_main_pet_base_t*>( this ) );
    return td;
  }

  void moving() override { return; }

  void init_spells() override;
  void init_special_effects() override;
};

// ==========================================================================
// Animal Companion
// ==========================================================================

struct animal_companion_td_t : public hunter_main_pet_base_td_t
{
public:
  struct debuffs_t
  {
    buff_t* bloodshed = nullptr;
  } debuffs;

  animal_companion_td_t( player_t* target, animal_companion_t* p );
};

struct animal_companion_t final : public hunter_main_pet_base_t
{
  target_specific_t<animal_companion_td_t> target_data;

  animal_companion_t( hunter_t* owner ) : hunter_main_pet_base_t( owner, "animal_companion", PET_HUNTER )
  {
    resource_regeneration = regen_type::DISABLED;
  }

  double composite_player_target_multiplier( player_t* target, school_e school ) const
  {
    double m = hunter_main_pet_base_t::composite_player_target_multiplier( target, school );

    auto td = get_target_data( target );
    if ( td->debuffs.bloodshed->check() && td->debuffs.bloodshed->has_common_school( school ) )
    {
      double bonus = td->debuffs.bloodshed->check_value();
      if ( td->debuffs.bloodshed->data().affected_by( o()->talents.venomous_bite->effectN( 1 ) ) )
        bonus *= 1 + o()->talents.venomous_bite->effectN( 1 ).percent();

      m *= 1 + bonus;
    }

    return m;
  }

  const animal_companion_td_t* find_target_data( const player_t* target ) const override
  {
    return target_data[ target ];
  }

  animal_companion_td_t* get_target_data( player_t* target ) const override
  {
    animal_companion_td_t*& td = target_data[target];
    if ( !td )
      td = new animal_companion_td_t( target, const_cast<animal_companion_t*>( this ) );
    return td;
  }

  void init_spells() override;
};

// ==========================================================================
// Hunter Main Pet
// ==========================================================================

struct hunter_main_pet_td_t: public hunter_main_pet_base_td_t
{
public:
  struct dots_t
  {
    dot_t* bloodseeker = nullptr;
    dot_t* bloodshed = nullptr;
  } dots;

  struct debuffs_t
  {
    buff_t* spearhead = nullptr;
  } debuffs;

  hunter_main_pet_td_t( player_t* target, hunter_main_pet_t* p );
};

struct hunter_main_pet_t final : public hunter_main_pet_base_t
{
  struct actions_t
  {
    action_t* basic_attack = nullptr;
    action_t* brutal_companion_ba = nullptr;
    action_t* bloodshed = nullptr;
    action_t* potent_mutagen = nullptr; // TWW S2 BM 4PC

    action_t* flanking_strike = nullptr;
    action_t* coordinated_assault = nullptr;

    action_t* no_mercy_ba = nullptr;
  } actions;

  struct buffs_t
  {
    buff_t* solitary_companion = nullptr;
    buff_t* potent_mutagen = nullptr; // TWW S2 BM 4pc

    buff_t* bloodseeker = nullptr;
    buff_t* spearhead = nullptr;
  } buffs;
  
  target_specific_t<hunter_main_pet_td_t> target_data;

  hunter_main_pet_t( hunter_t* owner, util::string_view pet_name, pet_e pt ) : hunter_main_pet_base_t( owner, pet_name, pt )
  {
    // FIXME work around assert in pet specs
    // Set default specs
    _spec = default_spec();
  }

  specialization_e default_spec()
  {
    if ( pet_type > PET_NONE          && pet_type < PET_FEROCITY_TYPE ) return PET_FEROCITY;
    if ( pet_type > PET_FEROCITY_TYPE && pet_type < PET_TENACITY_TYPE ) return PET_TENACITY;
    if ( pet_type > PET_TENACITY_TYPE && pet_type < PET_CUNNING_TYPE ) return PET_CUNNING;
    return PET_FEROCITY;
  }

  buff_t* spec_passive() const
  {
    switch ( specialization() )
    {
      case PET_CUNNING:  return o() -> buffs.pathfinding;
      case PET_FEROCITY: return o() -> buffs.predators_thirst;
      case PET_TENACITY: return o() -> buffs.endurance_training;
      default: assert( false && "Invalid pet spec" );
    }
    return nullptr;
  }

  void init_base_stats() override
  {
    hunter_main_pet_base_t::init_base_stats();

    resources.base[RESOURCE_HEALTH] = 6373;
    resources.base[RESOURCE_FOCUS] = 100;

    base_gcd = 1.5_s;

    resources.infinite_resource[RESOURCE_FOCUS] = o() -> resources.infinite_resource[RESOURCE_FOCUS];
  }

  void create_buffs() override
  {
    hunter_main_pet_base_t::create_buffs();

    buffs.bloodseeker =
      make_buff( this, "bloodseeker", o() -> find_spell( 260249 ) )
        -> set_default_value_from_effect( 1 )
        -> add_invalidate( CACHE_AUTO_ATTACK_SPEED );

    buffs.potent_mutagen = 
      make_buff( this, "potent_mutagen", o()->find_spell( 1218003 ) )
        //2025-02-11 - For some reason the base value is still 1 (so the pet buff says 1 second reduction per hit) but the server script doing the reduction only reduces by 0.5s
        ->set_default_value( o()->find_spell( 1218004 )->effectN( 2 ).base_value() / 2 );

    buffs.solitary_companion = 
      make_buff( this, "solitary_companion", find_spell( 474751 ) )
      ->set_default_value_from_effect( 2 );

    buffs.spearhead = 
      make_buff( this, "spearhead_buff", o()->talents.spearhead_debuff )
        ->set_default_value( o()->talents.deadly_duo->effectN( 2 ).percent() )
        ->set_schools_from_effect( 3 );
  }

  void init_action_list() override
  {
    if ( action_list_str.empty() )
    {
      action_list_str += "/snapshot_stats";
      action_list_str += "/claw";
      use_default_action_list = true;
    }

    hunter_main_pet_base_t::init_action_list();
  }

  double resource_regen_per_second( resource_e r ) const override
  {
    if ( r == RESOURCE_FOCUS )
      return owner -> resource_regen_per_second( RESOURCE_FOCUS ) * 1.25;

    return hunter_main_pet_base_t::resource_regen_per_second( r );
  }

  void summon( timespan_t duration = 0_ms ) override
  {
    hunter_main_pet_base_t::summon( duration );

    if ( o()->talents.trigger_finger.ok() )
      o()->invalidate_cache( CACHE_AUTO_ATTACK_SPEED );

    o() -> pets.main = this;
    
    if ( o()->talents.solitary_companion.ok() )
    {
       o()->pets.main->buffs.solitary_companion->trigger();
    }

    if ( o() -> pets.animal_companion )
    {
      o() -> pets.animal_companion -> summon();
      o() -> pets.animal_companion -> schedule_ready(0_s, false);
    }

    spec_passive() -> trigger();
  }

  void demise() override
  {
    hunter_main_pet_base_t::demise();

    if ( o() -> pets.main == this )
    {
      o() -> pets.main = nullptr;

      spec_passive() -> expire();

      if ( !sim->event_mgr.canceled && o()->talents.trigger_finger.ok() )
        o()->invalidate_cache( CACHE_AUTO_ATTACK_SPEED );
    }
    if ( o() -> pets.animal_companion )
      o() -> pets.animal_companion -> demise();
  }

  double composite_player_multiplier( school_e school ) const override
  {
    double m = hunter_main_pet_base_t::composite_player_multiplier( school );

    if ( o()->talents.solitary_companion.ok() && buffs.solitary_companion->up() )
      m *= 1 + buffs.solitary_companion->check_value();
    
    return m;
  }

  double composite_player_target_multiplier( player_t* target, school_e school ) const
  {
    double m = hunter_main_pet_base_t::composite_player_target_multiplier( target, school );

    if ( get_target_data( target )->dots.bloodshed->is_ticking() && o()->talents.bloodshed_dot->effectN( 2 ).has_common_school( school ) )
    {
      double bonus = o()->talents.bloodshed_dot->effectN( 2 ).percent();
      if ( o()->talents.bloodshed_dot->affected_by( o()->talents.venomous_bite->effectN( 1 ) ) )
        bonus *= 1 + o()->talents.venomous_bite->effectN( 1 ).percent();

      m *= 1 + bonus;
    }

    return m;
  }

  double composite_player_target_crit_chance( player_t* target ) const override
  {
    double m = hunter_main_pet_base_t::composite_player_target_crit_chance( target );
    
    m += get_target_data( target )->debuffs.spearhead->check_value();

    return m;
  }

  double composite_player_critical_damage_multiplier( const action_state_t* s ) const override
  {
    double m = hunter_main_pet_base_t::composite_player_critical_damage_multiplier( s );

    if ( buffs.spearhead->has_common_school( s->action->school ) )
      m *= 1 + buffs.spearhead->check_value();

    return m;
  }

  double composite_melee_auto_attack_speed() const override
  {
    double ah = hunter_main_pet_base_t::composite_melee_auto_attack_speed();

    if ( buffs.bloodseeker && buffs.bloodseeker->check() )
      ah /= 1 + buffs.bloodseeker->check_stack_value();

    return ah;
  }

  const hunter_main_pet_td_t* find_target_data( const player_t* target ) const override
  {
    return target_data[ target ];
  }

  hunter_main_pet_td_t* get_target_data( player_t* target ) const override
  {
    hunter_main_pet_td_t*& td = target_data[target];
    if ( !td )
      td = new hunter_main_pet_td_t( target, const_cast<hunter_main_pet_t*>( this ) );
    return td;
  }

  resource_e primary_resource() const override
  { return RESOURCE_FOCUS; }

  timespan_t available() const override
  {
    // XXX: this will have to be changed if we ever add other foreground attacks to pets besides Basic Attacks
    if ( !actions.basic_attack )
      return hunter_main_pet_base_t::available();

    const auto time_to_fc = timespan_t::from_seconds( ( actions.basic_attack->base_cost() - resources.current[ RESOURCE_FOCUS ] ) /
                                                        resource_regen_per_second( RESOURCE_FOCUS ) );
    const auto time_to_cd = actions.basic_attack->cooldown->remains();
    const auto remains = std::max( time_to_cd, time_to_fc );
    // 2018-07-23 - hunter pets seem to have a "generic" lag of about .6s on basic attack usage
    const auto delay_mean = o() -> options.pet_basic_attack_delay;
    const auto delay_stddev = 100_ms;
    const auto lag = o()->bugs ? rng().gauss( delay_mean, delay_stddev ) : 0_ms;
    return std::max( remains + lag, 100_ms );
  }

  action_t* create_action( util::string_view name, util::string_view options_str ) override;
  void init_spells() override;
};

namespace actions
{

static void trigger_beast_cleave( const action_state_t* s )
{
  if ( !s->action->result_is_hit( s->result ) )
    return;

  if ( s->action->sim->active_enemies == 1 )
    return;

  auto p = debug_cast<hunter_pet_t*>( s->action->player );

  if ( !p->buffs.beast_cleave->up() )
    return;

  // Target multipliers do not replicate to secondary targets
  const double target_da_multiplier = ( 1.0 / s->target_da_multiplier );
  const double target_pet_multiplier = ( 1.0 / s->target_pet_multiplier );

  const double amount = s->result_total * p->buffs.beast_cleave->check_value() * target_da_multiplier * target_pet_multiplier;
  p->actions.beast_cleave->execute_on_target( s->target, amount );
}

// Template for common hunter pet action code.
template <class T_PET, class Base>
struct hunter_pet_action_t : public Base
{
private:
  using ab = Base;
public:

  struct {
    // Hunter
    bool unnatural_causes_debuff = false;
    bool unnatural_causes_debuff_label = false;

    // Beast Mastery
    damage_affected_by bestial_wrath;
    damage_affected_by master_of_beasts;

    // Survival
    bool cull_the_herd = false;
    damage_affected_by spirit_bond;
    damage_affected_by tip_of_the_spear;
    damage_affected_by coordinated_assault;

    // Pack Leader
    damage_affected_by wyverns_cry;
    damage_affected_by lead_from_the_front;
  } affected_by;

  hunter_pet_action_t( util::string_view n, T_PET* p, const spell_data_t* s = spell_data_t::nil() ) :
    ab( n, p, s )
  {
    // If pets are not reported separately, create single stats_t objects for the various pet abilities.
    if ( ! ab::sim -> report_pets_separately )
    {
      auto first_pet = p -> owner -> find_pet( p -> name_str );
      if ( first_pet != nullptr && first_pet != p )
      {
        auto it = range::find( p -> stats_list, ab::stats );
        if ( it != p -> stats_list.end() )
        {
          p -> stats_list.erase( it );
          delete ab::stats;
          ab::stats = first_pet -> get_stats( ab::name_str, this );
        }
      }
    }

    if ( o()->talents.cull_the_herd.ok() )
      for ( size_t i = 1; i <= ab::data().effect_count(); i++ )
        if ( ab::data().mechanic() == MECHANIC_BLEED || ab::data().effectN( i ).mechanic() == MECHANIC_BLEED )
          affected_by.cull_the_herd = true;

    if ( o()->talents.unnatural_causes.ok() )
    {
      affected_by.unnatural_causes_debuff = check_affected_by( this, o()->talents.unnatural_causes_debuff->effectN( 1 ) );
      affected_by.unnatural_causes_debuff_label = check_affected_by( this, o()->talents.unnatural_causes_debuff->effectN( 2 ) );
    }

    affected_by.bestial_wrath = parse_damage_affecting_aura( this, o()->talents.bestial_wrath );
    affected_by.master_of_beasts = parse_damage_affecting_aura( this, o()->mastery.master_of_beasts );

    affected_by.spirit_bond = parse_damage_affecting_aura( this, o()->mastery.spirit_bond );
    affected_by.tip_of_the_spear = parse_damage_affecting_aura( this, o()->talents.tip_of_the_spear_buff );
    affected_by.coordinated_assault = parse_damage_affecting_aura( this, o()->talents.coordinated_assault );

    affected_by.wyverns_cry = parse_damage_affecting_aura( this, o()->talents.howl_of_the_pack_leader_wyvern_buff );
    affected_by.lead_from_the_front = parse_damage_affecting_aura( this, o()->talents.lead_from_the_front_buff );

    // Hunter Tree passives
    ab::apply_affecting_aura( o()->talents.specialized_arsenal );
    ab::apply_affecting_aura( o()->talents.unnatural_causes );

    // Beast Mastery Tree passives
    ab::apply_affecting_aura( o()->talents.aspect_of_the_beast );
    ab::apply_affecting_aura( o()->talents.savagery );

    // Survival Tree passives
    ab::apply_affecting_aura( o()->talents.frenzy_strikes );
    ab::apply_affecting_aura( o()->talents.tactical_advantage );
    ab::apply_affecting_aura( o()->talents.killer_companion );
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double am = ab::composite_da_multiplier( s );

    if ( affected_by.bestial_wrath.direct )
      am *= 1 + o()->buffs.bestial_wrath->check_value();

    if ( affected_by.master_of_beasts.direct )
      am *= 1 + o()->cache.mastery() * o()->mastery.master_of_beasts->effectN( affected_by.master_of_beasts.direct ).mastery_value();

    if ( affected_by.spirit_bond.direct )
    {
      double bonus = o()->cache.mastery() * o()->mastery.spirit_bond->effectN( affected_by.spirit_bond.direct ).mastery_value();
      bonus *= 1 + o()->mastery.spirit_bond_buff->effectN( 1 ).percent();
      am *= 1 + bonus;
    }

    if ( affected_by.coordinated_assault.direct && o()->buffs.coordinated_assault->check() )
      am *= 1 + o()->talents.coordinated_assault->effectN( affected_by.coordinated_assault.direct ).percent();

    if ( affected_by.tip_of_the_spear.direct && o()->buffs.tip_of_the_spear->check() )
      am *= 1 + o()->calculate_tip_of_the_spear_value( o()->talents.tip_of_the_spear->effectN( affected_by.tip_of_the_spear.direct ).percent() );

    if ( affected_by.wyverns_cry.direct )
      am *= 1 + o()->buffs.wyverns_cry->check_stack_value();

    if ( affected_by.lead_from_the_front.direct && o()->buffs.lead_from_the_front->check() )
      am *= 1 + o()->talents.lead_from_the_front_buff->effectN( affected_by.lead_from_the_front.direct ).percent();

    return am;
  }

  double composite_ta_multiplier( const action_state_t* s ) const override
  {
    double am = ab::composite_ta_multiplier( s );

    if ( affected_by.bestial_wrath.tick )
      am *= 1 + o()->buffs.bestial_wrath->check_value();

    if ( affected_by.spirit_bond.tick )
    {
      double bonus = o()->cache.mastery() * o()->mastery.spirit_bond->effectN( affected_by.spirit_bond.tick ).mastery_value();
      bonus *= 1 + o()->mastery.spirit_bond_buff->effectN( 3 ).percent();
      am *= 1 + bonus;
    }

    if ( affected_by.coordinated_assault.tick && o()->buffs.coordinated_assault->check() )
      am *= 1 + o()->talents.coordinated_assault->effectN( affected_by.coordinated_assault.tick ).percent();

    if ( affected_by.wyverns_cry.tick )
      am *= 1 + o()->buffs.wyverns_cry->check_stack_value();

    if ( affected_by.lead_from_the_front.tick && o()->buffs.lead_from_the_front->check() )
      am *= 1 + o()->talents.lead_from_the_front_buff->effectN( affected_by.lead_from_the_front.tick ).percent();

    return am;
  }

  double composite_target_da_multiplier( player_t* target ) const override
  {
    double da = ab::composite_target_da_multiplier( target );

    // Just assume the debuff will always be applied
    if ( affected_by.unnatural_causes_debuff || affected_by.unnatural_causes_debuff_label )
    {
      double execute_mod = 0.0;
      if ( target->health_percentage() < o()->talents.unnatural_causes->effectN( 3 ).base_value() )
        execute_mod = o()->talents.unnatural_causes->effectN( 2 ).percent();

      if ( affected_by.unnatural_causes_debuff )
        da *= 1 + o()->talents.unnatural_causes_debuff->effectN( 1 ).percent() * ( 1 + execute_mod );

      if ( affected_by.unnatural_causes_debuff_label )
        da *= 1 + o()->talents.unnatural_causes_debuff->effectN( 2 ).percent() * ( 1 + execute_mod );
    }

    return da;
  }

  double composite_target_ta_multiplier( player_t* target ) const override
  {
    double ta = ab::composite_target_ta_multiplier( target );

    if ( affected_by.cull_the_herd )
      ta *= 1 + o()->get_target_data( target )->debuffs.cull_the_herd->check_value();

    // Just assume the debuff will always be applied
    if ( affected_by.unnatural_causes_debuff || affected_by.unnatural_causes_debuff_label )
    {
      double execute_mod = 0.0;
      if ( target->health_percentage() < o()->talents.unnatural_causes->effectN( 3 ).base_value() )
        execute_mod = o()->talents.unnatural_causes->effectN( 2 ).percent();

      if ( affected_by.unnatural_causes_debuff )
        ta *= 1 + o()->talents.unnatural_causes_debuff->effectN( 1 ).percent() * ( 1 + execute_mod );

      if ( affected_by.unnatural_causes_debuff_label )
        ta *= 1 + o()->talents.unnatural_causes_debuff->effectN( 2 ).percent() * ( 1 + execute_mod );
    }

    return ta;
  }

  T_PET* p() { return static_cast<T_PET*>( ab::player ); }
  const T_PET* p() const { return static_cast<T_PET*>( ab::player ); }

  hunter_t* o() { return p()->o(); }
  const hunter_t* o() const { return p()->o(); }

  bool usable_moving() const override { return true; }
};

template <typename Pet>
struct hunter_pet_attack_t: public hunter_pet_action_t<Pet, melee_attack_t>
{
private:
  using ab = hunter_pet_action_t<Pet, melee_attack_t>;
public:

  hunter_pet_attack_t( util::string_view n, Pet* p, const spell_data_t* s ) : ab( n, p, s ) {}
};

template <typename Pet>
struct hunter_pet_melee_t: public hunter_pet_action_t<Pet, melee_attack_t>
{
private:
  using ab = hunter_pet_action_t<Pet, melee_attack_t>;
public:

  hunter_pet_melee_t( util::string_view n, Pet* p ):
    ab( n, p )
  {
    ab::background = ab::repeating = true;
    ab::special = false;

    ab::weapon = &( p->main_hand_weapon );
    ab::weapon_multiplier = 1;

    ab::base_execute_time = ab::weapon->swing_time;
    ab::school = SCHOOL_PHYSICAL;
    ab::may_crit = true;
  }

  timespan_t execute_time() const override
  {
    // There is a cap of ~.25s for pet auto attacks
    timespan_t t = ab::execute_time();
    if ( t < 0.25_s )
      t = 0.25_s;
    return t;
  }
};

// ==========================================================================
// Hunter Pet Attacks
// ==========================================================================

// Kill Command ============================================================

struct kill_command_bm_t: public hunter_pet_attack_t<hunter_main_pet_base_t>
{
  struct {
    double percent = 0;
    double multiplier = 1;
    benefit_t* benefit = nullptr;
  } killer_instinct;

  kill_command_bm_t( hunter_main_pet_base_t* p ) : hunter_pet_attack_t( "kill_command", p, p->o()->talents.kill_command_pet )
  {
    background = dual = proc = true;

    if ( o() -> talents.killer_instinct.ok() )
    {
      killer_instinct.percent = o() -> talents.killer_instinct -> effectN( 2 ).base_value();
      killer_instinct.multiplier = 1 + o() -> talents.killer_instinct -> effectN( 1 ).percent();
      killer_instinct.benefit = o() -> get_benefit( "killer_instinct" );
    }
  }

  void execute() override
  {
    hunter_pet_attack_t::execute();
  }

  void impact( action_state_t* s ) override
  {
    hunter_pet_attack_t::impact( s );

    if ( o() -> talents.kill_cleave.ok() && s -> action -> result_is_hit( s -> result ) &&
      s -> action -> sim -> active_enemies > 1 && p() -> hunter_pet_t::buffs.beast_cleave -> up() )
    {
      // Target multipliers do not replicate to secondary targets
      const double target_da_multiplier = ( 1.0 / s->target_da_multiplier );
      const double target_pet_multiplier = ( 1.0 / s->target_pet_multiplier );

      const double amount = s->result_total * o()->talents.kill_cleave->effectN( 1 ).percent() * target_da_multiplier * target_pet_multiplier;
      p()->actions.kill_cleave->execute_on_target( s->target, amount );
    }

    auto pet = o() -> pets.main;
    if ( pet == p() && o() -> talents.wild_instincts.ok() && o() -> buffs.call_of_the_wild -> check() )
    {
      o() -> get_target_data( s -> target ) -> debuffs.wild_instincts -> trigger();
    }

    if ( o()->talents.phantom_pain.ok() )
    {
      double replicate_amount = o()->talents.phantom_pain->effectN( 1 ).percent();
      for ( player_t* t : sim->target_non_sleeping_list )
      {
        if ( t->is_enemy() && !t->demise_event && t != s->target )
        {
          hunter_td_t* td = o()->get_target_data( t );
          if ( td->dots.black_arrow->is_ticking() )
          {
            double amount = replicate_amount * s->result_amount;
            o()->actions.phantom_pain->execute_on_target( t, amount );
          }
        }
      }
    }
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double da = hunter_pet_attack_t::composite_da_multiplier( s );

    // TODO 16/2/25: Alpha Predator and Pack Mentality are being combined additively before being applied.
    double bonus = o()->talents.alpha_predator->effectN( 2 ).percent();

    if ( o()->buffs.howl_of_the_pack_leader_wyvern->check()
      || o()->buffs.howl_of_the_pack_leader_boar->check()
      || o()->buffs.howl_of_the_pack_leader_bear->check() )
    {
      bonus += o()->talents.pack_mentality->effectN( 1 ).percent();
    }

    da *= 1 + bonus;

    if ( killer_instinct.percent )
    {
      const bool active = s->target->health_percentage() < killer_instinct.percent;
      killer_instinct.benefit->update( active );
      if ( active )
        da *= killer_instinct.multiplier;
    }

    return da;
  }
  
  double composite_crit_damage_bonus_multiplier() const override
  {
    double cm = hunter_pet_attack_t::composite_crit_damage_bonus_multiplier();

    if ( o() -> talents.go_for_the_throat.ok() )
    {
      cm *= 1 + o() -> talents.go_for_the_throat -> effectN( 2 ).percent() * o() -> cache.attack_crit_chance();
    }

    return cm;
  }

  double composite_target_multiplier( player_t* t ) const override
  {
    double am = hunter_pet_attack_t::composite_target_multiplier( t );

    am *= 1 + p()->get_target_data( t )->debuffs.venomous_bite->check_value();

    return am;
  }
};

struct kill_command_sv_t : public hunter_pet_attack_t<hunter_main_pet_t>
{
  kill_command_sv_t( hunter_main_pet_t* p ) : hunter_pet_attack_t( "kill_command", p, p->o()->talents.kill_command_pet )
  {
    if ( !o()->talents.bloodseeker.ok() )
      dot_duration = 0_ms;
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double da = hunter_pet_attack_t::composite_da_multiplier( s );

    // TODO 16/2/25: Alpha Predator and Pack Mentality are being combined additively before being applied.
    double bonus = o()->talents.alpha_predator->effectN( 2 ).percent();

    if ( o()->buffs.howl_of_the_pack_leader_wyvern->check()
      || o()->buffs.howl_of_the_pack_leader_boar->check()
      || o()->buffs.howl_of_the_pack_leader_bear->check() )
    {
      bonus += o()->talents.pack_mentality->effectN( 1 ).percent();
    }

    da *= 1 + bonus;

    return da;
  }
  
  void impact( action_state_t* s ) override
  {
    hunter_pet_attack_t::impact( s );

    if ( o()->talents.sentinel.ok() )
      o()->trigger_sentinel( s->target, false, o()->procs.sentinel_stacks );
  }
  
  void trigger_dot( action_state_t* s ) override
  {
    hunter_pet_attack_t::trigger_dot( s );

    o() -> trigger_bloodseeker_update();
  }

  void last_tick( dot_t* d ) override
  {
    hunter_pet_attack_t::last_tick( d );

    o() -> trigger_bloodseeker_update();
  }
};

// Beast Cleave ==============================================================

struct beast_cleave_attack_t: public hunter_pet_attack_t<hunter_pet_t>
{
  beast_cleave_attack_t( hunter_pet_t* p ) : hunter_pet_attack_t( "beast_cleave", p, p->find_spell( 118459 ) )
  {
    background = true;
    callbacks = proc = false;
    may_miss = may_crit = false;
    // The starting damage includes all the buffs
    base_dd_min = base_dd_max = 0;
    spell_power_mod.direct = attack_power_mod.direct = 0;
    weapon_multiplier = 0;

    aoe = -1;
    reduced_aoe_targets = data().effectN( 2 ).base_value();
  }

  void init() override
  {
    hunter_pet_attack_t::init();

    snapshot_flags |= STATE_TGT_MUL_DA;
    snapshot_flags |= STATE_TGT_MUL_PET;
  }

  size_t available_targets( std::vector< player_t* >& tl ) const override
  {
    hunter_pet_attack_t::available_targets( tl );

    // Cannot hit the original target.
    range::erase_remove( tl, target );

    return tl.size();
  }
};

// Kill Cleave ==============================================================

struct kill_cleave_t: public hunter_pet_attack_t<hunter_pet_t>
{
  kill_cleave_t( hunter_pet_t* p ) : hunter_pet_attack_t( "kill_cleave", p, p->find_spell( 389448 ) )
  {
    background = true;
    callbacks = proc = false;
    may_miss = may_crit = false;
    // The starting damage includes all the buffs
    base_dd_min = base_dd_max = 0;
    spell_power_mod.direct = attack_power_mod.direct = 0;
    weapon_multiplier = 0;

    aoe = -1;
    reduced_aoe_targets = data().effectN( 2 ).base_value();
  }

  void init() override
  {
    hunter_pet_attack_t::init();

    snapshot_flags |= STATE_TGT_MUL_DA;
    snapshot_flags |= STATE_TGT_MUL_PET;
  }

  size_t available_targets( std::vector< player_t* >& tl ) const override
  {
    hunter_pet_attack_t::available_targets( tl );

    // Cannot hit the original target.
    range::erase_remove( tl, target );

    return tl.size();
  }
};

// Melee ================================================================

struct pet_melee_t : public hunter_pet_melee_t<hunter_pet_t>
{
  pet_melee_t( util::string_view n, hunter_pet_t* p ) : hunter_pet_melee_t( n, p ) {}

  void impact( action_state_t* s ) override
  {
    hunter_pet_melee_t::impact( s );

    trigger_beast_cleave( s );
  }
};

struct main_pet_base_melee_t : public hunter_pet_melee_t<hunter_main_pet_base_t>
{
  main_pet_base_melee_t( util::string_view n, hunter_main_pet_base_t* p ) : hunter_pet_melee_t( n, p ) {}

  void impact( action_state_t* s ) override
  {
    hunter_pet_melee_t::impact( s );

    trigger_beast_cleave( s );

    if ( o()->rng().roll( o()->tier_set.tww_s1_bm_4pc->effectN( 1 ).percent() ) )
      o()->buffs.harmonize->trigger();

    if ( o()->buffs.wyverns_cry->check() )
      o()->buffs.wyverns_cry->increment( 1, buff_t::DEFAULT_VALUE(), o()->buffs.wyverns_cry->remains() );

    auto main_pet = dynamic_cast<hunter_main_pet_t*>( p() );
    if ( main_pet && main_pet->buffs.potent_mutagen->up() )
    {
      main_pet->actions.potent_mutagen->execute_on_target( s->target );
      o()->cooldowns.bestial_wrath->adjust( -timespan_t::from_seconds( main_pet->buffs.potent_mutagen->value() ) );
    }
  }
};

// Claw/Bite/Smack ======================================================

struct basic_attack_base_t : public hunter_pet_attack_t<hunter_main_pet_t>
{
  basic_attack_base_t( hunter_main_pet_t* p, util::string_view n, util::string_view suffix ) : 
    hunter_pet_attack_t( fmt::format("{}{}", n, suffix), p, p->find_pet_spell( n ) )
  {
    school = SCHOOL_PHYSICAL;
  }

  void impact( action_state_t* s ) override
  {
    hunter_pet_attack_t::impact( s );

    if ( result_is_hit( s -> result ) )
      trigger_beast_cleave( s );
  }
};

struct basic_attack_main_t final : public basic_attack_base_t
{
  struct {
    double cost_pct = 0;
    double multiplier = 1;
    benefit_t* benefit = nullptr;
  } wild_hunt;

  basic_attack_main_t( hunter_main_pet_t* p, util::string_view n, util::string_view options_str ) :
    basic_attack_base_t( p, n, "" )
  {
    parse_options( options_str );

    auto wild_hunt_spell = p -> find_spell( 62762 );
    wild_hunt.cost_pct = 1 + wild_hunt_spell -> effectN( 2 ).percent();
    wild_hunt.multiplier = 1 + wild_hunt_spell -> effectN( 1 ).percent();
    wild_hunt.benefit = p -> get_benefit( "wild_hunt" );

    p->actions.basic_attack = this;
  }

  bool use_wild_hunt() const
  {
    return p() -> resources.current[RESOURCE_FOCUS] > 50;
  }

  double action_multiplier() const override
  {
    double am = basic_attack_base_t::action_multiplier();

    const bool used_wild_hunt = use_wild_hunt();
    if ( used_wild_hunt )
      am *= wild_hunt.multiplier;
    wild_hunt.benefit -> update( used_wild_hunt );

    return am;
  }

  double cost_pct_multiplier() const override
  {
    double c = basic_attack_base_t::cost_pct_multiplier();

    if ( use_wild_hunt() )
      c *= wild_hunt.cost_pct;

    return c;
  }
};

struct no_mercy_ba_t : public basic_attack_base_t
{
  no_mercy_ba_t( hunter_main_pet_t* p, util::string_view n ) : basic_attack_base_t( p, n, "_no_mercy" )
  {
    background = true;
  }

  double cost() const override { return 0; }
};

struct brutal_companion_ba_t : public basic_attack_base_t
{
  brutal_companion_ba_t( hunter_main_pet_t* p, util::string_view n ) : basic_attack_base_t( p, n, "_brutal_companion" )
  {
    background = proc = true;
    base_multiplier *= 1 + o()->talents.brutal_companion->effectN( 2 ).percent();
  }
};

// Flanking Strike ===================================================

struct flanking_strike_t: public hunter_pet_attack_t<hunter_main_pet_t>
{
  flanking_strike_t( hunter_main_pet_t* p ) : hunter_pet_attack_t( "flanking_strike", p, p->o()->talents.flanking_strike_pet )
  {
    background = true;
  }

  double bonus_da( const action_state_t* s ) const override
  {
    double bonus = hunter_pet_attack_t::bonus_da( s );

    // Adding raw player ap (* effect 1 from talent spell tooltip).
    bonus += o()->cache.attack_power() * o()->composite_attack_power_multiplier() * o()->talents.flanking_strike->effectN( 1 ).percent();

    return bonus;
  }
};

// Coordinated Assault ====================================================

struct coordinated_assault_t: public hunter_pet_attack_t<hunter_main_pet_t>
{
  coordinated_assault_t( hunter_main_pet_t* p ) : hunter_pet_attack_t( "coordinated_assault", p, p->o()->talents.coordinated_assault_dmg )
  {
    background = true;
  }
};

// Stomp ===================================================================

struct stomp_t : public hunter_pet_attack_t<hunter_pet_t>
{
  struct cleave_t : public hunter_pet_attack_t<hunter_pet_t>
  {
    cleave_t( hunter_pet_t* p, util::string_view n, double effectiveness = 1.0 )
      : hunter_pet_attack_t( fmt::format( "{}_cleave", n ), p, p->o()->talents.stomp_cleave )
    {
      background = true;
      aoe = -1;
      base_dd_multiplier *= effectiveness;
    }

    size_t available_targets( std::vector< player_t* >& tl ) const override
    {
      hunter_pet_attack_t::available_targets( tl );

      range::erase_remove( tl, target );

      return tl.size();
    }
  };

  cleave_t* cleave;

  stomp_t( hunter_pet_t* p, util::string_view n = "stomp", double effectiveness = 1.0 )
    : hunter_pet_attack_t( n, p, p->o()->talents.stomp_primary ),
    cleave( new cleave_t( p, n, effectiveness ) )
  {
    background = true;
    base_dd_multiplier *= effectiveness;

    add_child( cleave );
  }

  void execute() override
  {
    hunter_pet_attack_t::execute();

    cleave->execute_on_target( target );
  }
};

// Bloodshed ===============================================================

struct bloodshed_t : hunter_pet_attack_t<hunter_main_pet_t>
{
  bloodshed_t( hunter_main_pet_t* p ) : hunter_pet_attack_t( "bloodshed", p, p->o()->talents.bloodshed_dot )
  {
    background = true;
    aoe = o() -> talents.shower_of_blood.ok() ? as<int>( o() -> talents.shower_of_blood -> effectN( 1 ).base_value() ) : 1;
  }

  void impact( action_state_t* s ) override
  {
    hunter_pet_attack_t::impact( s );

    p()->get_target_data( s->target )->hunter_main_pet_base_td_t::debuffs.venomous_bite->trigger( composite_dot_duration( s ) );

    if ( auto ac = o()->pets.animal_companion )
    {
      auto ac_td = ac->get_target_data( s->target );
      ac_td->debuffs.bloodshed->trigger();
      ac_td->hunter_main_pet_base_td_t::debuffs.venomous_bite->trigger( composite_dot_duration( s ) );
    }
  }
};

// Bestial Wrath ===========================================================

struct bestial_wrath_t : hunter_pet_attack_t<hunter_main_pet_base_t>
{
  bestial_wrath_t( hunter_main_pet_base_t* p ) : hunter_pet_attack_t( "bestial_wrath", p, p->find_spell( 344572 ) )
  {
    background = true;
  }
};

// Ravenous Leap (Fenryr) ===================================================

struct ravenous_leap_t : public hunter_pet_attack_t<fenryr_t>
{
  ravenous_leap_t( fenryr_t* p ) : hunter_pet_attack_t( "ravenous_leap", p, p->find_spell( 459753 ) )
  {
    background = true;
  }
};

// Rend Flesh (Bear) ===================================================

struct rend_flesh_t : public hunter_pet_attack_t<bear_t>
{
  struct envenomed_fangs_t : public hunter_pet_attack_t<bear_t>
  {
    envenomed_fangs_t( bear_t* p ) : hunter_pet_attack_t( "envenomed_fangs", p, p->o()->talents.envenomed_fangs_spell )
    {
      background = true;
    }
  };
  
  struct
  {
    double chance = 0;
    timespan_t reduction = 0_s;
    cooldown_t* cooldown = nullptr;
  } ursine_fury;

  envenomed_fangs_t* envenomed_fangs = nullptr;

  rend_flesh_t( bear_t* p ) : hunter_pet_attack_t( "rend_flesh", p, p->o()->talents.howl_of_the_pack_leader_bear_bleed )
  {
    background = true;
    aoe = as<int>( data().effectN( 2 ).base_value() );

    if ( o()->talents.ursine_fury.ok() )
    {
      ursine_fury.chance = o()->talents.ursine_fury_chance->proc_chance();

      if ( o()->specialization() == HUNTER_SURVIVAL )
      {
        if ( o()->talents.butchery.ok() )
          ursine_fury.cooldown = o()->cooldowns.butchery;

        if ( o()->talents.flanking_strike.ok() )
          ursine_fury.cooldown = o()->cooldowns.flanking_strike;

        ursine_fury.reduction = -o()->talents.ursine_fury->effectN( 3 ).time_value();
      }

      if ( o()->specialization() == HUNTER_BEAST_MASTERY )
      {
        ursine_fury.cooldown = o()->cooldowns.kill_command;
        ursine_fury.reduction = -o()->talents.ursine_fury->effectN( 2 ).time_value();
      }
    }

    if ( o()->talents.envenomed_fangs.ok() )
      envenomed_fangs = new envenomed_fangs_t( p );
  }

  dot_t* get_dot( player_t* t )
  {
    if ( !t )
      t = target;
    if ( !t )
      return nullptr;

    return p()->get_target_data( t )->dots.rend_flesh;
  }

  void tick( dot_t* d )
  {
    hunter_pet_attack_t::tick( d );

    if ( rng().roll( ursine_fury.chance ) )
      ursine_fury.cooldown->adjust( ursine_fury.reduction );
  }

  void impact( action_state_t* s ) override
  {
    hunter_pet_attack_t::impact( s );

    if ( o()->talents.envenomed_fangs.ok() )
    {
      dot_t* serpent_sting = o()->get_target_data( s->target )->dots.serpent_sting;
      if ( serpent_sting->is_ticking() )
      {
        auto new_state = serpent_sting->current_action->get_state( serpent_sting->state );
        new_state->result = RESULT_HIT;
        double tick_damage = serpent_sting->current_action->calculate_tick_amount( new_state, serpent_sting->current_stack() );
        action_state_t::release( new_state );

        double ticks_left = serpent_sting->ticks_left_fractional();
        sim->print_debug( "{} consumes {} with tick damage: {}, remaining ticks: {}", p()->name(), serpent_sting->name(), tick_damage, ticks_left );
        envenomed_fangs->execute_on_target( s->target, ticks_left * tick_damage );
        serpent_sting->cancel();
      }
    }
  }
};

// Potent Mutagen (The War Within Season 2 4 Piece Set Bonus) ======================

struct potent_mutagen_t : public hunter_pet_attack_t<hunter_main_pet_base_t>
{
  potent_mutagen_t( hunter_main_pet_base_t* p ) : hunter_pet_attack_t( "potent_mutagen", p, p->find_spell( 1218004 ) )
  {
    background = dual = true;
  }
};

} // end namespace pets::actions

fenryr_td_t::fenryr_td_t( player_t* target, fenryr_t* p ) : actor_target_data_t( target, p ), dots()
{
  dots.ravenous_leap = target->get_dot( "ravenous_leap", p );
};

bear_td_t::bear_td_t( player_t* target, bear_t* p ) : actor_target_data_t( target, p ), dots()
{
  dots.rend_flesh = target->get_dot( "rend_flesh", p );
};

hunter_main_pet_base_td_t::hunter_main_pet_base_td_t( player_t* target, hunter_main_pet_base_t* p ) : actor_target_data_t( target, p ), debuffs()
{
  debuffs.venomous_bite = make_buff( *this, "venomous_bite", p->o()->talents.venomous_bite_debuff )
    ->set_default_value_from_effect( 1 );
}

animal_companion_td_t::animal_companion_td_t( player_t* target, animal_companion_t* p ) : hunter_main_pet_base_td_t( target, p ), debuffs()
{
  debuffs.bloodshed = make_buff( *this, "bloodshed", p->o()->talents.bloodshed_debuff )
    ->set_default_value_from_effect( 2 );
}

hunter_main_pet_td_t::hunter_main_pet_td_t( player_t* target, hunter_main_pet_t* p ) : hunter_main_pet_base_td_t( target, p ), dots(), debuffs()
{
  dots.bloodseeker = target -> get_dot( "kill_command", p );
  dots.bloodshed = target -> get_dot( "bloodshed", p );

  debuffs.spearhead = make_buff( *this, "spearhead", p->o()->talents.spearhead_debuff )
    ->set_default_value_from_effect( 2 );
}

action_t* hunter_main_pet_t::create_action( util::string_view name, util::string_view options_str )
{
  if ( name == "claw" ) return new        actions::basic_attack_main_t( this, "Claw", options_str );
  if ( name == "bite" ) return new        actions::basic_attack_main_t( this, "Bite", options_str );
  if ( name == "smack" ) return new       actions::basic_attack_main_t( this, "Smack", options_str );

  return hunter_main_pet_base_t::create_action( name, options_str );
}

void hunter_pet_t::init_spells()
{
  pet_t::init_spells();

  if ( !main_hand_attack )
    main_hand_attack = new actions::pet_melee_t( "melee", this );
  
  actions.beast_cleave = new actions::beast_cleave_attack_t( this );
}

void stable_pet_t::init_spells()
{
  hunter_pet_t::init_spells();

  if ( o() -> talents.bloody_frenzy.ok() )
    actions.stomp = new actions::stomp_t( this );

  if ( o()->talents.thundering_hooves.ok() )
    actions.thundering_hooves = new actions::stomp_t( this, "thundering_hooves", o()->talents.thundering_hooves->effectN( 1 ).percent() );
}

void hunter_main_pet_base_t::init_spells()
{
  main_hand_attack = new actions::main_pet_base_melee_t( "melee", this );

  stable_pet_t::init_spells();

  if ( o()->specialization() == HUNTER_BEAST_MASTERY )
  {
    actions.kill_command = new actions::kill_command_bm_t( this );
    actions.bestial_wrath = new actions::bestial_wrath_t( this );
    
    if ( o() -> talents.kill_cleave.ok() )
      actions.kill_cleave = new actions::kill_cleave_t( this );

    if ( !stable_pet_t::actions.stomp && ( o()->talents.stomp.ok() || o()->talents.bloody_frenzy.ok() ) )
      stable_pet_t::actions.stomp = new actions::stomp_t( this );
  }
}

void animal_companion_t::init_spells()
{
  hunter_main_pet_base_t::init_spells();
}

void hunter_main_pet_t::init_spells()
{
  hunter_main_pet_base_t::init_spells();

  if ( o()->specialization() == HUNTER_SURVIVAL )
  {
    hunter_main_pet_base_t::actions.kill_command = new actions::kill_command_sv_t( this );

    if ( o()->talents.flanking_strike.ok() )
      actions.flanking_strike = new actions::flanking_strike_t( this );

    if ( o()->talents.coordinated_assault.ok() )
      actions.coordinated_assault = new actions::coordinated_assault_t( this );
  }
  else if ( o()->specialization() == HUNTER_BEAST_MASTERY )
  {
    if ( o()->talents.bloodshed.ok() )
      actions.bloodshed = new actions::bloodshed_t( this );

    if ( o()->talents.brutal_companion.ok() )
      actions.brutal_companion_ba = new actions::brutal_companion_ba_t( this, "Claw" );

    if ( o()->tier_set.tww_s2_bm_4pc.ok() )
      actions.potent_mutagen = new actions::potent_mutagen_t( this );
  }

  if ( o()->talents.no_mercy.ok() )
    actions.no_mercy_ba = new actions::no_mercy_ba_t( this, "Claw" );
}

void dire_critter_t::init_spells()
{
  hunter_pet_t::init_spells();
}

void dark_hound_t::init_spells()
{
  hunter_pet_t::init_spells();

  main_hand_attack->school = SCHOOL_SHADOW;
}

void fenryr_t::init_spells()
{
  hunter_pet_t::init_spells();

  actions.ravenous_leap = new actions::ravenous_leap_t( this );
}

void bear_t::init_spells()
{
  hunter_pet_t::init_spells();

  actions.rend_flesh = new actions::rend_flesh_t( this );
}

void hunter_main_pet_base_t::init_special_effects()
{
  stable_pet_t::init_special_effects();

  if ( o() -> talents.laceration.ok() )
  {
    struct laceration_cb_t : public dbc_proc_callback_t
    {
      double bleed_amount; 
      action_t* bleed; 

      laceration_cb_t( const special_effect_t& e, double amount, action_t* bleed ) : dbc_proc_callback_t( e.player, e ),
        bleed_amount( amount ), bleed( bleed )
      {
      }

      void execute( action_t*, action_state_t* s ) override
      {
        if ( s && s->target->is_sleeping() )
          return;

        if ( s )
        {
          double amount = s->result_amount * bleed_amount;
          if ( amount > 0 )
            residual_action::trigger( bleed, s->target, amount );
        }
      }
    };

    auto const effect = new special_effect_t( this );
    effect -> name_str = "laceration";
    effect -> spell_id =  o()->talents.laceration_driver->id();
    effect -> proc_flags2_ = PF2_CRIT;
    special_effects.push_back( effect );

    auto cb = new laceration_cb_t( *effect, o()->talents.laceration_driver->effectN( 1 ).percent(), o()->actions.laceration );
    cb -> initialize();
  }
}

template <typename Pet, size_t N>
struct active_pets_t
{
  using data_t = std::array<Pet*, N>;

  data_t data_;
  size_t active_;

  active_pets_t( data_t d, size_t n ):
    data_( d ), active_( n )
  {}

  auto begin() const { return data_.begin(); }
  auto end() const { return data_.begin() + active_; }
};

// returns the active pets from the list 'cast' to the supplied pet type
template <typename Pet, typename... Pets>
auto active( Pets... pets_ ) -> active_pets_t<Pet, sizeof...(Pets)>
{
  Pet* pets[] = { pets_... };
  typename active_pets_t<Pet, sizeof...(Pets)>::data_t active_pets{};
  size_t active_pet_count = 0;
  for ( auto pet : pets )
  {
    if ( pet && ! pet -> is_sleeping() )
      active_pets[ active_pet_count++ ] = pet;
  }

  return { active_pets, active_pet_count };
}

} // end namespace pets

namespace events {

struct tar_trap_aoe_t : public event_t
{
  hunter_t* p;
  double x_position, y_position;

  tar_trap_aoe_t( hunter_t* p, player_t* target, timespan_t t ) :
    event_t( *p -> sim, t ), p( p ),
    x_position( target -> x_position ), y_position( target -> y_position )
  { }

  const char* name() const override
  { return "Hunter-TarTrap-Aoe"; }

  void execute() override
  {
    if ( p -> state.tar_trap_aoe == this )
      p -> state.tar_trap_aoe = nullptr;
    p -> sim -> print_debug( "{} Tar Trap at {:.3f}:{:.3f} expired ({})", p -> name(), x_position, y_position, *this );
  }
};

} // end namespace events

namespace buffs {
} // end namespace buffs

void hunter_t::trigger_bloodseeker_update()
{
  if ( !talents.bloodseeker.ok() )
    return;

  int bleeding_targets = 0;
  for ( const player_t* t : sim -> target_non_sleeping_list )
  {
    if ( t -> is_enemy() && t -> debuffs.bleeding -> check() )
      bleeding_targets++;
  }
  bleeding_targets = std::min( bleeding_targets, buffs.bloodseeker -> max_stack() );

  const int current = buffs.bloodseeker -> check();
  if ( current < bleeding_targets )
  {
    buffs.bloodseeker -> trigger( bleeding_targets - current );
    if ( auto pet = pets.main )
      pet -> buffs.bloodseeker -> trigger( bleeding_targets - current );
  }
  else if ( current > bleeding_targets )
  {
    buffs.bloodseeker -> decrement( current - bleeding_targets );
    if ( auto pet = pets.main )
      pet -> buffs.bloodseeker -> decrement( current - bleeding_targets );
  }
}

int hunter_t::ticking_dots( hunter_td_t* td )
{
  int dots = 0;

  auto hunter_dots = td->dots;
  dots += hunter_dots.serpent_sting->is_ticking();
  dots += hunter_dots.wildfire_bomb->is_ticking();
  dots += hunter_dots.merciless_blow->is_ticking();
  dots += hunter_dots.spearhead->is_ticking();

  if ( pets.main )
  {
    auto pet_dots = pets.main->get_target_data( td->target )->dots;
    dots += pet_dots.bloodseeker->is_ticking();
  }

  if ( auto bear = pets.bear.active_pet() )
    dots += bear->get_target_data( td->target )->dots.rend_flesh->is_ticking();

  return dots;
}

void hunter_t::trigger_outland_venom_update()
{
  if ( !talents.outland_venom.ok() )
    return;

  for ( player_t* t : sim->target_non_sleeping_list )
  {
    if ( t->is_enemy() )
    {
      auto td = get_target_data( t );
      int current = td->debuffs.outland_venom->check();
      int new_stacks = ticking_dots( td );

      new_stacks = std::min( new_stacks, td->debuffs.outland_venom->max_stack() );

      if ( current < new_stacks )
        td->debuffs.outland_venom->trigger( new_stacks - current );
      else if ( current > new_stacks )
        td->debuffs.outland_venom->decrement( current - new_stacks );
    }
  }
}

void hunter_t::consume_trick_shots()
{
  if ( buffs.volley -> up() )
    return;

  buffs.trick_shots -> decrement();
}

void hunter_t::consume_precise_shots()
{
  if ( buffs.precise_shots->check() )
  {
    if ( talents.moving_target.ok() )
    {
      buffs.moving_target->trigger();
      buffs.streamline->trigger( buffs.precise_shots->check() );
    }

    cooldowns.explosive_shot->adjust( -talents.magnetic_gunpowder->effectN( 1 ).time_value() * buffs.precise_shots->check() );

    cooldowns.aimed_shot->adjust( -talents.focused_aim->effectN( 1 ).time_value() * buffs.precise_shots->check() );

    if ( talents.tensile_bowstring.ok() && buffs.trueshot->up() && state.tensile_bowstring_extension < talents.tensile_bowstring->effectN( 3 ).time_value() )
    {
      timespan_t extension = talents.tensile_bowstring->effectN( 1 ).time_value() * buffs.precise_shots->check();
      buffs.trueshot->extend_duration( this, extension );
      buffs.withering_fire->extend_duration( this, extension );
      state.tensile_bowstring_extension += extension;
    }
  }

  buffs.precise_shots->expire();
}

void hunter_t::trigger_spotters_mark( player_t* target, bool force )
{
  double chance = force ? 1.0 : specs.eyes_in_the_sky->effectN( 1 ).percent();
      
  if ( !force && talents.feathered_frenzy.ok() && buffs.trueshot->up() )
    chance *= 1 + talents.feathered_frenzy->effectN( 1 ).percent();
      
  if ( force || rng().roll( chance ) )
  {
    get_target_data( target )->debuffs.spotters_mark->trigger();
    
    if ( talents.ohnahran_winds.ok() )
    {
      int affected = 0;
      int max = as<int>( talents.ohnahran_winds->effectN( 1 ).base_value() );

      for ( player_t* t : sim->target_non_sleeping_list )
      {
        if ( t->is_enemy() && t != target && rng().roll( talents.ohnahran_winds->effectN( 2 ).percent() ) )
        {
          get_target_data( t )->debuffs.ohnahran_winds->trigger();
          affected++;
          if ( affected == max )
            break;
        }
      }
    }
  }
}

double hunter_t::calculate_tip_of_the_spear_value( double tip_bonus ) const
{
  if ( talents.flankers_advantage.ok() )
  {
    // Seems that the amount of the bonus given is based on the ratio of player crit % out of a cap of 50% from effect 5.
    double ratio = std::min( cache.attack_crit_chance(), talents.flankers_advantage->effectN( 5 ).percent() ) / talents.flankers_advantage->effectN( 5 ).percent();
    tip_bonus += tip_bonus * ratio;
  }

  if ( buffs.relentless_primal_ferocity->check() )
    tip_bonus *= 1 + talents.relentless_primal_ferocity_buff->effectN( 2 ).percent();

  return tip_bonus;
}

void hunter_t::trigger_deathblow( bool activated )
{
  if ( !talents.deathblow.ok() )
    return;

  procs.deathblow->occur();
  if ( activated )
  {
    buffs.deathblow->increment();
    if ( talents.razor_fragments.ok() )
      buffs.razor_fragments->increment();
  }
  else
  {
    buffs.deathblow->trigger();
    if ( talents.razor_fragments.ok() )
      buffs.razor_fragments->trigger();
  }
  
  talents.black_arrow.ok() ? cooldowns.black_arrow->reset( true ) : cooldowns.kill_shot->reset( true );
}

void hunter_t::trigger_sentinel( player_t* target, bool force, proc_t* proc )
{
  if ( force || buffs.eyes_closed->check() || rng().roll( 0.22 ) )
  {
    hunter_td_t* td = get_target_data( target );
    buff_t* sentinel = td->debuffs.sentinel;

    int stacks = sentinel->check();

    if ( proc )
      proc->occur();

    sentinel->trigger();
    if ( rng().roll( talents.release_and_reload->effectN( 1 ).percent() ) )
    {
      procs.release_and_reload_stacks->occur();
      sentinel->trigger();
    }

    if ( !stacks && talents.extrapolated_shots.ok() )
    {
      procs.extrapolated_shots_stacks->occur();
      sentinel->trigger( as<int>( talents.extrapolated_shots->effectN( 1 ).base_value() ) );
      // The stack from Extrapolated Shots has its own chance to roll a bonus stack from Release and Reload,
      // possibly generating 4 stacks at once.
      if ( rng().roll( talents.release_and_reload->effectN( 1 ).percent() ) )
      {
        procs.release_and_reload_stacks->occur();
        sentinel->trigger();
      }
    }

    // TODO: Seen strange behavior with multiple implosions triggering, ticks desyncing from the 2 second period by possibly 
    // overwriting or ticking in parallel, but for now model as just allowing one to tick at a time.
    if ( !td->sentinel_imploding && sentinel->check() > talents.sentinel->effectN( 1 ).base_value() && rng().roll( 0.32 ) )
    {
      procs.sentinel_implosions->occur();
      trigger_sentinel_implosion( td );
    }
  }
}

void hunter_t::trigger_sentinel_implosion( hunter_td_t* td )
{
  // Seems to tick one last time after it consumes the last Sentinel stack, resulting in a Sentinel 
  // re-application shortly after expiration but before the next tick to continue being consumed.
  if ( td->debuffs.sentinel->check() )
  {
    td->sentinel_imploding = true;
    actions.sentinel->execute_on_target( td->target );
    make_event( sim, 2_s, [ this, td ]() {
      if ( td->sentinel_imploding )
        trigger_sentinel_implosion( td );
    } );
  }
  else
  {
    td->sentinel_imploding = false;
  }
}

void hunter_t::trigger_symphonic_arsenal()
{
  if ( actions.symphonic_arsenal )
    for ( player_t* t : sim->target_non_sleeping_list )
      if ( t->is_enemy() && get_target_data( t )->debuffs.sentinel->check() )
        actions.symphonic_arsenal->execute_on_target( t );
}

void hunter_t::trigger_lunar_storm( player_t* target )
{
  if ( talents.lunar_storm.ok() )
  {
    buffs.lunar_storm_ready->expire();
    buffs.lunar_storm_cooldown->trigger();
    actions.lunar_storm_initial->execute_on_target( target );
    make_repeating_event(
        sim, talents.lunar_storm_periodic_trigger->effectN( 2 ).period(),
        [ this ] {
          auto& tl = actions.lunar_storm_periodic->target_list();
          if ( tl.size() )
          {
            rng().shuffle( tl.begin(), tl.end() );
            actions.lunar_storm_periodic->execute_on_target( tl.front() );
          }
        },
        as<int>( talents.lunar_storm_periodic_trigger->duration() / talents.lunar_storm_periodic_trigger->effectN( 2 ).period() ) );
  }
}

bool hunter_t::consume_howl_of_the_pack_leader( player_t* target )
{
  bool up = false;

  if ( buffs.howl_of_the_pack_leader_wyvern->check() )
  {
    up = true;
    buffs.wyverns_cry->trigger( as<int>( talents.howl_of_the_pack_leader->effectN( 3 ).base_value() + specs.survival_hunter->effectN( 12 ).base_value() ) );
    buffs.howl_of_the_pack_leader_wyvern->expire();
  }

  if ( buffs.howl_of_the_pack_leader_boar->check() )
  {
    up = true;
    make_event<ground_aoe_event_t>( *sim, this, 
      ground_aoe_params_t()
        .target( target )
        .duration( talents.howl_of_the_pack_leader_boar_charge_trigger->duration() )
        .pulse_time( talents.howl_of_the_pack_leader_boar_charge_trigger->effectN( 2 ).period() )
        .action( actions.boar_charge ),
      true );
    buffs.howl_of_the_pack_leader_boar->expire();

    if ( talents.hogstrider.ok() )
      buffs.mongoose_fury->extend_duration( this, buffs.mongoose_fury->buff_duration() - buffs.mongoose_fury->remains() );
  }

  if ( buffs.howl_of_the_pack_leader_bear->check() )
  {
    up = true;
    pets.bear.spawn( talents.howl_of_the_pack_leader_bear_summon->duration() + talents.dire_frenzy->effectN( 1 ).time_value() );
    buffs.howl_of_the_pack_leader_bear->expire();
  }

  // Only applied once even if two are summoned at once.
  if ( up )
  {
    cooldowns.barbed_shot->adjust( -talents.pack_mentality->effectN( 2 ).time_value() );
    cooldowns.wildfire_bomb->adjust( -talents.pack_mentality->effectN( 3 ).time_value() );
  }

  return up;
}

void hunter_t::trigger_howl_of_the_pack_leader()
{
  if ( state.howl_of_the_pack_leader_next_beast == WYVERN )
  {
    state.howl_of_the_pack_leader_next_beast = BOAR;
    buffs.howl_of_the_pack_leader_wyvern->trigger();
  }
  else if ( state.howl_of_the_pack_leader_next_beast == BOAR )
  {
    state.howl_of_the_pack_leader_next_beast = BEAR;
    buffs.howl_of_the_pack_leader_boar->trigger();
  }
  else if ( state.howl_of_the_pack_leader_next_beast == BEAR )
  {
    state.howl_of_the_pack_leader_next_beast = WYVERN;
    buffs.howl_of_the_pack_leader_bear->trigger();
  }
}

// ==========================================================================
// Hunter Attacks
// ==========================================================================

namespace attacks
{

template <typename Base>
struct auto_attack_base_t : hunter_action_t<Base>
{
private:
  using ab = hunter_action_t<Base>;

public:
  bool first = true;

  auto_attack_base_t( util::string_view n, hunter_t* p, const spell_data_t* s = spell_data_t::nil() ) :
    ab( n, p, s )
  {
    ab::allow_class_ability_procs = ab::not_a_proc = true;
    ab::background = ab::repeating = true;
    ab::interrupt_auto_attack = false;
    ab::special = false;
    ab::trigger_gcd = 0_ms;

    ab::weapon = &( p -> main_hand_weapon );
    ab::base_execute_time = ab::weapon -> swing_time;
  }

  void reset() override
  {
    ab::reset();
    first = true;
  }

  timespan_t execute_time() const override
  {
    if ( !ab::player -> in_combat )
      return 10_ms;
    if ( first )
      return 100_ms;
    return ab::execute_time();
  }

  void execute() override
  {
    first = false;
    ab::execute();
  }
};

// Auto Shot ================================================================

struct auto_shot_base_t : public auto_attack_base_t<ranged_attack_t>
{
  struct state_t : public action_state_t
  {
    using action_state_t::action_state_t;

    proc_types2 cast_proc_type2() const override
    {
      // Auto Shot seems to trigger Meticulous Scheming
      // (and possibly other effects that care about casts).
      return PROC2_CAST_DAMAGE;
    }
  };

  double snakeskin_quiver_chance = 0;
  double wild_call_chance = 0;

  auto_shot_base_t( util::string_view n, hunter_t* p, const spell_data_t* s ) : auto_attack_base_t( n, p, s )
  {
    wild_call_chance = p->talents.wild_call->effectN( 1 ).percent();
    snakeskin_quiver_chance = p->talents.snakeskin_quiver->effectN( 1 ).percent();
    
    if ( p->talents.precise_shots.ok() )
    {
      base_multiplier *= 1 + p->talents.precise_shots->effectN( 2 ).percent();
      base_execute_time += p->talents.precise_shots->effectN( 1 ).time_value();
    }
  }

  action_state_t* new_state() override
  {
    return new state_t( this, target );
  }

  void execute() override
  {
    auto_attack_base_t::execute();

    if ( rng().roll( snakeskin_quiver_chance ) )
    {
      p()->procs.snakeskin_quiver->occur();
      p()->actions.snakeskin_quiver->execute_on_target( target );
    }
  }

  void impact( action_state_t* s ) override
  {
    auto_attack_base_t::impact( s );

    if ( p() -> buffs.lock_and_load -> trigger() )
      p() -> cooldowns.aimed_shot -> reset( true );

    if ( s -> result == RESULT_CRIT && p() -> talents.wild_call.ok() && rng().roll( wild_call_chance ) )
    {
      p() -> cooldowns.barbed_shot -> reset( true );
      p() -> procs.wild_call -> occur();
    }
  }

  double action_multiplier() const override
  {
    double am = auto_attack_base_t::action_multiplier();

    if ( player -> buffs.heavens_nemesis )
      am *= 1 + player -> buffs.heavens_nemesis -> stack_value();

    return am;
  }

  timespan_t execute_time_flat_modifier() const override
  {
    timespan_t m = auto_attack_base_t::execute_time_flat_modifier();

    m += timespan_t::from_millis( p()->buffs.in_the_rhythm->check_value() );

    if ( p()->buffs.jackpot->check() )
      m += timespan_t::from_millis( p()->tier_set.tww_s2_mm_2pc->effectN( 2 ).trigger()->effectN( 2 ).base_value() );

    return m;
  }
};

struct auto_shot_t : public auto_shot_base_t
{
  auto_shot_t(hunter_t* p) : auto_shot_base_t( "auto_shot", p, p->specs.auto_shot )
  {
  }
};

//==============================
// Shared attacks
//==============================

struct residual_bleed_base_t : public residual_action::residual_periodic_action_t<hunter_ranged_attack_t>
{
  residual_bleed_base_t( util::string_view n, hunter_t* p, const spell_data_t* s )
    : residual_periodic_action_t( n, p, s )
  {
  }

  double base_ta( const action_state_t* s ) const override
  {
    double amount = residual_periodic_action_t::base_ta( s );
    
    // just assumed the debuff will always be applied
    if ( affected_by.unnatural_causes_debuff || affected_by.unnatural_causes_debuff_label )
    {
      double execute_mod = 0.0;
      if ( s->target->health_percentage() < p()->talents.unnatural_causes->effectN( 3 ).base_value() )
        execute_mod =  0.04; // for residual bleeds, execute damage bonus looks more like 14.4%, so we want 1.1 * 1.04 for our bonuses

      // only applying the execute bonus below, we should already have the 10% applied by the passive talent aura
      if ( affected_by.unnatural_causes_debuff )
        amount *= 1 + execute_mod;

      if ( affected_by.unnatural_causes_debuff_label )
        amount *= 1 + execute_mod;
    }

    return amount;
  }
};

// Steady Shot ========================================================================

struct steady_shot_t: public hunter_ranged_attack_t
{
  steady_shot_t( hunter_t* p, util::string_view options_str ):
    hunter_ranged_attack_t( "steady_shot", p, p -> specs.steady_shot )
  {
    parse_options( options_str );

    energize_type = action_energize::ON_CAST;
    energize_resource = RESOURCE_FOCUS;
    energize_amount = p->specs.steady_shot_energize->effectN( 1 ).base_value();
  }

  void execute() override
  {
    hunter_ranged_attack_t::execute();

    p()->cooldowns.aimed_shot->adjust( -data().effectN( 2 ).time_value() );
  }
};

// Arcane Shot ========================================================================

struct arcane_shot_base_t: public hunter_ranged_attack_t
{
  struct state_data_t
  {
    bool empowered_by_precise_shots = false;

    friend void sc_format_to( const state_data_t& data, fmt::format_context::iterator out )
    {
      fmt::format_to( out, "empowered_by_precise_shots={}", data.empowered_by_precise_shots );
    }
  };
  using state_t = hunter_action_state_t<state_data_t>;

  arcane_shot_base_t( util::string_view n, hunter_t* p ) : hunter_ranged_attack_t( n, p, p->specs.arcane_shot )
  {
  }

  void execute() override
  {
    hunter_ranged_attack_t::execute();

    p()->consume_precise_shots();

    p()->buffs.sulfurlined_pockets_building->trigger();
    if ( p()->buffs.sulfurlined_pockets_building->at_max_stacks() )
    {
      p()->buffs.sulfurlined_pockets_ready->trigger();
      p()->buffs.sulfurlined_pockets_building->expire();
    }
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double am = hunter_ranged_attack_t::composite_da_multiplier( s );

    am *= 1 + p()->buffs.precise_shots->check_stack_value();

    return am;
  }

  double composite_target_multiplier( player_t* target ) const override
  {
    double m = hunter_ranged_attack_t::composite_target_multiplier( target );

    m *= 1 + td( target )->debuffs.shrapnel_shot->value();

    return m;
  }

  double cost_pct_multiplier() const override
  {
    double c = hunter_ranged_attack_t::cost_pct_multiplier();

    if ( p()->buffs.precise_shots->check() )
      c *= 1 + p()->talents.precise_shots_buff->effectN( 3 ).percent();

    return c;
  }

  void impact( action_state_t* s ) override
  {
    hunter_ranged_attack_t::impact( s );

    if ( debug_cast<state_t*>( s )->empowered_by_precise_shots )
      p()->trigger_spotters_mark( s->target );

    td( s->target )->debuffs.shrapnel_shot->expire();
  }

  action_state_t* new_state() override
  {
    return new state_t( this, target );
  }

  void snapshot_internal( action_state_t* s, unsigned flags, result_amount_type rt ) override
  {
    hunter_ranged_attack_t::snapshot_internal( s, flags, rt );

    debug_cast<state_t*>( s )->empowered_by_precise_shots = p()->buffs.precise_shots->up();
  }
};

struct arcane_shot_t : public arcane_shot_base_t
{
  struct arcane_shot_aspect_of_the_hydra_t : arcane_shot_base_t
  {
    arcane_shot_aspect_of_the_hydra_t( util::string_view n, hunter_t* p ) : arcane_shot_base_t( n, p )
    {
      background = dual = true;
      base_costs[ RESOURCE_FOCUS ] = 0;
      base_multiplier *= p->talents.aspect_of_the_hydra->effectN( 1 ).percent();
    }
  };

  arcane_shot_aspect_of_the_hydra_t* aspect_of_the_hydra = nullptr;

  arcane_shot_t( hunter_t* p, util::string_view options_str ) : arcane_shot_base_t( "arcane_shot", p )
  {
    parse_options( options_str );

    if ( p->talents.aspect_of_the_hydra.ok() )
    {
      aspect_of_the_hydra = p->get_background_action<arcane_shot_aspect_of_the_hydra_t>( "arcane_shot_aspect_of_the_hydra" );
      add_child( aspect_of_the_hydra );
    }
  }

  void execute() override
  {
    arcane_shot_base_t::execute();

    auto tl = target_list();
    if ( aspect_of_the_hydra && tl.size() > 1 )
      aspect_of_the_hydra->execute_on_target( tl[ 1 ] );
  }
};

// Serpent Sting (Beast Mastery/Survival) =====================================================================

struct serpent_sting_t: public hunter_ranged_attack_t
{
  serpent_sting_t( util::string_view n, hunter_t* p ) : hunter_ranged_attack_t( n, p, p->specs.serpent_sting )
  {
    background = dual = true;
  }

  // We have a whole lot of Serpent Sting variations that all need to work with the same dot.
  dot_t* get_dot( player_t* t )
  {
    if ( !t )
      t = target;
    if ( !t )
      return nullptr;

    return td( t )->dots.serpent_sting;
  }
};

// Explosive Shot (Hunter Talent)  ====================================================================

struct explosive_shot_base_t : public hunter_ranged_attack_t
{
  struct damage_t final : hunter_ranged_attack_t
  {
    damage_t( util::string_view n, hunter_t* p ) : hunter_ranged_attack_t( n, p, p->talents.explosive_shot_damage )
    {
      background = dual = true;
      reduced_aoe_targets = p->talents.explosive_shot_cast->effectN( 2 ).base_value();
      aoe = -1;
    }

    void execute() override
    {
      // Need to update here with the damage spell multipliers since the state came from the cast
      // and the execute() call will skip player modifiers when a pre_execute_state exists
      if ( pre_execute_state )
      {
        update_state( pre_execute_state, result_amount_type::DMG_DIRECT );
        if ( pre_execute_state->persistent_multiplier > 1.0 )
          p()->procs.tww_s2_mm_4pc_explosive->occur();
      }

      hunter_ranged_attack_t::execute();
      
      p()->buffs.tip_of_the_spear_explosive->decrement();

      if ( p()->talents.thundering_hooves.ok() )
      {
        for ( auto pet : pets::active<pets::stable_pet_t>( p() -> pets.main, p() -> pets.animal_companion ) )
        {
          pet->actions.thundering_hooves->execute();
        }
        for ( auto pet : p() -> pets.cotw_stable_pet.active_pets() )
          pet->actions.thundering_hooves->execute();
      }
    }

    void impact( action_state_t* s ) override
    {
      hunter_ranged_attack_t::impact( s );

      td( s->target )->debuffs.shrapnel_shot->trigger();
    }

    double composite_da_multiplier( const action_state_t* s ) const override
    {
      double m = hunter_ranged_attack_t::composite_da_multiplier( s );
    
      m *= 1.0 + p()->buffs.precision_detonation_hidden->check_value();
      
      if ( p()->buffs.tip_of_the_spear_explosive->check() )
        m *= 1.0 + p()->calculate_tip_of_the_spear_value( p()->talents.tip_of_the_spear->effectN( 1 ).percent() );

      return m;
    }
  };

  timespan_t grenade_juggler_reduction = 0_s;
  damage_t* explosion = nullptr;

  explosive_shot_base_t( util::string_view n, hunter_t* p, const spell_data_t* s ) : hunter_ranged_attack_t( n, p, s )
  {
    may_miss = may_crit = false;

    explosion = p->get_background_action<damage_t>( "explosive_shot_damage" );
    grenade_juggler_reduction = p->talents.grenade_juggler->effectN( 3 ).time_value();
  }

  void init() override
  {
    hunter_ranged_attack_t::init();

    snapshot_flags = STATE_MUL_PERSISTENT;
  }

  virtual void update_state( action_state_t* s, result_amount_type rt )
  {
    hunter_ranged_attack_t::update_state( s, rt );

    s->persistent_multiplier = 1.0;
  }

  // We have a whole lot of Explosive Shot variations that all need to work with the same dot.
  dot_t* get_dot( player_t* t ) override
  {
    if ( !t )
      t = target;
    if ( !t )
      return nullptr;

    return td( t )->dots.explosive_shot;
  }

  void impact( action_state_t* s ) override
  {
    dot_t* dot = td( s->target )->dots.explosive_shot;

    if ( dot->is_ticking() )
    {
      if ( !explosion->pre_execute_state )
        explosion->pre_execute_state = explosion->get_state();
      
      // Either of the following scenarios will cause the detonation to have an effectiveness bonus and the last_tick() to have no bonus:
      // - an existing dot applied with an effectiveness bonus being detonated by a normal cast
      // - an existing dot applied by a normal cast being detonated by a cast with an effectiveness bonus
      // There is no way to test if a competing effectiveness bonus would be combined, overwritten, or would carry on to the last_tick(),
      // so just use the effectiveness bonus if it exists then clear the bonus from the dot state.
      // For Precision Detonation, a detonation from an Aimed Shot cast alongside an Explosive Shot with bonus effectiveness that itself 
      // detonates an existing dot before the Aimed Shot impact will result in both detonations having a bonus.
      // For that case, delay updating the dot state and clearing the effectiveness bonus until after the Aimed Shot would hit.
      if ( s->action->snapshot_flags & STATE_MUL_PERSISTENT )
        dot->state->persistent_multiplier = s->persistent_multiplier;

      explosion->pre_execute_state->copy_state( dot->state );
      explosion->execute_on_target( s->target );

      // Based on their travel times, about 200ms should always give Aimed Shot a chance to hit if cast alongside the Explosive Shot.
      make_event( sim, 200_ms, [ this, dot ]()
        {
          if ( dot->is_ticking() )
            update_state( dot->state, dot->state->result_type );
        } );
    }

    hunter_ranged_attack_t::impact( s );
  }

  void tick( dot_t* d ) override
  {
    // Prevent tick() from updating state so it can be used to clear the effectiveness bonus.
  }

  void last_tick( dot_t* d ) override
  {
    hunter_ranged_attack_t::last_tick( d );

    if ( !explosion->pre_execute_state )
      explosion->pre_execute_state = explosion->get_state();

    // The dot should have the state from the cast that triggered it, so forward it to the explosion.
    explosion->pre_execute_state->copy_state( d->state );
    explosion->execute_on_target( d->target );
  }

  void execute() override
  {
    hunter_ranged_attack_t::execute();
    
    p()->cooldowns.wildfire_bomb->adjust( -grenade_juggler_reduction );
    p()->buffs.bombardier->decrement();

    if ( p()->talents.precision_detonation.ok() )
      p()->buffs.streamline->trigger();
  }

  double cost_pct_multiplier() const override
  {
    double c = hunter_ranged_attack_t::cost_pct_multiplier();

    return c;
  }

  int n_targets() const override
  {
    if ( p()->buffs.bombardier->up() )
      return as<int>( p()->buffs.bombardier->data().effectN( 2 ).base_value() );

    return hunter_ranged_attack_t::n_targets();
  }
};

struct explosive_shot_t : public explosive_shot_base_t
{
  explosive_shot_t( hunter_t* p, util::string_view options_str )
    : explosive_shot_base_t( "explosive_shot", p, p->talents.explosive_shot )
  {
    parse_options( options_str );
  }

  void init() override
  {
    explosive_shot_base_t::init();

    if ( p()->specialization() == HUNTER_MARKSMANSHIP )
    {
      explosion->stats = stats;
      stats->action_list.push_back( explosion );
    }
  }
  
  void execute() override
  {
    explosive_shot_base_t::execute();

    if ( p()->buffs.tip_of_the_spear->up() )
    {
      p()->buffs.tip_of_the_spear->decrement();
      p()->buffs.tip_of_the_spear_explosive->trigger();
    }
  }

  void schedule_travel( action_state_t* s ) override
  {
    explosive_shot_base_t::schedule_travel( s );

    p()->state.traveling_explosive = this;
  }

  void impact( action_state_t* s ) override
  {
    explosive_shot_base_t::impact( s );

    p()->state.traveling_explosive = nullptr;
  }
};

struct explosive_shot_background_t : public explosive_shot_base_t
{
  explosive_shot_background_t( util::string_view n, hunter_t* p )
    : explosive_shot_base_t( n, p, p->talents.explosive_shot_cast )
  {
    background = dual = proc = true;
  }
};

// Kill Shot (Hunter Talent) ====================================================================

struct kill_shot_base_t : hunter_ranged_attack_t
{
  struct state_data_t
  {
    bool razor_fragments_up = false;
    bool empowered_by_precise_shots = false;

    friend void sc_format_to( const state_data_t& data, fmt::format_context::iterator out ) {
      fmt::format_to( out, "razor_fragments_up={}, empowered_by_precise_shots={}", data.razor_fragments_up, data.empowered_by_precise_shots );
    }
  };
  using state_t = hunter_action_state_t<state_data_t>;

  // Razor Fragments (Marksmanship Talent)
  struct razor_fragments_t : residual_bleed_base_t
  {
    double result_mod;

    razor_fragments_t( util::string_view n, hunter_t* p )
      : residual_bleed_base_t( n, p, p -> talents.razor_fragments_bleed )
    {
      result_mod = p -> talents.razor_fragments_buff -> effectN( 3 ).percent();
      aoe = as<int>( p -> talents.razor_fragments_buff -> effectN( 2 ).base_value() );
    }
  };

  double health_threshold_pct;
  razor_fragments_t* razor_fragments = nullptr;

  kill_shot_base_t( util::string_view n, hunter_t* p, spell_data_ptr_t s ) :
    hunter_ranged_attack_t( n, p, s ),
    health_threshold_pct( p -> talents.kill_shot -> effectN( 2 ).base_value() )
  {
    if ( p->talents.razor_fragments.ok() )
      razor_fragments = p -> get_background_action<razor_fragments_t>( "razor_fragments" );
  }

  void execute() override
  {
    hunter_ranged_attack_t::execute();

    if ( !background )
    {
      p()->buffs.deathblow->cancel();
      p()->buffs.razor_fragments->cancel();

      if ( p()->talents.headshot.ok() )
        p()->consume_precise_shots();
    }
  }

  void impact( action_state_t* s ) override
  {
    hunter_ranged_attack_t::impact( s );

    if ( razor_fragments && debug_cast<state_t*>( s ) -> razor_fragments_up && s -> chain_target < 1 )
    {
      double amount = s -> result_amount * razor_fragments -> result_mod;
      if ( amount > 0 )
      {
        std::vector<player_t*>& tl = target_list();
        for ( player_t* t : util::make_span( tl ).first( std::min( tl.size(), size_t( razor_fragments -> aoe ) ) ) )
          residual_action::trigger( razor_fragments, t, amount );
      }
    }

    if ( debug_cast<state_t*>( s )->empowered_by_precise_shots )
      p()->trigger_spotters_mark( s->target );

    if ( p()->talents.cull_the_herd.ok() )
      td( s->target )->debuffs.cull_the_herd->trigger();
  }

  int n_targets() const override
  {
    if ( p()->talents.sic_em.ok() && p()->buffs.deathblow->check() )
      return as<int>( p()->talents.sic_em->effectN( 2 ).base_value() );

    if ( p()->talents.hunters_prey.ok() )
    {
      int active = 0;
      for ( auto pet : pets::active<pets::hunter_pet_t>( p()->pets.main, p()->pets.animal_companion ) )
        active += pet->is_active();
      
      active += as<int>( p()->pets.cotw_stable_pet.n_active_pets() );

      return 1 + std::min( active, as<int>( p()->talents.hunters_prey_hidden_buff->max_stacks() ) );
    }

    return hunter_ranged_attack_t::n_targets();
  }

  bool target_ready( player_t* candidate_target ) override
  {
    return hunter_ranged_attack_t::target_ready( candidate_target ) &&
      ( candidate_target -> health_percentage() <= health_threshold_pct
        || p() -> buffs.deathblow -> check() );
  }

  double action_multiplier() const override
  {
    double am = hunter_ranged_attack_t::action_multiplier();

    am *= 1 + p()->buffs.razor_fragments->check_value();
    if ( p()->talents.hunters_prey.ok() )
    {
      int active = 0; 
      for ( auto pet : pets::active<pets::hunter_pet_t>( p()->pets.main, p()->pets.animal_companion ) )
        active += pet->is_active();
      
      active += as<int>( p()->pets.cotw_stable_pet.n_active_pets() );

      am *= 1 + p()->talents.hunters_prey_hidden_buff->effectN( 3 ).percent() * std::min( active, as<int>( p()->talents.hunters_prey_hidden_buff->max_stacks() ) );
    }

    return am;
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double am = hunter_ranged_attack_t::composite_da_multiplier( s );

    if ( p()->talents.headshot.ok() )
      am *= 1 + p()->buffs.precise_shots->check_stack_value() * p()->talents.headshot->effectN( 1 ).percent();

    return am;
  }

  action_state_t* new_state() override
  {
    return new state_t( this, target );
  }

  void snapshot_state( action_state_t* s, result_amount_type type ) override
  {
    hunter_ranged_attack_t::snapshot_state( s, type );
    debug_cast<state_t*>( s )->razor_fragments_up = p()->buffs.razor_fragments->check();
    debug_cast<state_t*>( s )->empowered_by_precise_shots = p()->talents.headshot.ok() && p()->buffs.precise_shots->up();
  }
};

struct kill_shot_t : public kill_shot_base_t
{
  kill_shot_t( hunter_t* p, util::string_view options_str )
    : kill_shot_base_t( "kill_shot", p, p->talents.kill_shot )
  {
    if ( p->talents.black_arrow.ok() )
      background = true;
    
    parse_options( options_str );
  }

  double composite_target_multiplier( player_t* target ) const override
  {
    double m = kill_shot_base_t::composite_target_multiplier( target );

    if ( p()->talents.born_to_kill && td( target )->debuffs.cull_the_herd->check() )
      m *= 1 + p()->talents.born_to_kill->effectN( 3 ).percent();

    return m;
  }

  void impact( action_state_t* s ) override
  {
    kill_shot_base_t::impact( s );

    if ( p()->talents.no_mercy.ok() && p()->cooldowns.no_mercy->up() )
    {
      p()->pets.main->actions.no_mercy_ba->execute_on_target( s->target );
      p()->cooldowns.no_mercy->start();
    }
  }
};

// Bursting Shot ======================================================================

struct bursting_shot_t : public hunter_ranged_attack_t
{
  bursting_shot_t( hunter_t* p, util::string_view options_str ) :
    hunter_ranged_attack_t( "bursting_shot", p, p -> talents.bursting_shot )
  {
    parse_options( options_str );
  }
};

// Black Arrow (Dark Ranger) =========================================================

struct black_arrow_base_t : public kill_shot_base_t
{
  struct black_arrow_dot_t : public hunter_ranged_attack_t
  {
    struct shadow_surge_t final : hunter_ranged_attack_t
    {
      shadow_surge_t( util::string_view n, hunter_t* p ) : hunter_ranged_attack_t( n, p, p->talents.shadow_surge_spell )
      {
        aoe = -1;
        background = dual = true;
        reduced_aoe_targets = p->talents.shadow_surge->effectN( 1 ).base_value();
      }
    };

    shadow_surge_t* shadow_surge = nullptr;
    timespan_t dark_hound_duration;
  
    black_arrow_dot_t( util::string_view n, hunter_t* p ) : hunter_ranged_attack_t( n, p, p->talents.black_arrow_dot )
    {
      background = dual = true;
      hasted_ticks = false;

      if ( p->talents.shadow_hounds.ok() )
        dark_hound_duration = p->talents.shadow_hounds_summon->duration();

      if ( p->talents.shadow_surge.ok() )
        shadow_surge = p->get_background_action<shadow_surge_t>( "shadow_surge" );
    }

    void tick( dot_t* d ) override
    {
      hunter_ranged_attack_t::tick( d );

      if ( shadow_surge && p()->rppm.shadow_surge->trigger() )
        shadow_surge->execute_on_target( d->target );

      if ( p()->talents.shadow_hounds.ok() && p()->rppm.shadow_hounds->trigger() )
      {
        p()->pets.dark_hound.spawn( dark_hound_duration );
        if ( !p()->pets.dark_hound.active_pets().empty() )
          p()->pets.dark_hound.active_pets().back()->buffs.beast_cleave->trigger( dark_hound_duration );
      }
    }
  };

  struct bleak_powder_t : public hunter_ranged_attack_t
  {
    bleak_powder_t( util::string_view n, hunter_t* p ) : hunter_ranged_attack_t( n, p, p->talents.bleak_powder_spell )
    {
      background = dual = true;
      aoe = -1;
    }

    size_t available_targets( std::vector<player_t*>& tl ) const override
    {
      hunter_ranged_attack_t::available_targets( tl );

      // Cannot hit the original target.
      range::erase_remove( tl, target );

      return tl.size();
    }
  };

  black_arrow_dot_t* black_arrow_dot = nullptr;
  bleak_powder_t* bleak_powder = nullptr;
  black_arrow_dot_t* dot;

  black_arrow_base_t( util::string_view n, hunter_t* p, spell_data_ptr_t s ) : kill_shot_base_t( n, p, s ),
    dot( p->get_background_action<black_arrow_dot_t>( "black_arrow_dot" ) )
  {
    impact_action = dot;

    if ( p->talents.bleak_powder.ok() )
      bleak_powder = p->get_background_action<bleak_powder_t>( "bleak_powder" );
  }

  void impact( action_state_t* s ) override
  {
    kill_shot_base_t::impact( s );

    if ( bleak_powder && ( p()->buffs.trick_shots->check() || p()->buffs.beast_cleave->check() ) && p()->cooldowns.bleak_powder->up() )
    {
      bleak_powder->execute_on_target( s->target );
      p()->cooldowns.bleak_powder->start();
    }
  }
};

struct black_arrow_t final : public black_arrow_base_t
{
  // Withering Fire (Dark Ranger) =========================================================
  struct withering_fire_t final : black_arrow_base_t
  {
    withering_fire_t( util::string_view n, hunter_t* p ) : black_arrow_base_t( n, p, p->talents.withering_fire_black_arrow )
    {
      background = dual = true;
    }

    // Ignore Kill Shot target count mods
    int n_targets() const override
    {
      return 1;
    }
  };

  struct
  {
    int count = 0;
    withering_fire_t* action = nullptr;
  } withering_fire;

  double lower_health_threshold_pct;
  double upper_health_threshold_pct;

  black_arrow_t( hunter_t* p, util::string_view options_str ) : black_arrow_base_t( "black_arrow", p, p->talents.black_arrow_spell ),
    lower_health_threshold_pct( data().effectN( 2 ).base_value() ),
    upper_health_threshold_pct( data().effectN( 3 ).base_value() )
  {
    parse_options( options_str );

    add_child( dot );
    if ( dot->shadow_surge )
      add_child( dot->shadow_surge );

    if ( p->talents.withering_fire.ok() )
    {
      withering_fire.count = as<int>( p->talents.withering_fire->effectN( 3 ).base_value() );
      withering_fire.action = p->get_background_action<withering_fire_t>( "black_arrow_withering_fire" );
      add_child( withering_fire.action );
    }
  }

  void execute() override
  {
    black_arrow_base_t::execute();

    if ( rng().roll( p()->talents.ebon_bowstring->effectN( 1 ).percent() ) )
      p()->trigger_deathblow( true );

    if ( p()->buffs.withering_fire->up() )
    {
      auto tl = target_list();
      withering_fire.action->execute_on_target( tl[ tl.size() > 1 ? 1 : 0 ] );
      withering_fire.action->execute_on_target( tl[ tl.size() > 2 ? 2 : 0 ] );
    }
  }

  void impact( action_state_t* s ) override
  {
    black_arrow_base_t::impact( s );

    // The chance is not in spell data and is hardcoded into the tooltip
    if ( p()->talents.banshees_mark.ok() && rng().roll( 0.25 ) && p()->cooldowns.banshees_mark->up() )
    {
      p()->actions.a_murder_of_crows->execute_on_target( s->target ); 
      p()->cooldowns.banshees_mark->start();
    }
  }

  bool target_ready( player_t* candidate_target ) override
  {
    // Black Arrow has different target ready conditionals than regular Kill Shot, so we don't call Kill Shot base.
    return hunter_ranged_attack_t::target_ready( candidate_target ) &&
      ( candidate_target->health_percentage() <= lower_health_threshold_pct ||
        ( p()->bugs && candidate_target->health_percentage() >= upper_health_threshold_pct ) ||
        ( p()->talents.the_bell_tolls.ok() && candidate_target->health_percentage() >= upper_health_threshold_pct ) ||
        p()->buffs.deathblow->check() );
  }
};

// Bleak Arrows (Dark Ranger)

struct bleak_arrows_t : public auto_shot_base_t
{
  double deathblow_chance; 

  bleak_arrows_t( hunter_t* p ) : auto_shot_base_t( "bleak_arrows", p, p->talents.bleak_arrows_spell ),
    deathblow_chance( p->talents.bleak_arrows->effectN( p->specialization() == HUNTER_MARKSMANSHIP ? 2 : 1 ).percent() )
  {
  }

  void impact( action_state_t* s ) override
  {
    auto_shot_base_t::impact( s );

    if ( rng().roll( deathblow_chance ) )
      p()->trigger_deathblow();
  }
};

// Phantom Pain (Dark Ranger) =========================================================

struct phantom_pain_t final : hunter_ranged_attack_t
{
  phantom_pain_t( hunter_t* p ) : hunter_ranged_attack_t( "phantom_pain", p, p->talents.phantom_pain_spell )
  {
    background = dual = true;
    base_dd_min = base_dd_max = 1.0;
  }
};

// Howl of the Pack Leader (Pack Leader)

struct boar_charge_t final : hunter_ranged_attack_t
{
  struct cleave_t : hunter_ranged_attack_t
  {
    double hogstrider_mongoose_fury_chance;

    cleave_t( hunter_t* p ) : hunter_spell_t( "boar_charge_cleave", p, p->talents.howl_of_the_pack_leader_boar_charge_cleave ),
      hogstrider_mongoose_fury_chance( p->talents.hogstrider->effectN( 1 ).percent() )
    {
      background = dual = true;
      aoe = as<int>( data().effectN( 2 ).base_value() );
    }

    size_t available_targets( std::vector< player_t* >& tl ) const override
    {
      hunter_ranged_attack_t::available_targets( tl );

      // TODO 31/1/25: currently hits primary target
      // range::erase_remove( tl, target );

      return tl.size();
    }

    void impact( action_state_t* s ) override
    {
      hunter_ranged_attack_t::impact( s );

      if ( rng().roll( hogstrider_mongoose_fury_chance ) )
        p()->buffs.mongoose_fury->trigger();

      p()->buffs.hogstrider->increment();
    }
  };

  cleave_t* cleave;

  boar_charge_t( hunter_t* p ) : hunter_spell_t( "boar_charge", p, p->talents.howl_of_the_pack_leader_boar_charge_impact ),
    cleave( new cleave_t( p ) )
  {
    background = dual = true;
  }

  void execute() override
  {
    hunter_spell_t::execute();

    cleave->execute_on_target( target );
  }

  void impact( action_state_t* s ) override
  {
    hunter_ranged_attack_t::impact( s );

    if ( rng().roll( cleave->hogstrider_mongoose_fury_chance ) )
      p()->buffs.mongoose_fury->trigger();

    p()->buffs.hogstrider->increment();
  }
};

// Sentinel (Sentinel) ==================================================================

struct sentinel_t : hunter_ranged_attack_t
{
  struct {
    double chance = 0.0;
    double gain = 0.0;
  } invigorating_pulse;

  struct {
    timespan_t reduction = 0_s;
    timespan_t limit = 0_s;
    cooldown_t* cooldown = nullptr;
  } sentinel_watch;

  sentinel_t( hunter_t* p ) : hunter_ranged_attack_t( "sentinel", p, p->talents.sentinel_tick )
  {
    background = dual = true;

    if ( p->talents.invigorating_pulse.ok() )
    {
      invigorating_pulse.chance = p->talents.invigorating_pulse->effectN( 2 ).percent();
      invigorating_pulse.gain = p->talents.invigorating_pulse->effectN( 1 ).base_value();
    }

    if ( p->talents.sentinel_watch.ok() )
    {
      sentinel_watch.reduction = timespan_t::from_seconds( p->talents.sentinel_watch->effectN( 1 ).base_value() );
      sentinel_watch.limit     = timespan_t::from_seconds( p->talents.sentinel_watch->effectN( 2 ).base_value() );

      if ( p->specialization() == HUNTER_SURVIVAL )
        sentinel_watch.cooldown = p->cooldowns.coordinated_assault;
      else if ( p->specialization() == HUNTER_MARKSMANSHIP )
        sentinel_watch.cooldown = p->cooldowns.trueshot;
    }
  }

  void execute() override
  {
    hunter_ranged_attack_t::execute();

    td( target )->debuffs.sentinel->decrement();
  }

  void impact( action_state_t* s ) override
  {
    hunter_ranged_attack_t::impact( s );

    if ( rng().roll( invigorating_pulse.chance ) )
      p()->resource_gain( RESOURCE_FOCUS, invigorating_pulse.gain, p()->gains.invigorating_pulse, this );

    if ( sentinel_watch.cooldown && p()->state.sentinel_watch_reduction < sentinel_watch.limit )
    {
      p()->state.sentinel_watch_reduction += sentinel_watch.reduction;
      sentinel_watch.cooldown->adjust( -sentinel_watch.reduction, true );
    }
  }
};

// Symphonic Arsenal (Sentinel) ============================================================

struct symphonic_arsenal_t : hunter_ranged_attack_t
{
  symphonic_arsenal_t( hunter_t* p ) : hunter_ranged_attack_t( "symphonic_arsenal", p, p->talents.symphonic_arsenal_spell )
  {
    background = dual = true;
    attack_power_mod.direct = p->specialization() == HUNTER_SURVIVAL ? p->talents.symphonic_arsenal_spell->effectN( 3 ).ap_coeff() : p->talents.symphonic_arsenal_spell->effectN( 1 ).ap_coeff();
    aoe = 1 + as<int>( p->talents.symphonic_arsenal->effectN( 1 ).base_value() );
  }
};

// Lunar Storm (Sentinel) ============================================================

struct lunar_storm_initial_t : hunter_ranged_attack_t
{
  lunar_storm_initial_t( hunter_t* p ) : hunter_ranged_attack_t( "lunar_storm_initial", p, p->talents.lunar_storm_initial_spell )
  {
    background = dual = true;
    aoe = -1;
  }
};

struct lunar_storm_periodic_t : hunter_ranged_attack_t
{
  lunar_storm_periodic_t( hunter_t* p ) : hunter_ranged_attack_t( "lunar_storm_periodic", p, p->talents.lunar_storm_periodic_spell )
  {
    background = dual = true;
  }

  void impact( action_state_t* s ) override
  {
    hunter_ranged_attack_t::impact( s );

    td( s->target )->debuffs.lunar_storm->trigger();
  }

  std::vector<player_t*>& target_list() const override
  {
    target_cache.is_valid = false;
    return hunter_ranged_attack_t::target_list();
  }

  size_t available_targets( std::vector<player_t*>& tl ) const override
  {
    hunter_ranged_attack_t::available_targets( tl );

    range::erase_remove( tl, [ this ]( player_t* t ) { return !td( t )->debuffs.sentinel->check(); } );

    return tl.size();
  }
};

// Rend Flesh (Pack Leader) ========================================================

// Used by the pet action to inherit damage modifiers from the player
struct rend_flesh_t : public hunter_melee_attack_t
{
  rend_flesh_t( hunter_t* p ) : hunter_melee_attack_t( "rend_flesh", p, p->talents.howl_of_the_pack_leader_bear_bleed ) {}
};

//==============================
// Beast Mastery attacks
//==============================

// Multi-Shot =================================================================

struct multishot_bm_t: public hunter_ranged_attack_t
{
  multishot_bm_t( hunter_t* p, util::string_view options_str ):
    hunter_ranged_attack_t( "multishot", p, p -> talents.multishot_bm )
  {
    parse_options( options_str );

    aoe = -1;
    reduced_aoe_targets = data().effectN( 1 ).base_value();
  }

  void execute() override
  {
    hunter_ranged_attack_t::execute();

    if ( p()->talents.beast_cleave->ok() && p()->buffs.beast_cleave->buff_duration() > p()->buffs.beast_cleave->remains() ) {

      p() -> buffs.beast_cleave -> trigger();
      
      for ( auto pet : pets::active<pets::hunter_pet_t>( p() -> pets.main, p() -> pets.animal_companion ) )
        pet -> buffs.beast_cleave -> trigger();
    }
  }
};

// Cobra Shot =================================================================

struct cobra_shot_t: public hunter_ranged_attack_t
{
  const timespan_t kill_command_reduction;

  cobra_shot_t( hunter_t* p, util::string_view options_str ):
    hunter_ranged_attack_t( "cobra_shot", p, p -> talents.cobra_shot ),
    kill_command_reduction( -timespan_t::from_seconds( data().effectN( 3 ).base_value() ) )
  {
    parse_options( options_str );
  }

  int n_targets() const override
  {
    int n = hunter_ranged_attack_t::n_targets();

    n += p()->buffs.hogstrider->check();

    return n;
  }

  void execute() override
  {
    hunter_ranged_attack_t::execute();

    if ( p() -> talents.killer_cobra.ok() && p() -> buffs.bestial_wrath -> check() )
      p() -> cooldowns.kill_command -> reset( true );
    
    if ( p()->talents.serpentine_rhythm.ok() )
    {
      if ( p()->buffs.serpentine_rhythm->at_max_stacks() )
      {
        p()->buffs.serpentine_rhythm->expire();
        p()->buffs.serpentine_blessing->trigger();
      }
      else
      {
        p()->buffs.serpentine_rhythm->trigger();
      }
    }

    if ( p()->talents.barbed_scales.ok() )
      p()->cooldowns.barbed_shot->adjust( -p()->talents.barbed_scales->effectN( 1 ).time_value() );

    p()->buffs.howl_of_the_pack_leader_cooldown->extend_duration( p(), -p()->talents.dire_summons->effectN( 3 ).time_value() );

    p()->buffs.hogstrider->expire();
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double m = hunter_ranged_attack_t::composite_da_multiplier( s );

    m *= 1.0 + p()->buffs.serpentine_rhythm->check_stack_value();
    m *= 1 + p()->buffs.hogstrider->check_stack_value();

    return m;
  }

  void schedule_travel( action_state_t* s ) override
  {
    hunter_ranged_attack_t::schedule_travel( s );

    p() -> cooldowns.kill_command -> adjust( kill_command_reduction );
  }
};

// Cobra Shot (Snakeskin Quiver)

struct cobra_shot_snakeskin_quiver_t: public cobra_shot_t
{
  cobra_shot_snakeskin_quiver_t( hunter_t* p ):
    cobra_shot_t( p, "" )
  {
    background = dual = true;
    base_costs[ RESOURCE_FOCUS ] = 0;
  }
};

// Barbed Shot ===============================================================

struct barbed_shot_t: public hunter_ranged_attack_t
{
  struct poisoned_barbs_t final : hunter_ranged_attack_t
  {
    serpent_sting_t* poisoned_barbs_serpent_sting = nullptr;

    poisoned_barbs_t( util::string_view n, hunter_t* p ) : hunter_ranged_attack_t( "poisoned_barbs", p, p->find_spell( 1217549 ) )
    {
      background = dual = true;
      aoe = -1;
      reduced_aoe_targets = p->talents.poisoned_barbs->effectN( 2 ).base_value();

      poisoned_barbs_serpent_sting = p->get_background_action<serpent_sting_t>( "serpent_sting" );
    }

    void impact( action_state_t* s ) override
    {
      hunter_ranged_attack_t::impact( s );

      poisoned_barbs_serpent_sting->execute_on_target( s->target );
    }
  };

  timespan_t bestial_wrath_reduction;
  action_t* poisoned_barbs = nullptr;

  barbed_shot_t( hunter_t* p, util::string_view options_str ) :
    hunter_ranged_attack_t( "barbed_shot", p, p -> talents.barbed_shot )
  {
    parse_options(options_str);

    bestial_wrath_reduction = p -> talents.barbed_wrath -> effectN( 1 ).time_value();

    tick_zero = true; 

    p -> actions.barbed_shot = this;

    if ( p->talents.poisoned_barbs.ok() )
      poisoned_barbs = p->get_background_action<poisoned_barbs_t>( "poisoned_barbs" );
  }

  void execute() override
  {
    hunter_ranged_attack_t::execute();

    // trigger regen buff
    auto it = range::find_if( p() -> buffs.barbed_shot, []( buff_t* b ) { return !b -> check(); } );
    if ( it != p() -> buffs.barbed_shot.end() )
      ( *it ) -> trigger();
    else if ( sim -> debug )
      sim -> out_debug.print( "{} {} unable to trigger excess Barbed Shot buff", player -> name(), name() );

    p() -> buffs.thrill_of_the_hunt -> trigger();

    p() -> cooldowns.bestial_wrath -> adjust( -bestial_wrath_reduction );

    if ( rng().roll( p() -> talents.war_orders -> effectN( 3 ).percent() ) )
      p() -> cooldowns.kill_command -> reset( true );

    for ( auto pet : pets::active<pets::hunter_main_pet_base_t>( p() -> pets.main, p() -> pets.animal_companion ) )
    {
      if ( p() -> talents.stomp.ok() )
        pet -> stable_pet_t::actions.stomp -> execute();

      pet -> buffs.frenzy -> trigger();
      pet -> buffs.thrill_of_the_hunt -> trigger();
    }

    auto pet = p() -> pets.main;
    if ( pet && pet -> hunter_main_pet_base_t::buffs.frenzy -> check() == as<int>( p() -> talents.brutal_companion -> effectN( 1 ).base_value() ) )
    {
      pet -> actions.brutal_companion_ba -> execute_on_target( target );
    }
  }

  void impact( action_state_t* s ) override
  {
    hunter_ranged_attack_t::impact( s );

    if ( poisoned_barbs && rng().roll( p()->talents.poisoned_barbs->effectN( 1 ).percent() ) )
      poisoned_barbs->execute_on_target( s->target );
  }

  void tick( dot_t* d ) override
  {
    hunter_ranged_attack_t::tick( d );

    if ( p() -> talents.master_handler -> ok() )
    {
      p() -> cooldowns.kill_command -> adjust( -p() -> talents.master_handler -> effectN( 1 ).time_value() );
    }
  }
};

// Barrage (Beast Mastery Talent) ===========================================

struct barrage_t: public hunter_spell_t
{
  struct barrage_damage_t final : public hunter_ranged_attack_t
  {
    barrage_damage_t( util::string_view n, hunter_t* p ):
      hunter_ranged_attack_t( n, p, p -> talents.barrage -> effectN( 1 ).trigger() )
    {
      background = dual = true;
      aoe = -1;
      reduced_aoe_targets = data().effectN( 1 ).base_value();
    }
  };

  action_t* barrage_tick = nullptr;

  barrage_t( hunter_t* p, util::string_view options_str ):
    hunter_spell_t( "barrage", p, p -> talents.barrage )
  {
    parse_options( options_str );

    may_miss = may_crit = false;
    channeled = true;

    tick_action = p->get_background_action<barrage_damage_t>( "barrage_damage" );

    starved_proc = p -> get_proc( "starved: barrage" );
  }

  void execute() override
  {
    hunter_spell_t::execute();

    if ( p()->buffs.beast_cleave->buff_duration() > p()->buffs.beast_cleave->remains() )
    {
      p() -> buffs.beast_cleave -> trigger(); 
      for ( auto pet : pets::active<pets::hunter_pet_t>( p() -> pets.main, p() -> pets.animal_companion ) )
        pet -> buffs.beast_cleave -> trigger();
    }
  }
};

// Laceration (Beast Mastery Talent) ==============================================

struct laceration_t : public residual_bleed_base_t
{
  laceration_t( hunter_t* p ) : residual_bleed_base_t( "laceration", p, p->talents.laceration_bleed ) {}
};

// Barbed Shot (Empowered) (The War Within Season 2 2 Piece Set Bonus) ==============

struct barbed_shot_tww_s2_bm_2pc_t : public barbed_shot_t
{
  barbed_shot_tww_s2_bm_2pc_t( util::string_view n, hunter_t* p ) : barbed_shot_t( p, "" )
  {
    background = dual = proc = true;
  }

  void init() override
  {
    barbed_shot_t::init();

    snapshot_flags |= STATE_MUL_PERSISTENT;
  }

  double composite_persistent_multiplier( const action_state_t *state ) const override
  {
    double pm = barbed_shot_t::composite_persistent_multiplier( state );

    pm *= p()->tier_set.tww_s2_bm_2pc->effectN( 1 ).percent();

    return pm;
  }
};

//==============================
// Marksmanship attacks
//==============================

// Master Marksman ====================================================================

struct master_marksman_t : public residual_bleed_base_t
{
  master_marksman_t( hunter_t* p ) : residual_bleed_base_t( "master_marksman", p, p->talents.master_marksman_bleed ) {}
};

// Multi-Shot =================================================================

struct multishot_mm_t: public hunter_ranged_attack_t
{
  struct state_data_t
  {
    bool empowered_by_precise_shots = false;

    friend void sc_format_to( const state_data_t& data, fmt::format_context::iterator out )
    {
      fmt::format_to( out, "empowered_by_precise_shots={}", data.empowered_by_precise_shots );
    }
  };
  using state_t = hunter_action_state_t<state_data_t>;

  multishot_mm_t( hunter_t* p, util::string_view options_str ) : hunter_ranged_attack_t( "multishot", p, p->specs.multishot )
  {
    parse_options( options_str );

    aoe = -1;
    reduced_aoe_targets = p -> find_spell( 2643 ) -> effectN( 1 ).base_value();
  }

  void execute() override
  {
    hunter_ranged_attack_t::execute();

    p()->consume_precise_shots();

    if ( ( p() -> talents.trick_shots.ok() && num_targets_hit >= p() -> talents.trick_shots -> effectN( 2 ).base_value() ) )
      p() -> buffs.trick_shots -> trigger();

    p()->trigger_symphonic_arsenal();
  }

  void schedule_travel( action_state_t* s ) override
  {
    hunter_ranged_attack_t::schedule_travel( s );
  }

  void impact( action_state_t* s ) override
  {
    hunter_ranged_attack_t::impact( s );

    // Multi-Shot only ever seems to trigger Spotter's Mark on the primary target
    if ( s->chain_target == 0 && debug_cast<state_t*>( s )->empowered_by_precise_shots )
      p()->trigger_spotters_mark( s->target );

    td( s->target )->debuffs.shrapnel_shot->expire();

    p()->cooldowns.rapid_fire->adjust( -p()->talents.bullet_hell->effectN( 1 ).time_value() );
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double m = hunter_ranged_attack_t::composite_da_multiplier( s );

    m *= 1 + p()->buffs.precise_shots->check_stack_value();

    return m;
  }

  double composite_target_multiplier( player_t* target ) const override
  {
    double m = hunter_ranged_attack_t::composite_target_multiplier( target );

    m *= 1 + td( target )->debuffs.shrapnel_shot->value();

    return m;
  }

  double cost_pct_multiplier() const override
  {
    double c = hunter_ranged_attack_t::cost_pct_multiplier();

    if ( p()->buffs.precise_shots->check() )
      c *= 1 + p()->talents.precise_shots_buff->effectN( 3 ).percent();

    return c;
  }

  action_state_t* new_state() override
  {
    return new state_t( this, target );
  }

  void snapshot_internal( action_state_t* s, unsigned flags, result_amount_type rt ) override
  {
    hunter_ranged_attack_t::snapshot_internal( s, flags, rt );

    debug_cast<state_t*>( s )->empowered_by_precise_shots = p()->buffs.precise_shots->up();
  }
};

// Aimed Shot =========================================================================

struct aimed_shot_base_t : public hunter_ranged_attack_t
{
  const int trick_shots_targets;

  timespan_t target_acquisition_reduction;

  aimed_shot_base_t( util::string_view n, hunter_t* p, spell_data_ptr_t s ) :
    hunter_ranged_attack_t( n, p, s ),
    trick_shots_targets( as<int>( p->talents.trick_shots_data->effectN( 1 ).base_value() ) ),
    target_acquisition_reduction( p->talents.target_acquisition->effectN( 1 ).time_value() )
  {
    radius = 8;
    base_aoe_multiplier = p->talents.trick_shots_data->effectN( 4 ).percent();
  }

  double action_multiplier() const override
  {
    double am = hunter_ranged_attack_t::action_multiplier();

    // TODO 20/1/25: Aimed Shots that are mid cast when Lock and Load triggers
    // are affected by the Quickdraw bonus without consuming it
    if ( p()->buffs.lock_and_load->check() )
      am *= 1 + p()->talents.quickdraw->effectN( 1 ).percent();

    return am;
  }

  double composite_da_multiplier( const action_state_t* s ) const override
  {
    double m = hunter_ranged_attack_t::composite_da_multiplier( s );

    m *= 1 + p()->buffs.bulletstorm->check_stack_value();
    m *= 1 + p()->buffs.moving_target->value();

    return m;
  }

  double composite_target_da_multiplier( player_t* t ) const override
  {
    double m = hunter_ranged_attack_t::composite_target_da_multiplier( t );

    m *= 1 + td( target )->debuffs.spotters_mark->check_value();
    m *= 1 + td( target )->debuffs.ohnahran_winds->check_value();

    return m;
  }

  double composite_crit_damage_bonus_multiplier() const override
  {
    double cm = hunter_ranged_attack_t::composite_crit_damage_bonus_multiplier();

    cm *= 1 + p()->buffs.bulletstorm->check() * p()->talents.incendiary_ammunition->effectN( 1 ).percent();

    return cm;
  }

  double composite_target_crit_chance( player_t* target ) const override
  {
    double c = hunter_ranged_attack_t::composite_target_crit_chance( target );

    if ( p()->talents.killer_mark.ok() && td( target )->debuffs.spotters_mark->check() )
      c += p()->talents.killer_mark->effectN( 1 ).percent();

    if ( p()->talents.killer_mark.ok() && td( target )->debuffs.ohnahran_winds->check() )
      c += p()->talents.killer_mark->effectN( 1 ).percent();

    return c;
  }

  void execute() override
  {
    if ( is_aoe() )
      target_cache.is_valid = false;

    hunter_ranged_attack_t::execute();

    // TODO 23/1/25: Moving Target gets consumed by all forms of Aimed Shot casts
    // for example casting an Aimed Shot with an Arcane Shot queued immediately after will apply the 
    // Moving Target triggered by the Arcane Shot to an Aspect of the Hydra or Double Tap Aimed Shot
    // and consume the Moving Target
    // likewise if the primary Aimed Shot cast consumes a Moving Target and it is not triggered again
    // with a queued Precise Shot spender, a secondary Aimed Shot will not receive a bonus from it
    p()->buffs.moving_target->expire();
  }

  int n_targets() const override
  {
    if ( p()->buffs.trick_shots->check() )
      return 1 + trick_shots_targets;

    return hunter_ranged_attack_t::n_targets();
  }

  void impact( action_state_t* s ) override
  {
    hunter_ranged_attack_t::impact( s );

    hunter_td_t* target_data = td( s->target );

    // TODO 25/1/25: All forms of Aimed Shot impacts reliably trigger both existing and queued Explosive Shots, but there is a lot 
    // of inconsistency on bonus applications of Precision Detonation and TWW S2 4pc, perhaps tied to the trigger and expiration 
    // timing of the hidden buff 474199. Pray for fixes and buff all for now.
    if ( p()->talents.precision_detonation->ok() )
    {
      bool ticking = target_data->dots.explosive_shot->is_ticking();
      if ( s->chain_target == 0 )
      {
        bool traveling = p()->state.traveling_explosive && p()->state.traveling_explosive->has_travel_events_for( s->target );

        if ( ticking || traveling )
        {
          p()->buffs.precision_detonation_hidden->trigger();

          // Precision Detonation will affect an Explosive Shot queued immediately after an Aimed Shot, which intuitively shouldn't work
          // since they have the same travel time and the Aimed Shot should impact before the Explosive Shot, so it seems there's some
          // fudging going on to make it work, once again probably related to the use and timing of the hidden buff 474199, so just 
          // schedule the detonation to occur after the next Explosive Shot impact if there is one in flight.
          if ( traveling )
            make_event( p()->sim, p()->state.traveling_explosive->shortest_travel_event(), [ this, target_data ]()
              {
                p()->procs.precision_detonation->occur();
                target_data->dots.explosive_shot->cancel();
                p()->buffs.precision_detonation_hidden->expire();
              } );
          else
            // Expire Precision Detonation after other possible impacts.
            make_event( p()->sim, [ this ]() { p()->buffs.precision_detonation_hidden->expire(); } );
        }
        else
        {
          // Expire Precision Detonation after other possible impacts in case they trigger the buff.
          make_event( p()->sim, [ this ]() { p()->buffs.precision_detonation_hidden->expire(); } );
        }
      }

      if ( ticking )
      {
        p()->procs.precision_detonation->occur();
        p()->buffs.precision_detonation_hidden->trigger();
        target_data->dots.explosive_shot->cancel();
      }
    }

    if ( p()->talents.phantom_pain.ok() )
    {
      double replicate_amount = p()->talents.phantom_pain->effectN( 1 ).percent();
      for ( player_t* t : sim->target_non_sleeping_list )
      {
        if ( t->is_enemy() && !t->demise_event && t != s->target )
        {
          hunter_td_t* td = p()->get_target_data( t );
          if ( td->dots.black_arrow->is_ticking() )
          {
            double amount = replicate_amount * s->result_amount;
            p()->actions.phantom_pain->execute_on_target( t, amount );
          }
        }
      }
    }

    if ( target_data->debuffs.spotters_mark->check() || target_data->debuffs.ohnahran_winds->check() )
    {
      target_data->debuffs.spotters_mark->expire();
      target_data->debuffs.ohnahran_winds->expire();
      
      p()->buffs.on_target->trigger();
      
      if ( p()->talents.target_acquisition.ok() && p()->cooldowns.target_acquisition->up() )
      {
        p()->cooldowns.target_acquisition->start();
        p()->cooldowns.aimed_shot->adjust( -target_acquisition_reduction );
      }

      p()->cooldowns.trueshot->adjust( -( p()->talents.calling_the_shots->effectN( 1 ).time_value() + p()->talents.unerring_vision->effectN( 3 ).time_value() ) );
    }

    p()->cooldowns.volley->adjust( -p()->talents.bullet_hell->effectN( 2 ).time_value() );
  }
};

struct aimed_shot_t : public aimed_shot_base_t
{
  struct aimed_shot_aspect_of_the_hydra_t : aimed_shot_base_t
  {
    aimed_shot_aspect_of_the_hydra_t( util::string_view n, hunter_t* p ) : aimed_shot_base_t( n, p, p->talents.aimed_shot )
    {
      background = dual = true;
      base_costs[ RESOURCE_FOCUS ] = 0;
      base_multiplier *= p->talents.aspect_of_the_hydra->effectN( 1 ).percent();
    }
  };

  struct aimed_shot_double_tap_t : aimed_shot_base_t
  {
    aimed_shot_double_tap_t( util::string_view n, hunter_t* p ) : aimed_shot_base_t( n, p, p->talents.aimed_shot )
    {
      background = dual = true;
      base_costs[ RESOURCE_FOCUS ] = 0;
      base_multiplier *= p->talents.double_tap->effectN( 3 ).percent();
    }
  };

  struct explosive_shot_tww_s2_mm_4pc_t : public explosive_shot_background_t
  {
    explosive_shot_tww_s2_mm_4pc_t( util::string_view n, hunter_t* p ) : explosive_shot_background_t( n, p ) {}

    double composite_persistent_multiplier( const action_state_t *state ) const override
    {
      double pm = explosive_shot_background_t::composite_persistent_multiplier( state );

      pm *= p()->tier_set.tww_s2_mm_4pc->effectN( 1 ).percent();

      return pm;
    }
  };

  struct {
    double chance = 0;
    proc_t* proc;
  } surging_shots;

  struct {
    double chance = 0; 
  } deathblow;

  aimed_shot_aspect_of_the_hydra_t* aspect_of_the_hydra = nullptr;
  aimed_shot_double_tap_t* double_tap = nullptr;
  explosive_shot_tww_s2_mm_4pc_t* tww_s2_mm_4pc = nullptr;
  bool lock_and_loaded = false;

  aimed_shot_t( hunter_t* p, util::string_view options_str ) : 
    aimed_shot_base_t( "aimed_shot", p, p->talents.aimed_shot )
  {
    parse_options( options_str );

    if ( p->talents.aspect_of_the_hydra.ok() )
    {
      aspect_of_the_hydra = p->get_background_action<aimed_shot_aspect_of_the_hydra_t>( "aimed_shot_aspect_of_the_hydra" );
      add_child( aspect_of_the_hydra );
    }

    if ( p->talents.double_tap.ok() )
    {
      double_tap = p->get_background_action<aimed_shot_double_tap_t>( "aimed_shot_double_tap" );
      add_child( double_tap );
    }

    if ( p -> talents.surging_shots.ok() )
    {
      surging_shots.chance = p -> talents.surging_shots -> proc_chance();
      surging_shots.proc = p -> get_proc( "Surging Shots Rapid Fire reset" );
    }

    if ( p->talents.deathblow.ok() )
      deathblow.chance = p->talents.improved_deathblow.ok() ? p->talents.improved_deathblow->effectN( 2 ).percent() : p->talents.deathblow->effectN( 1 ).percent();
  
    if ( p->tier_set.tww_s2_mm_4pc.ok() )
      tww_s2_mm_4pc = p->get_background_action<explosive_shot_tww_s2_mm_4pc_t>( "explosive_shot_tww_s2_mm_4pc" );
  }

  double cost() const override
  {
    const bool casting = p() -> executing && p() -> executing == this;
    if ( casting ? lock_and_loaded : p() -> buffs.lock_and_load -> check() )
      return 0;

    return aimed_shot_base_t::cost();
  }

  double cost_pct_multiplier() const override
  {
    double c = aimed_shot_base_t::cost_pct_multiplier();

    double streamline_mod = p()->buffs.streamline->check() * p()->talents.streamline_buff->effectN( 2 ).percent();

    if ( p()->buffs.trueshot->check() )
      streamline_mod *= 1 + p()->talents.tensile_bowstring->effectN( 2 ).percent();

    c *= 1 + streamline_mod;

    return c;
  }

  double execute_time_pct_multiplier() const override
  {
    if ( p() -> buffs.lock_and_load -> check() )
      return 0;

    auto et = aimed_shot_base_t::execute_time_pct_multiplier();

    double streamline_mod = p()->buffs.streamline->check_value();
    
    if ( p()->buffs.trueshot->check() )
      streamline_mod *= 1 + p()->talents.tensile_bowstring->effectN( 2 ).percent();

    et *= 1 + streamline_mod;

    return et;
  }
  
  void schedule_execute( action_state_t* s ) override
  {
    lock_and_loaded = p() -> buffs.lock_and_load -> up();

    aimed_shot_base_t::schedule_execute( s );
  }

  void execute() override
  {
    // The Explosive Shot hits before the Aimed Shot, so it must be cast beforehand.
    if ( lock_and_loaded && tww_s2_mm_4pc )
      tww_s2_mm_4pc->execute_on_target( target );

    aimed_shot_base_t::execute();

    // Lock and Load completely supresses consumption of Streamline
    if ( !p()->buffs.lock_and_load->check() )
      p()->buffs.streamline->expire();

    if ( rng().roll( surging_shots.chance ) )
    {
      surging_shots.proc -> occur();
      p() -> cooldowns.rapid_fire -> reset( true );
    }
    
    p()->buffs.trick_shots->up(); // Benefit tracking
    p()->consume_trick_shots();

    int precise_shot_stacks = 1;
    if ( rng().roll( p()->talents.windrunner_quiver->effectN( 6 ).percent() ) )
      precise_shot_stacks++;
    p()->buffs.precise_shots->increment( precise_shot_stacks );

    if ( rng().roll( deathblow.chance ) )
      p()->trigger_deathblow();

    auto tl = target_list();
    if ( aspect_of_the_hydra && tl.size() > 1 )
      aspect_of_the_hydra->execute_on_target( tl[ 1 ] );

    if ( double_tap && p()->buffs.double_tap->up() )
    {
      double_tap->execute_on_target( target );
      p()->buffs.double_tap->expire();
    }

    if ( lock_and_loaded )
    {
      p()->buffs.lock_and_load->decrement();
      p()->cooldowns.explosive_shot->adjust( p()->talents.magnetic_gunpowder->effectN( 2 ).time_value() );
    }
    lock_and_loaded = false;
  }

  double recharge_rate_multiplier( const cooldown_t& cd ) const override
  {
    double m = aimed_shot_base_t::recharge_rate_multiplier( cd );

    if ( p() -> buffs.trueshot -> check() )
      m /= 1 + p() -> talents.trueshot -> effectN( 3 ).percent();

    return m;
  }

  bool usable_moving() const override
  {
    return false;
  }
};

// Rapid Fire =========================================================================

struct rapid_fire_t: public hunter_spell_t
{
  struct rapid_fire_tick_t : public hunter_ranged_attack_t
  {
    const int trick_shots_targets;

    rapid_fire_tick_t( util::string_view n, hunter_t* p )
      : hunter_ranged_attack_t( n, p, p->talents.rapid_fire_tick ),
        trick_shots_targets( as<int>( p->talents.trick_shots_data->effectN( 3 ).base_value() ) )
    {
      background = dual = true;
      direct_tick = true;
      radius = 8;
      base_aoe_multiplier = p->talents.trick_shots_data->effectN( 5 ).percent();

      // energize
      parse_effect_data( p->talents.rapid_fire_energize->effectN( 1 ) );
    }

    int n_targets() const override
    {
      if ( p()->buffs.trick_shots->check() )
        return 1 + trick_shots_targets;

      return hunter_ranged_attack_t::n_targets();
    }

    void execute() override
    {
      hunter_ranged_attack_t::execute();

      p()->buffs.trick_shots->up(); // Benefit tracking
    }

    void impact( action_state_t* state ) override
    {
      hunter_ranged_attack_t::impact( state );

      p()->buffs.bulletstorm->trigger();
    }
  };

  struct rapid_fire_tick_aspect_of_the_hydra_t : public rapid_fire_tick_t
  {
    rapid_fire_tick_aspect_of_the_hydra_t( util::string_view n, hunter_t* p ) : rapid_fire_tick_t( n, p )
    {
      base_multiplier *= p->talents.aspect_of_the_hydra->effectN( 1 ).percent();
    }
  };

  rapid_fire_tick_t* damage;
  rapid_fire_tick_aspect_of_the_hydra_t* aspect_of_the_hydra = nullptr;
  int base_num_ticks;

  struct {
    double chance = 0; 
  } deathblow;

  rapid_fire_t( hunter_t* p, util::string_view options_str ):
    hunter_spell_t( "rapid_fire", p, p -> talents.rapid_fire ),
    damage( p -> get_background_action<rapid_fire_tick_t>( "rapid_fire_tick" ) ),
    base_num_ticks( as<int>( data().effectN( 1 ).base_value() ) )
  {
    parse_options( options_str );

    may_miss = may_crit = false;
    channeled = reset_auto_attack = true;

    base_num_ticks += p -> talents.ammo_conservation.ok() ? as<int>( p -> talents.ammo_conservation -> effectN( 2 ).base_value() ) : 0;

    if ( p->talents.improved_deathblow.ok() )
      deathblow.chance = p->talents.improved_deathblow->effectN( 1 ).percent();

    if ( p->talents.aspect_of_the_hydra.ok() )
    {
      aspect_of_the_hydra = p->get_background_action<rapid_fire_tick_aspect_of_the_hydra_t>( "rapid_fire_tick_aspect_of_the_hydra" );
      add_child( aspect_of_the_hydra );
    }
  }

  void init() override
  {
    hunter_spell_t::init();

    damage -> gain = gain;
    damage -> stats = stats;
    stats -> action_list.push_back( damage );
  }

  void execute() override
  {
    hunter_spell_t::execute();

    p()->buffs.streamline->trigger();

    if ( rng().roll( deathblow.chance ) )
      p()->trigger_deathblow();

    if ( p()->talents.no_scope.ok() )
      p()->buffs.precise_shots->trigger();

    p()->buffs.bulletstorm->expire();

    if ( p()->buffs.lunar_storm_ready->up() )
      p()->trigger_lunar_storm( target );
  }

  void tick( dot_t* d ) override
  {
    hunter_spell_t::tick( d );

    damage -> execute_on_target( d->target );

    auto tl = target_list();
    if ( aspect_of_the_hydra && tl.size() > 1 )
      aspect_of_the_hydra->execute_on_target( tl[ 1 ] );
  }

  void last_tick( dot_t* d ) override
  {
    hunter_spell_t::last_tick( d );

    p()->consume_trick_shots();
    p()->buffs.in_the_rhythm->trigger();
    p()->buffs.double_tap->expire();
  }

  timespan_t composite_dot_duration( const action_state_t* s ) const override
  {
    // substract 1 here because RF has a tick at zero
    double num_ticks = base_num_ticks - 1;

    if ( p()->buffs.double_tap->check() )
      num_ticks *= 1 + p()->talents.double_tap_buff->effectN( 3 ).percent();

    timespan_t base_duration = num_ticks * tick_time( s );
    
    return base_duration; 
  }

  double tick_time_pct_multiplier( const action_state_t* s ) const override
  {
    double m = hunter_spell_t::tick_time_pct_multiplier( s );

    m *= 1 + p()->buffs.double_tap->check_value();

    return m;
  }

  double energize_cast_regen( const action_state_t* ) const override
  {
    return base_num_ticks * damage -> composite_energize_amount( nullptr );
  }

  double recharge_rate_multiplier( const cooldown_t& cd ) const override
  {
    double m = hunter_spell_t::recharge_rate_multiplier( cd );

    if ( p() -> buffs.trueshot -> check() )
      m /= 1 + p() -> talents.trueshot -> effectN( 1 ).percent();

    return m;
  }
};

//==============================
// Survival attacks
//==============================

// Melee attack ==============================================================

struct melee_t : public auto_attack_base_t<melee_attack_t>
{
  melee_t( hunter_t* player ) :
    auto_attack_base_t( "auto_attack_mh", player )
  {
    school             = SCHOOL_PHYSICAL;
    weapon_multiplier  = 1;
    may_glance         = true;
    may_crit           = true;
  }

  void impact( action_state_t* s ) override
  {
    auto_attack_base_t::impact( s );

    p()->cooldowns.wildfire_bomb->adjust( -p()->talents.lunge->effectN( 2 ).time_value() );
    p()->buffs.wildfire_arsenal->trigger();
  }

  double action_multiplier() const override
  {
    double am = auto_attack_base_t::action_multiplier();

    double bonus = p()->cache.mastery() * p()->mastery.spirit_bond -> effectN( 5 ).mastery_value();
    bonus *= 1 + p()->mastery.spirit_bond_buff->effectN( 1 ).percent();
      
    am *= 1 + bonus;

    return am;
  }
};

// Harpoon ==================================================================

struct harpoon_t : public hunter_melee_attack_t
{
  struct terms_of_engagement_t final : hunter_ranged_attack_t
  {
    terms_of_engagement_t( util::string_view n, hunter_t* p ) : hunter_ranged_attack_t( n, p, p->talents.terms_of_engagement_dmg )
    {
      background = dual = true;
    }

    void impact( action_state_t* s ) override
    {
      hunter_ranged_attack_t::impact( s );

      p()->buffs.terms_of_engagement->trigger();
    }
  };
  terms_of_engagement_t* terms_of_engagement = nullptr;

  harpoon_t( hunter_t* p, util::string_view options_str )
    : hunter_melee_attack_t( "harpoon", p, p->specs.harpoon )
  {
    parse_options( options_str );

    harmful = false;
    base_teleport_distance = data().max_range();
    movement_directionality = movement_direction_type::OMNI;

    if ( p->talents.terms_of_engagement.ok() )
    {
      terms_of_engagement = p->get_background_action<terms_of_engagement_t>( "harpoon_terms_of_engagement" );
      add_child( terms_of_engagement );
    }

    if ( p->main_hand_weapon.group() != WEAPON_2H )
      background = true;
  }

  void execute() override
  {
    hunter_melee_attack_t::execute();

    if ( terms_of_engagement )
      terms_of_engagement->execute_on_target( target );
  }

  bool ready() override
  {
    // XXX: disable this for now to actually make it usable without explicit apl support for movement
    // if ( p() -> current.distance_to_move < data().min_range() )
    //  return false;

    return hunter_melee_attack_t::ready();
  }
};

// Raptor Strike/Mongoose Bite ================================================================

struct melee_focus_spender_t: hunter_melee_attack_t
{
  struct serpent_sting_vipers_venom_t final : public serpent_sting_t
  {
    serpent_sting_vipers_venom_t( util::string_view n, hunter_t* p ) : serpent_sting_t( n, p )
    {
    }

    int n_targets() const override
    {
      if ( p()->talents.contagious_reagents.ok() && p()->get_target_data( target )->dots.serpent_sting->is_ticking() )
        return 1 + as<int>( p()->talents.contagious_reagents->effectN( 1 ).base_value() );

      return serpent_sting_t::n_targets();
    }

    size_t available_targets( std::vector<player_t*>& tl ) const override
    {
      serpent_sting_t::available_targets( tl );

      if ( is_aoe() && tl.size() > 1 )
      {
        // Prefer targets without Serpent Sting ticking.
        auto start = tl.begin();
        std::partition( *start == target ? std::next( start ) : start, tl.end(),
                        [ this ]( player_t* t ) { return !( this->td( t )->dots.serpent_sting->is_ticking() ); } );
      }

      return tl.size();
    }
  };

  serpent_sting_vipers_venom_t* vipers_venom = nullptr;
  
  struct {
    double chance = 0;
    proc_t* proc;
  } rylakstalkers_strikes;

  double wildfire_infusion_chance = 0;

  melee_focus_spender_t( util::string_view n, hunter_t* p, const spell_data_t* s ):
    hunter_melee_attack_t( n, p, s )
  {
    if ( p -> talents.vipers_venom.ok() )
      vipers_venom = p->get_background_action<serpent_sting_vipers_venom_t>( "serpent_sting_vipers_venom" );

    wildfire_infusion_chance = p->talents.wildfire_infusion->effectN( 1 ).percent();
  }

  int n_targets() const override
  {
    int n = hunter_melee_attack_t::n_targets();

    n += p()->buffs.hogstrider->check();

    return n;
  }

  double action_multiplier() const override
  {
    double am = hunter_melee_attack_t::action_multiplier();

    am *= 1 + p() -> buffs.mongoose_fury -> stack_value();
        
    if ( p()->buffs.strike_it_rich->check() )
      am *= 1 + p() -> buffs.strike_it_rich -> value();  

    return am;
  }

  double composite_target_da_multiplier( player_t* t ) const override
  {
    double m = hunter_melee_attack_t::composite_target_da_multiplier( t );

    hunter_td_t *td = p()->get_target_data( t );

    if ( p()->tier_set.tww_s1_sv_4pc.ok() && td->dots.wildfire_bomb->is_ticking() )
      m *= 1 + p()->tier_set.tww_s1_sv_4pc->effectN( 1 ).percent();

    return m;
  }

  void execute() override
  {
    hunter_melee_attack_t::execute();

    if ( rng().roll( rylakstalkers_strikes.chance ) )
    {
      p() -> cooldowns.wildfire_bomb -> reset( true );
      rylakstalkers_strikes.proc -> occur();
    }

    if ( rng().roll( wildfire_infusion_chance ) )
      p()->cooldowns.kill_command->reset( true );

    p()->buffs.howl_of_the_pack_leader_cooldown->extend_duration( p(), -p()->talents.dire_summons->effectN( 4 ).time_value() );

    p()->buffs.hogstrider->expire();

    if ( p()->tier_set.tww_s2_sv_4pc.ok() && p()->buffs.strike_it_rich->check() )
    {
      p()->buffs.strike_it_rich->expire();
      p()->cooldowns.wildfire_bomb->adjust( -p()->buffs.strike_it_rich->data().effectN( 2 ).time_value() );
    }
  }

  void impact( action_state_t* s ) override
  {
    hunter_melee_attack_t::impact( s );

    if ( vipers_venom )
      vipers_venom->execute_on_target( s->target );
  }

  bool ready() override
  {
    const bool has_eagle = p() -> buffs.aspect_of_the_eagle -> check();
    return ( range > 10 ? has_eagle : !has_eagle ) && hunter_melee_attack_t::ready();
  }
};

// Raptor Strike =====================================================================

struct raptor_strike_base_t : public melee_focus_spender_t
{
  raptor_strike_base_t( util::string_view n, hunter_t* p, spell_data_ptr_t s ) : melee_focus_spender_t( n, p, s )
  {
  }
};

struct raptor_strike_t : public raptor_strike_base_t
{
  raptor_strike_t( hunter_t* p, util::string_view options_str )
    : raptor_strike_base_t( "raptor_strike", p, p->talents.raptor_strike )
  {
    parse_options( options_str );
  }
};

struct raptor_strike_eagle_t : public raptor_strike_base_t
{
  raptor_strike_eagle_t( hunter_t* p, util::string_view options_str )
    : raptor_strike_base_t( "raptor_strike_eagle", p, p->talents.raptor_strike_eagle )
  {
    parse_options( options_str );
  }
};

// Mongoose Bite =======================================================================

struct mongoose_bite_base_t: melee_focus_spender_t
{
  struct {
    std::array<proc_t*, 7> at_fury;
  } stats_;

  mongoose_bite_base_t( util::string_view n, hunter_t* p, spell_data_ptr_t s ):
    melee_focus_spender_t( n, p, s )
  {
    for ( size_t i = 0; i < stats_.at_fury.size(); i++ )
      stats_.at_fury[ i ] = p -> get_proc( fmt::format( "bite_at_{}_fury", i ) );

    if ( !p->talents.mongoose_bite.ok() )
      background = true;
  }

  void execute() override
  {
    melee_focus_spender_t::execute();

    stats_.at_fury[ p() -> buffs.mongoose_fury -> check() ] -> occur();

    p()->buffs.mongoose_fury->trigger();
  }
};

struct mongoose_bite_t : mongoose_bite_base_t
{
  mongoose_bite_t( hunter_t* p, util::string_view options_str ):
    mongoose_bite_base_t( "mongoose_bite", p, p -> talents.mongoose_bite )
  {
    parse_options( options_str );
  }
};

struct mongoose_bite_eagle_t : mongoose_bite_base_t
{
  mongoose_bite_eagle_t( hunter_t* p, util::string_view options_str ):
    mongoose_bite_base_t( "mongoose_bite_eagle", p, p -> talents.mongoose_bite_eagle )
  {
    parse_options( options_str );
  }
};

// Flanking Strike =====================================================================

struct flanking_strike_t: hunter_melee_attack_t
{
  struct damage_t final : hunter_melee_attack_t
  {
    struct merciless_blow_t : public hunter_melee_attack_t
    {
      merciless_blow_t( util::string_view n, hunter_t* p ) : hunter_melee_attack_t( n, p, p->talents.merciless_blow_flanking_bleed )
      {
        background = dual = true;
      }

      dot_t* get_dot( player_t* t )
      {
        if ( !t )
          t = target;
        if ( !t )
          return nullptr;

        return td( t )->dots.merciless_blow;
      }
    };

    merciless_blow_t* merciless_blow = nullptr;

    damage_t( util::string_view n, hunter_t* p ) : hunter_melee_attack_t( n, p, p->talents.flanking_strike_player )
    {
      background = dual = true;

      // Decrement after player and pet damage.
      decrements_tip_of_the_spear = false;

      if ( p->talents.merciless_blow.ok() )
        merciless_blow = p->get_background_action<merciless_blow_t>( "flanking_strike_merciless_blow" );
    }

    void execute() override
    {
      hunter_melee_attack_t::execute();

      if ( merciless_blow )
        merciless_blow->execute_on_target( target );
    }
  };

  damage_t* damage;

  flanking_strike_t( hunter_t* p, util::string_view options_str ):
    hunter_melee_attack_t( "flanking_strike", p, p -> talents.flanking_strike ),
    damage( p -> get_background_action<damage_t>( "flanking_strike_player" ) )
  {
    parse_options( options_str );

    base_teleport_distance  = data().max_range();
    movement_directionality = movement_direction_type::OMNI;

    add_child( damage );
    if ( damage->merciless_blow )
      add_child( damage->merciless_blow );

    // Decrement after player and pet damage.
    decrements_tip_of_the_spear = false;
  }

  void init_finished() override
  {
    for ( auto pet : p() -> pet_list )
      add_pet_stats( pet, { "flanking_strike" } );

    hunter_melee_attack_t::init_finished();
  }

  void execute() override
  {
    hunter_melee_attack_t::execute();

    if ( p() -> main_hand_weapon.group() == WEAPON_2H )
      damage -> execute_on_target( target );

    if ( auto pet = p() -> pets.main )
      pet->actions.flanking_strike->execute_on_target( target );

    p()->buffs.tip_of_the_spear->decrement();
    p()->buffs.tip_of_the_spear->trigger( as<int>( p()->talents.flanking_strike->effectN( 2 ).base_value() ) );
    p()->buffs.frenzy_strikes->trigger();
  }
};

// Butchery ==========================================================================

struct butchery_t : public hunter_melee_attack_t
{
  struct merciless_blow_t : public hunter_melee_attack_t
  {
    merciless_blow_t( util::string_view n, hunter_t* p ) : hunter_melee_attack_t( n, p, p->talents.merciless_blow_butchery_bleed )
    {
      background = dual = true;
    }

    dot_t* get_dot( player_t* t )
    {
      if ( !t )
        t = target;
      if ( !t )
        return nullptr;

      return td( t )->dots.merciless_blow;
    }
  };

  struct
  {
    timespan_t reduction = 0_s;
    int cap = 0;
  } frenzy_strikes;

  merciless_blow_t* merciless_blow = nullptr;

  butchery_t( hunter_t* p, util::string_view options_str ):
    hunter_melee_attack_t( "butchery", p, p -> talents.butchery )
  {
    parse_options( options_str );

    aoe = -1;
    reduced_aoe_targets = data().effectN( 3 ).base_value();

    if ( p->talents.frenzy_strikes.ok() )
    {
      frenzy_strikes.reduction = data().effectN( 2 ).time_value();
      frenzy_strikes.cap = as<int>( data().effectN( 3 ).base_value() );
    }

    if ( p->talents.merciless_blow.ok() )
    {
      merciless_blow = p->get_background_action<merciless_blow_t>( "butchery_merciless_blow" );
      add_child( merciless_blow );
    }
  }

  void execute() override
  {
    hunter_melee_attack_t::execute();

    p()->trigger_symphonic_arsenal();

    if ( p()->talents.frenzy_strikes.ok() )
      p()->cooldowns.wildfire_bomb->adjust( -frenzy_strikes.reduction * std::min( num_targets_hit, frenzy_strikes.cap ) );
  }

  void impact( action_state_t* s ) override
  {
    hunter_melee_attack_t::impact( s );

    if ( merciless_blow && s->chain_target < p()->talents.merciless_blow->effectN( 1 ).base_value() )
      merciless_blow->execute_on_target( s->target );
  }
};

// Fury of the Eagle ==============================================================

struct fury_of_the_eagle_t : public hunter_melee_attack_t
{
  bool procced_at_max_tip = false; 

  struct fury_of_the_eagle_tick_t: public hunter_melee_attack_t
  {
    struct
    {
      double bonus = 0;
      double threshold = 0;
    } crit;

    fury_of_the_eagle_tick_t( util::string_view n, hunter_t* p ) : hunter_melee_attack_t( n, p, p->talents.fury_of_the_eagle->effectN( 1 ).trigger() )
    {
      aoe = -1;
      background = true;
      may_crit = true;
      radius = data().max_range();
      reduced_aoe_targets = p->talents.fury_of_the_eagle->effectN( 5 ).base_value();

      crit.threshold = p -> talents.fury_of_the_eagle -> effectN( 4 ).base_value();
      crit.bonus = p -> talents.fury_of_the_eagle -> effectN( 3 ).percent();

      // Fury of the Eagle ticks do not decrement Tip of the Spear stacks.
      decrements_tip_of_the_spear = false;
    }

    double composite_target_crit_chance( player_t* target ) const override
    {
      double c = hunter_melee_attack_t::composite_target_crit_chance( target );

      if ( target -> health_percentage() < crit.threshold )
        c += crit.bonus;

      return c;
    }

    double composite_da_multiplier( const action_state_t* s ) const override
    {
      double m = hunter_melee_attack_t::composite_da_multiplier( s );
    
      // TODO 13/2/25: Casting Fury of the Eagle with more than one stack of Tip of the Spear 
      // will result in a double application of the bonus damage, from both the generic buff
      // and the new FotE specific hidden buff.
      if ( p()->buffs.tip_of_the_spear_fote->check() )
        m *= 1.0 + p()->calculate_tip_of_the_spear_value( p()->talents.tip_of_the_spear->effectN( 1 ).percent() );

      return m;
    }
  };

  fury_of_the_eagle_tick_t* fote_tick;

  fury_of_the_eagle_t( hunter_t* p, util::string_view options_str ):
    hunter_melee_attack_t( "fury_of_the_eagle", p, p -> talents.fury_of_the_eagle ),
      fote_tick( p->get_background_action<fury_of_the_eagle_tick_t>( "fury_of_the_eagle_damage" ) )
  {
    parse_options( options_str );

    channeled = true;
    tick_zero = true;

    add_child( fote_tick );

    decrements_tip_of_the_spear = false;
  }

  void execute() override
  {
    hunter_melee_attack_t::execute();

    if ( p()->buffs.tip_of_the_spear->up() )
    {
      p()->buffs.tip_of_the_spear->decrement();
      p()->buffs.tip_of_the_spear_fote->trigger();
    }
  }

  void tick( dot_t* dot ) override
  {
    hunter_melee_attack_t::tick( dot );

    fote_tick -> execute_on_target( dot -> target );

    if ( p()->talents.ruthless_marauder.ok() && p()->cooldowns.ruthless_marauder->up() && rng().roll( p()->talents.ruthless_marauder->effectN( 1 ).percent() ) )
    {      
      p()->buffs.tip_of_the_spear->trigger();
      p()->cooldowns.ruthless_marauder->start();
    }
  }

  void last_tick( dot_t* dot ) override
  {
    hunter_melee_attack_t::last_tick( dot );

    p()->buffs.tip_of_the_spear_fote->decrement();
    p()->buffs.ruthless_marauder->trigger();
  }
};

// Coordinated Assault ==============================================================

struct coordinated_assault_t: public hunter_melee_attack_t
{
  struct damage_t final : hunter_melee_attack_t
  {
    damage_t( util::string_view n, hunter_t* p ):
      hunter_melee_attack_t( n, p, p->talents.coordinated_assault_dmg )
    {
      background = dual = true;

      // Despite damage being affected, does not consume.
      decrements_tip_of_the_spear = false;
    }
  };
  damage_t* damage;

  coordinated_assault_t( hunter_t* p, util::string_view options_str ):
    hunter_melee_attack_t( "coordinated_assault", p, p->talents.coordinated_assault )
  {
    parse_options( options_str );

    base_teleport_distance = data().max_range();
    movement_directionality = movement_direction_type::OMNI;
    gcd_type = gcd_haste_type::ATTACK_HASTE;

    damage = p->get_background_action<damage_t>( "coordinated_assault_player" );
    add_child( damage );

    decrements_tip_of_the_spear = false;
  }

  void init_finished() override
  {
    for ( auto pet : p()->pet_list )
      add_pet_stats( pet, { "coordinated_assault" } );

    hunter_melee_attack_t::init_finished();
  }

  void execute() override
  {
    // Can be affected by its own generated stacks.
    if ( p()->talents.symbiotic_adrenaline.ok() )
      p()->buffs.tip_of_the_spear->trigger( as<int>( p()->talents.symbiotic_adrenaline->effectN( 2 ).base_value() ) );

    hunter_melee_attack_t::execute();

    p()->state.sentinel_watch_reduction = 0_s;
    p()->buffs.eyes_closed->trigger();

    if ( p() -> main_hand_weapon.group() == WEAPON_2H )
      damage -> execute_on_target( target );

    p()->buffs.coordinated_assault->trigger();
    p()->buffs.relentless_primal_ferocity->trigger();

    if ( auto pet = p()->pets.main )
      pet->actions.coordinated_assault->execute_on_target( target );
    
    if ( p()->talents.bombardier.ok() )
      p()->cooldowns.wildfire_bomb->reset( false, as<int>( p()->talents.bombardier->effectN( 1 ).base_value() ) );

    p()->buffs.wildfire_arsenal->trigger( p()->buffs.wildfire_arsenal->max_stack() );

    if ( p()->talents.lead_from_the_front->ok() )
    {
      p()->buffs.lead_from_the_front->trigger();
      p()->trigger_howl_of_the_pack_leader();
    }
  }
};

} // end namespace attacks

// ==========================================================================
// Hunter Spells
// ==========================================================================

namespace spells
{

//==============================
// Shared spells
//==============================

// Base Interrupt ===========================================================

struct interrupt_base_t: public hunter_spell_t
{
  interrupt_base_t( util::string_view n, hunter_t* p, const spell_data_t* s ):
    hunter_spell_t( n, p, s )
  {
    may_miss = may_block = may_dodge = may_parry = false;
    is_interrupt = true;
  }

  bool target_ready( player_t* candidate_target ) override
  {
    if ( !candidate_target -> debuffs.casting || !candidate_target -> debuffs.casting -> check() ) return false;
    return hunter_spell_t::target_ready( candidate_target );
  }
};

// Summon Pet ===============================================================

struct summon_pet_t: public hunter_spell_t
{
  bool opt_disabled;
  pet_t* pet;

  summon_pet_t( hunter_t* p, util::string_view options_str ):
    hunter_spell_t( "summon_pet", p, p->specs.call_pet ),
    opt_disabled( false ), pet( nullptr )
  {
    parse_options( options_str );

    harmful = false;
    callbacks = false;
    ignore_false_positive = true;

    opt_disabled = util::str_compare_ci( p -> options.summon_pet_str, "disabled" );

    target = player;
  }

  void init_finished() override
  {
    if ( !pet && !opt_disabled )
      pet = player -> find_pet( p() -> options.summon_pet_str );

    if ( !pet && ( p() -> specialization() != HUNTER_MARKSMANSHIP || p()->talents.unbreakable_bond.ok() ) )
    {
      throw std::invalid_argument(fmt::format("Unable to find pet '{}' for summons.", p() -> options.summon_pet_str));
    }

    hunter_spell_t::init_finished();
  }

  void execute() override
  {
    hunter_spell_t::execute();

    pet -> type = PLAYER_PET;
    pet -> summon();

    if ( p() -> main_hand_attack ) p() -> main_hand_attack -> cancel();
  }

  bool ready() override
  {
    if ( opt_disabled || p() -> pets.main == pet || p()->specialization() == HUNTER_MARKSMANSHIP && !p()->talents.unbreakable_bond.ok() )
      return false;

    return hunter_spell_t::ready();
  }
};

// Base Trap =========================================================================

struct trap_base_t : hunter_spell_t
{
  timespan_t precast_time;

  trap_base_t( util::string_view n, hunter_t* p, spell_data_ptr_t s ) :
    hunter_spell_t( n, p, s )
  {
    add_option( opt_timespan( "precast_time", precast_time ) );

    harmful = may_miss = false;
  }

  void init() override
  {
    hunter_spell_t::init();

    precast_time = clamp( precast_time, 0_ms, cooldown -> duration );
  }

  void execute() override
  {
    hunter_spell_t::execute();

    adjust_precast_cooldown( precast_time );
  }

  timespan_t travel_time() const override
  {
    timespan_t time_to_travel = hunter_spell_t::travel_time();
    if ( is_precombat )
      return std::max( 0_ms, time_to_travel - precast_time );
    return time_to_travel;
  }
};

// Freezing Trap ============================================================================

struct freezing_trap_t : public trap_base_t
{
  freezing_trap_t( hunter_t* p, util::string_view options_str )
    : trap_base_t( "freezing_trap", p, p->specs.freezing_trap )
  {
    parse_options( options_str );
  }
};

// Tar Trap (Hunter Talent) ==============================================================

struct tar_trap_t : public trap_base_t
{
  timespan_t debuff_duration;

  tar_trap_t( hunter_t* p, util::string_view options_str ) :
    trap_base_t( "tar_trap", p, p -> talents.tar_trap )
  {
    parse_options( options_str );

    debuff_duration = p -> find_spell( 13810 ) -> duration();
  }

  void impact( action_state_t* s ) override
  {
    trap_base_t::impact( s );

    p() -> state.tar_trap_aoe = make_event<events::tar_trap_aoe_t>( *p() -> sim, p(), s -> target, debuff_duration );
  }
};

// High Explosive/Implosive Trap (Hunter Talent) ======================================================

struct explosive_trap_damage_t final : hunter_ranged_attack_t
{
  explosive_trap_damage_t( util::string_view n, hunter_t* p ) : hunter_ranged_attack_t( n, p, p->talents.explosive_trap_damage )
  {
    aoe = -1;
    attack_power_mod.direct = data().effectN( 2 ).ap_coeff();
  }
};

struct high_explosive_trap_t : public trap_base_t
{
  high_explosive_trap_t( hunter_t* p, util::string_view options_str )
    : trap_base_t( "high_explosive_trap", p, p -> talents.high_explosive_trap )
  {
    parse_options( options_str );

    impact_action = p->get_background_action<explosive_trap_damage_t>( "high_explosive_trap_damage" );
  }
};

struct implosive_trap_t : public trap_base_t
{
  implosive_trap_t( hunter_t* p, util::string_view options_str )
    : trap_base_t( "implosive_trap", p, p->talents.implosive_trap )
  {
    parse_options( options_str );

    impact_action = p->get_background_action<explosive_trap_damage_t>( "implosive_trap_damage" );
  }
};

// Counter Shot (Marksmanship/Beast Mastery Talent) ===========================================================

struct counter_shot_t: public interrupt_base_t
{
  counter_shot_t( hunter_t* p, util::string_view options_str ):
    interrupt_base_t( "counter_shot", p, p -> talents.counter_shot )
  {
    parse_options( options_str );
  }
};

// Kill Command (Beast Mastery/Survival Talent) =============================================================

struct kill_command_t: public hunter_spell_t
{
  struct arcane_shot_quick_shot_t final : public attacks::arcane_shot_base_t
  {
    arcane_shot_quick_shot_t( util::string_view n, hunter_t* p ) : arcane_shot_base_t( n, p )
    {
      background = dual = true;
      base_costs[ RESOURCE_FOCUS ] = 0;
      base_dd_multiplier *= p->talents.quick_shot->effectN( 2 ).percent();

      // Don't consume a stack or buff the damage.
      affected_by.tip_of_the_spear.direct = 0;
      decrements_tip_of_the_spear = false;
    }

    void execute() override
    {
      arcane_shot_base_t::execute();

      p()->buffs.wildfire_arsenal->trigger();
    }
  };

  struct explosive_shot_sulfurlined_pockets_t : public attacks::explosive_shot_background_t
  {
    explosive_shot_sulfurlined_pockets_t( util::string_view n, hunter_t* p ) : explosive_shot_background_t( n, p ) {}

    void init() override
    {
      explosive_shot_background_t::init();

      snapshot_flags = STATE_MUL_PERSISTENT;
    }

    double composite_persistent_multiplier( const action_state_t *state ) const override
    {
      double pm = explosive_shot_background_t::composite_persistent_multiplier( state );

      pm *= p()->talents.sulfurlined_pockets->effectN( 2 ).percent();

      return pm;
    }
  };

  struct {
    double chance = 0;
    proc_t* proc = nullptr;
  } reset;

  struct {
    double chance = 0;
    arcane_shot_quick_shot_t* arcane_shot = nullptr;
    explosive_shot_sulfurlined_pockets_t* explosive_shot = nullptr;
  } quick_shot;

  struct {
    double chance = 0;
    proc_t* proc;
  } dire_command;

  struct {
    double chance = 0; 
  } deathblow;

  struct
  {
    timespan_t extension = 0_s;
    timespan_t cap = 0_s;
  } fury_of_the_wyvern;

  timespan_t wildfire_infusion_reduction = 0_s;
  timespan_t bloody_claws_extension = 0_s;

  kill_command_t( hunter_t* p, util::string_view options_str ):
    hunter_spell_t( "kill_command", p, p -> talents.kill_command )
  {
    parse_options( options_str );

    cooldown -> charges += as<int>( p -> talents.alpha_predator -> effectN( 1 ).base_value() );

    if ( p -> specialization() == HUNTER_SURVIVAL )
    {
      reset.chance = data().effectN( 2 ).percent() + p -> talents.flankers_advantage -> effectN( 1 ).percent();
      reset.proc = p -> get_proc( "Kill Command Reset" );

      if ( p -> talents.quick_shot.ok() )
      {
        quick_shot.chance = p -> talents.quick_shot -> effectN( 1 ).percent();
        quick_shot.arcane_shot = p->get_background_action<arcane_shot_quick_shot_t>( "arcane_shot_quick_shot" );
        add_child( quick_shot.arcane_shot );

        if ( p->talents.sulfurlined_pockets.ok() )
          quick_shot.explosive_shot = p->get_background_action<explosive_shot_sulfurlined_pockets_t>( "explosive_shot_sulfurlined_pockets" );
      }

      wildfire_infusion_reduction = p->talents.wildfire_infusion->effectN( 2 ).time_value();
      bloody_claws_extension = p->talents.bloody_claws->effectN( 2 ).time_value();

      if ( p->talents.deathblow.ok() )
        deathblow.chance = p->talents.deathblow->effectN( 3 ).percent() 
          + p->talents.sic_em->effectN( 1 ).percent()
          + p->talents.born_to_kill->effectN( 1 ).percent();
    }
    
    if ( p->specialization() == HUNTER_BEAST_MASTERY )
    {
      if ( p->talents.deathblow.ok() )
        deathblow.chance = p->talents.deathblow->effectN( 2 ).percent();
      
      if ( p->talents.fury_of_the_wyvern.ok() )
      {
        fury_of_the_wyvern.extension = p->talents.fury_of_the_wyvern->effectN( 2 ).time_value();
        fury_of_the_wyvern.cap = timespan_t::from_seconds( p->talents.fury_of_the_wyvern->effectN( 4 ).base_value() );
      }
    }

    if ( p -> talents.dire_command.ok() )
    {
      dire_command.chance = p -> talents.dire_command -> effectN( 1 ).percent();
    }
  }

  void init_finished() override
  {
    for ( auto pet : p() -> pet_list )
      add_pet_stats( pet, { "kill_command" } );

    hunter_spell_t::init_finished();
  }

  void execute() override
  {
    hunter_spell_t::execute();

    for ( auto pet : pets::active<pets::hunter_main_pet_base_t>( p() -> pets.main, p() -> pets.animal_companion ) )
      pet -> actions.kill_command -> execute_on_target( target );

    int tip_stacks = 1;
    
    if ( p()->consume_howl_of_the_pack_leader( target ) )
      tip_stacks++;

    if ( p()->buffs.relentless_primal_ferocity->check() )
      tip_stacks += as<int>( p()->talents.relentless_primal_ferocity_buff->effectN( 4 ).base_value() );

    p()->buffs.tip_of_the_spear->trigger( tip_stacks );

    if ( rng().roll( quick_shot.chance ) )
    {
      if ( p()->buffs.sulfurlined_pockets_ready->up() )
      {
        p()->buffs.sulfurlined_pockets_ready->expire();
        quick_shot.explosive_shot->execute_on_target( target );
      }
      else
        quick_shot.arcane_shot->execute_on_target( target );
    }

    if ( reset.chance != 0 )
    {
      double chance = reset.chance;

      chance += p()->talents.bloody_claws->effectN( 1 ).percent() * p()->buffs.mongoose_fury->check();
      chance += p()->buffs.coordinated_assault->check_value();

      if ( rng().roll( chance ) )
      {
        reset.proc -> occur();
        cooldown -> reset( true );
      }
    }

    if ( rng().roll( dire_command.chance ) )
    {
      p() -> actions.dire_command -> execute();
      p() -> procs.dire_command -> occur();
    }

    if ( p()->talents.deathblow.ok() )
    {
      double chance = deathblow.chance;
      // Sic 'Em doubles the chance of Deathblow during Coordinated Assault, but it is not in spell data.
      if ( p()->talents.sic_em.ok() && p()->buffs.coordinated_assault->check() )
        chance *= 2;

      if ( rng().roll( chance ) )
        p()->trigger_deathblow();
    }

    if ( p() -> talents.a_murder_of_crows.ok() )
    {
      p() -> buffs.a_murder_of_crows -> trigger();
      if ( p() -> buffs.a_murder_of_crows -> at_max_stacks() )
      {
        p() -> actions.a_murder_of_crows -> execute_on_target( target );
        p() -> buffs.a_murder_of_crows -> expire();
      }
    }

    p()->cooldowns.wildfire_bomb->adjust( -wildfire_infusion_reduction );
    p()->buffs.mongoose_fury->extend_duration( p(), bloody_claws_extension );

    p()->buffs.howl_of_the_pack_leader_cooldown->extend_duration( p(), -p()->talents.dire_summons->effectN( p()->specialization() == HUNTER_BEAST_MASTERY ? 1 : 2 ).time_value() );
    
    if ( p()->state.fury_of_the_wyvern_extension < fury_of_the_wyvern.cap )
    {
      p()->buffs.wyverns_cry->extend_duration( p(), fury_of_the_wyvern.extension );
      p()->state.fury_of_the_wyvern_extension += fury_of_the_wyvern.extension;
    }
  }

  bool target_ready( player_t* candidate_target ) override
  {
    if ( p()->pets.main &&
         p()->pets.main->hunter_main_pet_base_t::actions.kill_command->target_ready( candidate_target ) )
      return hunter_spell_t::target_ready( candidate_target );

    return false;
  }

  bool ready() override
  {
    if ( p()->pets.main &&
         p()->pets.main->hunter_main_pet_base_t::actions.kill_command->ready() ) // Range check from the pet.
    {
        return hunter_spell_t::ready();
    }

    return false;
  }

  std::unique_ptr<expr_t> create_expression(util::string_view expression_str) override
  {
    // this is somewhat unfortunate but we can't get at the pets dot in any other way
    auto splits = util::string_split<util::string_view>( expression_str, "." );
    if ( splits.size() == 2 && splits[ 0 ] == "bloodseeker" && splits[ 1 ] == "remains" )
    {
      if ( !p() -> talents.bloodseeker.ok() )
        return expr_t::create_constant( expression_str, 0_ms );

      return make_fn_expr( expression_str, [ this ] () {
          if ( auto pet = p() -> pets.main )
            return pet -> get_target_data( target ) -> dots.bloodseeker -> remains();
          return 0_ms;
        } );
    }

    if ( expression_str == "damage" )
    {
      auto pet = p()->find_pet( p()->options.summon_pet_str );
      if ( pet )
      {
        auto kc = pet->find_action( "kill_command" );
        if ( kc )
          return std::make_unique<pet_amount_expr_t>( expression_str, *this, *kc );
      }
    }

    return hunter_spell_t::create_expression( expression_str );
  }
};

//==============================
// Beast Mastery spells
//==============================

// Dire Beast ===============================================================

struct dire_beast_t: public hunter_spell_t
{
  struct
  {
    timespan_t duration = 0_s;
  } shadow_hounds;

  dire_beast_t( hunter_t* p, util::string_view options_str ):
    hunter_spell_t( "dire_beast", p, p -> talents.dire_beast )
  {
    parse_options( options_str );

    harmful = false;

    if ( p->talents.shadow_hounds.ok() )
    {
      shadow_hounds.duration = p->talents.shadow_hounds_summon->duration();
    }
  }

  void execute() override
  {
    hunter_spell_t::execute();

    timespan_t summon_duration;
    int base_attacks_per_summon;
    std::tie( summon_duration, base_attacks_per_summon ) = pets::dire_beast_duration( p() );

    sim -> print_debug( "Dire Beast summoned with {} autoattacks", base_attacks_per_summon );

    p() -> pets.dire_beast.spawn( summon_duration );
    
    if ( p()->talents.shadow_hounds.ok() && rng().roll( p()->talents.shadow_hounds->effectN( 1 ).percent() ) && p()->specialization() == HUNTER_BEAST_MASTERY )
    {
      p()->pets.dark_hound.spawn( shadow_hounds.duration );
      if ( !p()->pets.dark_hound.active_pets().empty() )
      {
        p()->pets.dark_hound.active_pets().back()->buffs.beast_cleave->trigger( shadow_hounds.duration );
      }
    }
    if ( p()->talents.huntmasters_call.ok() )
    {
      p()->buffs.huntmasters_call->trigger();
      if ( p()->buffs.huntmasters_call->at_max_stacks() )
      {
        if ( rng().roll( 0.5 ) )
        {
          p()->buffs.summon_fenryr->trigger();
          p()->pets.fenryr.despawn();
          p()->pets.fenryr.spawn( p()->buffs.summon_fenryr->buff_duration() );
        }
        else
        {
          p()->buffs.summon_hati->trigger();
          p()->pets.hati.despawn();
          p()->pets.hati.spawn( p()->buffs.summon_hati->buff_duration() );
        }
        p()->buffs.huntmasters_call->expire();
      }
    }
  }
};

// Dire Command =============================================================

struct dire_command_summon_t final : hunter_spell_t
{
  struct
  {
    timespan_t duration = 0_s;
  } shadow_hounds;

  dire_command_summon_t( hunter_t* p ) : hunter_spell_t( "dire_command_summon", p, p -> find_spell( 219199 ) )
  {
    cooldown -> duration = 0_ms;
    track_cd_waste = false;
    background = true;
    harmful = false;
    
    if ( p->talents.shadow_hounds.ok() )
    {
      shadow_hounds.duration = p->talents.shadow_hounds_summon->duration();
    }
  }

  void execute() override
  {
    hunter_spell_t::execute();

    p() -> pets.dire_beast.spawn( pets::dire_beast_duration( p() ).first );
    if ( p()->talents.shadow_hounds.ok() && rng().roll( p()->talents.shadow_hounds->effectN( 1 ).percent() ) && p()->specialization() == HUNTER_BEAST_MASTERY )
    {
      p()->pets.dark_hound.spawn( shadow_hounds.duration );
      if ( !p()->pets.dark_hound.active_pets().empty() )
      {
        p()->pets.dark_hound.active_pets().back()->buffs.beast_cleave->trigger( shadow_hounds.duration );
      }
    }
  }
};

// Bestial Wrath ============================================================

struct bestial_wrath_t: public hunter_spell_t
{
  timespan_t precast_time = 0_ms;
  attacks::barbed_shot_tww_s2_bm_2pc_t* barbed_shot_tww_s2_bm_2pc = nullptr;

  bestial_wrath_t( hunter_t* p, util::string_view options_str ):
    hunter_spell_t( "bestial_wrath", p, p -> talents.bestial_wrath )
  {
    add_option( opt_timespan( "precast_time", precast_time ) );
    parse_options( options_str );

    precast_time = clamp( precast_time, 0_ms, data().duration() );

    if ( p->tier_set.tww_s2_bm_2pc.ok() && p->talents.barbed_shot.ok() )
      barbed_shot_tww_s2_bm_2pc = p->get_background_action<attacks::barbed_shot_tww_s2_bm_2pc_t>( "barbed_shot_tww_s2_bm_2pc" );
  }

  bool usable_precombat() const override
  {
    return true;
  }

  void init_finished() override
  {
    for ( auto pet : p() -> pet_list )
      add_pet_stats( pet, { "bestial_wrath" } );

    hunter_spell_t::init_finished();
  }

  void execute() override
  {
    hunter_spell_t::execute();

    trigger_buff( p() -> buffs.bestial_wrath, precast_time );

    for ( auto pet : pets::active<pets::hunter_main_pet_base_t>( p() -> pets.main, p() -> pets.animal_companion ) )
    {
      // Assume the pet is out of range / not engaged when precasting.
      if ( !is_precombat )
        pet -> actions.bestial_wrath -> execute_on_target( target );
      trigger_buff( pet -> buffs.bestial_wrath, precast_time );
    }

    adjust_precast_cooldown( precast_time );

    if ( p() -> talents.scent_of_blood.ok() )
      p() -> cooldowns.barbed_shot -> reset( true, as<int>( p() -> talents.scent_of_blood -> effectN( 1 ).base_value() ) );

    if ( p()->talents.lead_from_the_front->ok() )
    {
      p()->buffs.lead_from_the_front->trigger();
      p()->trigger_howl_of_the_pack_leader();
    }

    if ( !is_precombat )
    {
      if ( barbed_shot_tww_s2_bm_2pc )
          barbed_shot_tww_s2_bm_2pc->execute_on_target( target );

      if ( p()->tier_set.tww_s2_bm_4pc.ok() )
        p()->pets.main->buffs.potent_mutagen->trigger();
    }
  }

  bool ready() override
  {
    if ( !p() -> pets.main )
      return false;

    return hunter_spell_t::ready();
  }
};

// Call of the Wild =======================================================

struct call_of_the_wild_t: public hunter_spell_t
{
  call_of_the_wild_t( hunter_t* p, util::string_view options_str ):
    hunter_spell_t( "call_of_the_wild", p, p -> talents.call_of_the_wild )
  {
    parse_options( options_str );

    harmful = false;
    // disable automatic generation of the dot from spell data
    dot_duration = 0_ms;

    gcd_type = gcd_haste_type::ATTACK_HASTE;
  }

  void execute() override
  {
    hunter_spell_t::execute();

    p() -> buffs.call_of_the_wild -> trigger();
    p() -> pets.cotw_stable_pet.spawn( data().duration(), as<int>( data().effectN( 1 ).base_value() ) );

    double percent_reduction = p() -> talents.call_of_the_wild -> effectN( 3 ).base_value() / 100.0; 
    double on_cast_reduction = percent_reduction * as<int>( data().effectN( 1 ).base_value() );
    p() -> cooldowns.kill_command -> adjust( -( p() -> cooldowns.kill_command -> duration * on_cast_reduction ) );
    p() -> cooldowns.barbed_shot -> adjust( -( p() -> cooldowns.barbed_shot -> duration * on_cast_reduction ) );

    //2023-11-14 
    //When casting Call of the Wild with Bloody Frenzy talented it will apply beast cleave to the player, the main pet, AC pet and all call of the wild pets
    if ( p() -> talents.bloody_frenzy -> ok() )
    {
      timespan_t duration = p() -> buffs.call_of_the_wild -> remains();
      p() -> buffs.beast_cleave -> trigger( duration ); 
      for ( auto pet : pets::active<pets::hunter_pet_t>( p() -> pets.main, p() -> pets.animal_companion ) )
        pet -> buffs.beast_cleave -> trigger( duration );

      for ( auto pet : p() -> pets.cotw_stable_pet.active_pets() )
        pet -> buffs.beast_cleave -> trigger( duration );
    }

    p()->buffs.withering_fire->trigger( p()->buffs.call_of_the_wild->buff_duration() );
  }
};

// Bloodshed ================================================================

struct bloodshed_t : hunter_spell_t
{
  bloodshed_t( hunter_t* p, util::string_view options_str ):
    hunter_spell_t( "bloodshed", p, p -> talents.bloodshed )
  {
    parse_options( options_str );

    may_hit = false;
  }

  void init_finished() override
  {
    for ( auto pet : p() -> pet_list )
      add_pet_stats( pet, { "bloodshed" } );

    hunter_spell_t::init_finished();
  }

  void execute() override
  {
    hunter_spell_t::execute();

    if ( auto pet = p() -> pets.main )
      pet->actions.bloodshed -> execute_on_target( target );
  }

  bool target_ready( player_t* candidate_target ) override
  {
    return p() -> pets.main &&
           p() -> pets.main -> actions.bloodshed -> target_ready( candidate_target ) &&
           hunter_spell_t::target_ready( candidate_target );
  }

  bool ready() override
  {
    return p() -> pets.main &&
           p() -> pets.main -> actions.bloodshed -> ready() &&
           hunter_spell_t::ready();
  }
};

// A Murder of Crows ========================================================

struct a_murder_of_crows_t : public hunter_spell_t
{
  struct peck_t final : public hunter_ranged_attack_t
  {
    peck_t( util::string_view n, hunter_t* p ) : hunter_ranged_attack_t( n, p, p->find_spell( 131900 ) )
    {
      background = dual = true;
    }

    timespan_t travel_time() const override
    {
      return timespan_t::from_seconds( data().missile_speed() );
    }
  };

  a_murder_of_crows_t( hunter_t* p ) : hunter_spell_t( "a_murder_of_crows", p, p->talents.a_murder_of_crows_dot )
  {
    background = dual = true;
    tick_action = p->get_background_action<peck_t>( "a_murder_of_crows_peck" );
  }

  // Spell data for A Murder of Crows still has it listed as costing focus
  double cost() const override
  {
    return 0;
  }
};

//==============================
// Marksmanship spells
//==============================

// Harrier's Cry ===========================================================

struct harriers_cry_t: public hunter_spell_t
{
  harriers_cry_t( hunter_t* p, util::string_view options_str ) : hunter_spell_t( "harriers_cry", p, p->specs.harriers_cry )
  {
    parse_options( options_str );

    harmful = false;
    track_cd_waste = false;
  }

  void execute() override
  {
    hunter_spell_t::execute();

    for ( auto* p : sim->player_non_sleeping_list )
    {
      if ( p->buffs.exhaustion->check() || p->is_pet() )
        continue;

      p->buffs.bloodlust->trigger();
      p->buffs.exhaustion->trigger();
    }
  }

  bool ready() override
  {
    if ( !sim->overrides.bloodlust )
      return false;

    return hunter_spell_t::ready();
  }
};

// Trueshot =================================================================

struct trueshot_t: public hunter_spell_t
{
  trueshot_t( hunter_t* p, util::string_view options_str ):
    hunter_spell_t( "trueshot", p, p -> talents.trueshot )
  {
    parse_options( options_str );
  }

  void execute() override
  {
    hunter_spell_t::execute();

    p()->state.sentinel_watch_reduction = 0_s;
    p()->buffs.eyes_closed->trigger();

    // Applying Trueshot directly does not extend an existing Trueshot and resets Unerring Vision stacks.
    p() -> buffs.trueshot -> expire();
    p() -> buffs.trueshot -> trigger();
    
    if ( p()->talents.withering_fire.ok() && !is_precombat )
    {
      p()->buffs.withering_fire->trigger( p()->buffs.trueshot->data().duration() );
      p()->trigger_deathblow( true );
    }

    if ( p()->talents.feathered_frenzy.ok() )
      p()->trigger_spotters_mark( target, true );

    p()->buffs.double_tap->trigger();

    p()->buffs.jackpot->trigger();
  }
};

// Volley ===========================================================================

struct volley_t : public hunter_spell_t
{
  struct damage_t final : hunter_ranged_attack_t
  {
    struct salvo {
      attacks::explosive_shot_background_t* explosive = nullptr;
      int targets = 0;
    } salvo;

    damage_t( util::string_view n, hunter_t* p ) : hunter_ranged_attack_t( n, p, p -> talents.volley_dmg )
    {
      aoe = -1;
      background = dual = ground_aoe = true;

      if ( p -> talents.salvo.ok() )
      {
        salvo.targets = as<int>( p->talents.salvo->effectN( 1 ).base_value() );
        salvo.explosive = p -> get_background_action<attacks::explosive_shot_background_t>( "explosive_shot" );
      }
    }

    void execute() override
    {
      hunter_ranged_attack_t::execute();

      p()->cooldowns.salvo->start();
    }

    void impact( action_state_t* s ) override
    {
      hunter_ranged_attack_t::impact( s );

      if ( s->chain_target < salvo.targets && p()->cooldowns.salvo->up() )
        salvo.explosive->execute_on_target( s->target );

      if ( p()->talents.kill_zone.ok() )
        p()->get_target_data( s->target )->debuffs.kill_zone->trigger();

      p()->cooldowns.rapid_fire->adjust( -p()->talents.bullet_hell->effectN( 1 ).time_value() );
    }
  };

  damage_t* damage;
  timespan_t tick_duration;

  volley_t( hunter_t* p, util::string_view options_str ) : hunter_spell_t( "volley", p, p->talents.volley ),
    damage( p->get_background_action<damage_t>( "volley_damage" ) ),
    tick_duration( data().duration() )
  {
    parse_options( options_str );

    // disable automatic generation of the dot from spell data
    dot_duration = 0_ms;

    may_hit = false;
    damage -> stats = stats;
  }

  void execute() override
  {
    hunter_spell_t::execute();

    p() -> buffs.volley -> trigger( tick_duration );
    p() -> buffs.trick_shots -> trigger( tick_duration );

    p() -> state.current_volley = 
      make_event<ground_aoe_event_t>( *sim, player, ground_aoe_params_t()
        .target( execute_state -> target )
        .duration( tick_duration )
        .pulse_time( data().effectN( 2 ).period() )
        .action( damage )
        .state_callback( [ this ]( ground_aoe_params_t::state_type type, ground_aoe_event_t* event ) {
          switch ( type )
            {
              case ground_aoe_params_t::EVENT_CREATED:
                p() -> state.current_volley = event;
                break;
              case ground_aoe_params_t::EVENT_STOPPED:
              {
                p()->state.current_volley = nullptr;
                if ( p()->talents.kill_zone.ok() )
                  // Scheduled after next Volley tick.
                  make_event( *sim, 0_ms, [ this ]() { 
                    for ( player_t* t : sim->target_non_sleeping_list )
                      if ( t->is_enemy() )
                        p()->get_target_data( t )->debuffs.kill_zone->expire();
                  } );
                  
                break;
              }
              default:
                break;
            }
        } )
      );

    p()->buffs.double_tap->trigger();
  }
};

//==============================
// Survival spells
//==============================

// Spearhead ==============================================================

struct spearhead_t : public hunter_spell_t
{
  struct bleed_t : public hunter_melee_attack_t
  {
    bleed_t( util::string_view n, hunter_t* p ) : hunter_melee_attack_t( n, p, p->talents.spearhead_bleed )
    {
      background = dual = true;
    }

    dot_t* get_dot( player_t* t )
    {
      if ( !t )
        t = target;
      if ( !t )
        return nullptr;

      return td( t )->dots.spearhead;
    }
  };

  bleed_t* bleed;

  spearhead_t( hunter_t* p, util::string_view options_str ) : hunter_spell_t( "spearhead", p, p->talents.spearhead ),
    bleed( p->get_background_action<bleed_t>( "spearhead_bleed" ) )
  {
    parse_options( options_str );

    bleed->stats = stats;
    stats->action_list.push_back( bleed );

    decrements_tip_of_the_spear = false;
  }

  void execute() override
  {
    hunter_spell_t::execute();

    bleed->execute_on_target( target );

    if ( auto pet = p()->pets.main )
    {
      pet->get_target_data( target )->debuffs.spearhead->trigger();
      pet->buffs.spearhead->trigger();
    }
  }
};

// Wildfire Bomb ==============================================================

struct wildfire_bomb_base_t: public hunter_spell_t
{
  struct bomb_damage_t : public hunter_spell_t
  {
    struct bomb_dot_t final : public hunter_spell_t
    {
      bomb_dot_t( util::string_view n, hunter_t* p ) :
        hunter_spell_t( n, p, p->talents.wildfire_bomb_dot )
      {
        background = dual = true;
      }

      double composite_ta_multiplier( const action_state_t* s ) const override
      {
        double am = hunter_spell_t::composite_ta_multiplier( s );

        if ( as<double>( s->n_targets ) > reduced_aoe_targets )
          am *= std::sqrt( reduced_aoe_targets / s->n_targets );

        if ( s->chain_target == 0 )
          am *= 1.0 + p()->talents.wildfire_bomb->effectN( 3 ).percent();

        return am;
      }
    };

    bomb_dot_t* bomb_dot;

    bomb_damage_t( util::string_view n, hunter_t* p, wildfire_bomb_base_t* a ) : 
      hunter_spell_t( n, p, p->talents.wildfire_bomb_dmg ),
      bomb_dot( p->get_background_action<bomb_dot_t>( "wildfire_bomb_dot" ) )
    {
      background = dual = true;

      aoe = -1;
      reduced_aoe_targets = p -> talents.wildfire_bomb -> effectN( 2 ).base_value();
      radius = 5; // XXX: It's actually a circle + cone, but we sadly can't really model that

      bomb_dot->reduced_aoe_targets = reduced_aoe_targets;
      bomb_dot->aoe                 = aoe;
      bomb_dot->radius              = radius;

      a->add_child( this );
      a->add_child( bomb_dot );
    }

    void execute() override
    {
      hunter_spell_t::execute();

      if ( num_targets_hit > 0 )
      {
        // Dot applies to all of the same targets hit by the main explosion
        bomb_dot->target                    = target;
        bomb_dot->target_cache.list         = target_cache.list;
        bomb_dot->target_cache.is_valid     = true;
        bomb_dot->execute();
      }

      if ( rng().roll( p()->talents.grenade_juggler->effectN( 2 ).percent() ) )
        p()->cooldowns.explosive_shot->reset( true );

      if ( p()->buffs.lunar_storm_ready->up() )
        p()->trigger_lunar_storm( target );

      p()->buffs.wildfire_arsenal->expire();
    }

    double composite_da_multiplier( const action_state_t* s ) const override
    {
      double am = hunter_spell_t::composite_da_multiplier( s );

      if ( s->chain_target == 0 )
        am *= 1.0 + p()->talents.wildfire_bomb->effectN( 3 ).percent();

      return am;
    }
  };

  wildfire_bomb_base_t( hunter_t* p, const spell_data_t* s = spell_data_t::nil() ) : hunter_spell_t( "wildfire_bomb", p, s )
  {
    may_miss = false;
    school = SCHOOL_FIRE; // for report coloring
    harmful = false;

    impact_action = p->get_background_action<bomb_damage_t>( "wildfire_bomb_damage", this );
  }
};

struct wildfire_bomb_t: public wildfire_bomb_base_t
{
  struct
  {
    timespan_t extension = 0_s;
    timespan_t cap = 0_s;
  } fury_of_the_wyvern;

  wildfire_bomb_t( hunter_t* p, util::string_view options_str ) : wildfire_bomb_base_t( p, p->talents.wildfire_bomb )
  {
    parse_options( options_str );

    if ( p->talents.fury_of_the_wyvern.ok() )
    {
      fury_of_the_wyvern.extension = p->talents.fury_of_the_wyvern->effectN( 3 ).time_value();
      fury_of_the_wyvern.cap = timespan_t::from_seconds( p->talents.fury_of_the_wyvern->effectN( 4 ).base_value() );
    }
  }

  void execute() override
  {
    wildfire_bomb_base_t::execute();

    if ( p()->state.fury_of_the_wyvern_extension < fury_of_the_wyvern.cap )
    {
      p()->buffs.wyverns_cry->extend_duration( p(), fury_of_the_wyvern.extension );
      p()->state.fury_of_the_wyvern_extension += fury_of_the_wyvern.extension;
    }

    if ( p()->buffs.winning_streak->check() && rng().roll( p()->tier_set.tww_s2_sv_2pc->proc_chance() ) )
    {
      p()->buffs.winning_streak->expire();  // Consume 2pc buff
      if ( p()->tier_set.tww_s2_sv_4pc.ok() )
        p()->buffs.strike_it_rich->trigger(); // Apply 4pc buff
    }
  }
};

// Aspect of the Eagle ======================================================

struct aspect_of_the_eagle_t: public hunter_spell_t
{
  aspect_of_the_eagle_t( hunter_t* p, util::string_view options_str ):
    hunter_spell_t( "aspect_of_the_eagle", p, p -> find_spell( 186289 ) )
  {
    parse_options( options_str );

    harmful = false;
  }

  void execute() override
  {
    hunter_spell_t::execute();

    p() -> buffs.aspect_of_the_eagle -> trigger();
  }
};

// Muzzle =============================================================

struct muzzle_t: public interrupt_base_t
{
  muzzle_t( hunter_t* p, util::string_view options_str ):
    interrupt_base_t( "muzzle", p, p -> talents.muzzle )
  {
    parse_options( options_str );
  }
};

} // end namespace spells

namespace actions {

// Auto attack =======================================================================

struct auto_attack_t: public action_t
{
  auto_attack_t( hunter_t* p, util::string_view options_str ) :
    action_t( ACTION_OTHER, "auto_attack", p )
  {
    parse_options( options_str );

    ignore_false_positive = true;
    trigger_gcd = 0_ms;

    if ( p -> main_hand_weapon.type == WEAPON_NONE )
    {
      background = true;
    }
    else if ( p -> main_hand_weapon.group() == WEAPON_RANGED )
    {
      if ( p->talents.bleak_arrows.ok() )
        p -> main_hand_attack = new attacks::bleak_arrows_t( p );
      else
        p -> main_hand_attack = new attacks::auto_shot_t( p );
    }
    else
    {
      p -> main_hand_attack = new attacks::melee_t( p );
      range = 5;
    }
  }

  void execute() override
  {
    player -> main_hand_attack -> schedule_execute();
  }

  bool ready() override
  {
    if ( player->is_moving() && !usable_moving() )
      return false;

    return player -> main_hand_attack -> execute_event == nullptr; // not swinging
  }
};

} // end namespace actions

hunter_td_t::hunter_td_t( player_t* t, hunter_t* p ) : actor_target_data_t( t, p ),
  cooldowns(),
  debuffs(),
  dots()
{
  cooldowns.overwatch = t->get_cooldown( "overwatch" );
  cooldowns.overwatch->duration = timespan_t::from_seconds( p->talents.overwatch->effectN( 2 ).base_value() );

  debuffs.cull_the_herd = 
    make_buff( *this, "cull_the_herd", p->talents.cull_the_herd_debuff )
      ->set_default_value_from_effect( 1 )
      ->apply_affecting_effect( p->talents.born_to_kill->effectN( 2 ) );

  debuffs.wild_instincts = make_buff( *this, "wild_instincts", p -> find_spell( 424567 ) )
    -> set_default_value_from_effect( 1 );

  debuffs.outland_venom = make_buff( *this, "outland_venom", p->talents.outland_venom_debuff )
    -> set_default_value( p->talents.outland_venom_debuff->effectN( 1 ).percent() )
    -> set_period( 0_s );

  debuffs.kill_zone = make_buff( *this, "kill_zone", p->talents.kill_zone_debuff )
    -> set_default_value_from_effect( 2 )
    -> set_chance( p->talents.kill_zone.ok() );

  debuffs.spotters_mark = make_buff( *this, "spotters_mark", p->specs.spotters_mark_debuff )
    ->set_default_value( p->specs.spotters_mark_debuff->effectN( 1 ).percent() + p->talents.improved_spotters_mark->effectN( 1 ).percent() );

  debuffs.ohnahran_winds = make_buff( *this, "ohnahran_winds", p->talents.ohnahran_winds_debuff )
    ->set_default_value( p->talents.ohnahran_winds_debuff->effectN( 1 ).percent() + p->talents.improved_spotters_mark->effectN( 1 ).percent() );

  debuffs.shrapnel_shot = make_buff( *this, "shrapnel_shot", p->talents.shrapnel_shot_debuff )
    ->set_default_value_from_effect( 1 );

  debuffs.sentinel = make_buff( *this, "sentinel", p->talents.sentinel_debuff );

  debuffs.crescent_steel = make_buff( *this, "crescent_steel", p->talents.crescent_steel_debuff )
    -> set_tick_callback(
      [ this, p ]( buff_t*, int, const timespan_t& ) {
        p->trigger_sentinel( target, true, p->procs.crescent_steel_stacks );
      } );

  debuffs.lunar_storm = make_buff( *this, "lunar_storm", p->talents.lunar_storm_periodic_spell->effectN( 2 ).trigger() )
    -> set_default_value_from_effect( 1 )
    -> set_schools_from_effect( 1 );

  dots.serpent_sting = t -> get_dot( "serpent_sting", p );
  dots.a_murder_of_crows = t -> get_dot( "a_murder_of_crows", p );
  dots.wildfire_bomb = t -> get_dot( "wildfire_bomb_dot", p );
  dots.black_arrow = t -> get_dot( "black_arrow_dot", p );
  dots.barbed_shot = t -> get_dot( "barbed_shot", p );
  dots.explosive_shot = t->get_dot( "explosive_shot", p );
  dots.merciless_blow = t->get_dot( "merciless_blow", p );
  dots.spearhead = t->get_dot( "spearhead", p );

  t -> register_on_demise_callback( p, [this](player_t*) { target_demise(); } );
}

void hunter_td_t::target_demise()
{
  damaged = false;
  sentinel_imploding = false;

  // Don't pollute results at the end-of-iteration deaths of everyone
  if ( source -> sim -> event_mgr.canceled )
    return;

  hunter_t* p = static_cast<hunter_t*>( source );
  if ( p -> talents.terms_of_engagement.ok() && damaged )
  {
    p -> sim -> print_debug( "{} harpoon cooldown reset on damaged target death.", p -> name() );
    p -> cooldowns.harpoon -> reset( true );
  }
  if ( p->talents.soul_drinker.ok() && dots.black_arrow->is_ticking() && p->rng().roll( p->talents.soul_drinker->effectN( 1 ).percent() ) )
    p->trigger_deathblow();
}

/**
 * Hunter specific action expression
 *
 * Use this function for expressions which are bound to an action property such as target, cast_time etc.
 * If you need an expression tied to the player itself use the normal hunter_t::create_expression override.
 */
std::unique_ptr<expr_t> hunter_t::create_action_expression ( action_t& action, util::string_view expression_str )
{
  return player_t::create_action_expression( action, expression_str );
}

std::unique_ptr<expr_t> hunter_t::create_expression( util::string_view expression_str )
{
  auto splits = util::string_split<util::string_view>( expression_str, "." );

  if ( splits.size() == 2 && splits[ 0 ] == "tar_trap" )
  {
    if ( splits[ 1 ] == "up" )
      return make_fn_expr( expression_str, [ this ] { return state.tar_trap_aoe != nullptr; } );

    if ( splits[ 1 ] == "remains" )
    {
      return make_fn_expr( expression_str,
        [ this ]() -> timespan_t {
          if ( state.tar_trap_aoe == nullptr )
            return 0_ms;
          return state.tar_trap_aoe -> remains();
        } );
    }
  }
  else if ( splits.size() >= 2 && splits[ 0 ] == "pet" && splits[ 1 ] == "main" &&
            !util::str_compare_ci( options.summon_pet_str, "disabled" ) )
  {
    // fudge the expression to refer to the "main pet"
    splits[ 1 ] = options.summon_pet_str;
    return player_t::create_expression( util::string_join( splits, "." ) );
  }
  else if ( splits.size() == 1 && splits[ 0 ] == "howl_summon_ready" )
  {
    return make_fn_expr( expression_str,
      [ this ] {
        return buffs.howl_of_the_pack_leader_wyvern->check() || 
          buffs.howl_of_the_pack_leader_boar->check() ||
          buffs.howl_of_the_pack_leader_bear->check();
      } );
  }

  return player_t::create_expression( expression_str );
}

action_t* hunter_t::create_action( util::string_view name, util::string_view options_str )
{
  using namespace attacks;
  using namespace spells;

  if ( name == "aimed_shot"            ) return new             aimed_shot_t( this, options_str );
  if ( name == "arcane_shot"           ) return new            arcane_shot_t( this, options_str );
  if ( name == "aspect_of_the_eagle"   ) return new    aspect_of_the_eagle_t( this, options_str );
  if ( name == "auto_attack"           ) return new   actions::auto_attack_t( this, options_str );
  if ( name == "auto_shot"             ) return new   actions::auto_attack_t( this, options_str );
  if ( name == "barbed_shot"           ) return new            barbed_shot_t( this, options_str );
  if ( name == "barrage"               ) return new                barrage_t( this, options_str );
  if ( name == "bestial_wrath"         ) return new          bestial_wrath_t( this, options_str );
  if ( name == "black_arrow"           ) return new            black_arrow_t( this, options_str );
  if ( name == "bloodshed"             ) return new              bloodshed_t( this, options_str );
  if ( name == "bursting_shot"         ) return new          bursting_shot_t( this, options_str );
  if ( name == "butchery"              ) return new               butchery_t( this, options_str );
  if ( name == "call_of_the_wild"      ) return new       call_of_the_wild_t( this, options_str );
  if ( name == "cobra_shot"            ) return new             cobra_shot_t( this, options_str );
  if ( name == "coordinated_assault"   ) return new    coordinated_assault_t( this, options_str );
  if ( name == "counter_shot"          ) return new           counter_shot_t( this, options_str );
  if ( name == "dire_beast"            ) return new             dire_beast_t( this, options_str );
  if ( name == "explosive_shot"        ) return new         explosive_shot_t( this, options_str );
  if ( name == "flanking_strike"       ) return new        flanking_strike_t( this, options_str );
  if ( name == "freezing_trap"         ) return new          freezing_trap_t( this, options_str );
  if ( name == "fury_of_the_eagle"     ) return new      fury_of_the_eagle_t( this, options_str );
  if ( name == "harpoon"               ) return new                harpoon_t( this, options_str );
  if ( name == "harriers_cry"          ) return new           harriers_cry_t( this, options_str );
  if ( name == "high_explosive_trap"   ) return new    high_explosive_trap_t( this, options_str );
  if ( name == "implosive_trap"        ) return new         implosive_trap_t( this, options_str );
  if ( name == "kill_command"          ) return new           kill_command_t( this, options_str );
  if ( name == "kill_shot"             ) return new              kill_shot_t( this, options_str );
  if ( name == "muzzle"                ) return new                 muzzle_t( this, options_str );
  if ( name == "rapid_fire"            ) return new             rapid_fire_t( this, options_str );
  if ( name == "spearhead"             ) return new              spearhead_t( this, options_str );
  if ( name == "steady_shot"           ) return new            steady_shot_t( this, options_str );
  if ( name == "summon_pet"            ) return new             summon_pet_t( this, options_str );
  if ( name == "tar_trap"              ) return new               tar_trap_t( this, options_str );
  if ( name == "trueshot"              ) return new               trueshot_t( this, options_str );
  if ( name == "volley"                ) return new                 volley_t( this, options_str );
  if ( name == "wildfire_bomb"         ) return new          wildfire_bomb_t( this, options_str );

  if ( name == "raptor_strike" || name == "mongoose_bite" || name == "raptor_bite" || name == "mongoose_strike" )
  {
    if ( talents.mongoose_bite.ok() )
      return new mongoose_bite_t( this, options_str );
    else
      return new raptor_strike_t( this, options_str );
  }

  if ( name == "raptor_strike_eagle" || name == "mongoose_bite_eagle" || name == "raptor_bite_eagle" || name == "mongoose_strike_eagle" )
  {
    if ( talents.mongoose_bite.ok() )
      return new mongoose_bite_eagle_t( this, options_str );
    else
      return new raptor_strike_eagle_t( this, options_str );
  }

  if ( name == "multishot" )
  {
    if ( specialization() == HUNTER_MARKSMANSHIP )
      return new multishot_mm_t( this, options_str );
    if ( specialization() == HUNTER_BEAST_MASTERY )
      return new multishot_bm_t( this, options_str );
  }

  return player_t::create_action( name, options_str );
}

pet_t* hunter_t::create_pet( util::string_view pet_name, util::string_view pet_type )
{
  using namespace pets;

  pet_t* p = find_pet( pet_name );

  if ( p )
    return p;

  pet_e type = util::parse_pet_type( pet_type );
  if ( type > PET_NONE && type < PET_HUNTER )
    return new pets::hunter_main_pet_t( this, pet_name, type );

  if ( !pet_type.empty() )
  {
    throw std::invalid_argument(fmt::format("Pet '{}' has unknown type '{}'.", pet_name, pet_type ));
  }

  return nullptr;
}

void hunter_t::create_pets()
{
  if ( !util::str_compare_ci( options.summon_pet_str, "disabled" ) )
    create_pet( options.summon_pet_str, options.summon_pet_str );

  if ( talents.animal_companion.ok() )
    pets.animal_companion = new pets::animal_companion_t( this );
}

void hunter_t::init()
{
  player_t::init();
}

double hunter_t::resource_loss( resource_e resource_type, double amount, gain_t* g, action_t* a )
{
  double loss = player_t::resource_loss(resource_type, amount, g, a);

  return loss;
}

void hunter_t::init_spells()
{
  player_t::init_spells();

  // Hunter Tree
  talents.kill_shot                         = find_talent_spell( talent_tree::CLASS, "Kill Shot" );

  talents.deathblow                         = find_talent_spell( talent_tree::CLASS, "Deathblow" );
  talents.deathblow_buff                    = talents.deathblow.ok() ? find_spell( 378770 ) : spell_data_t::not_found();

  talents.tar_trap                          = find_talent_spell( talent_tree::CLASS, "Tar Trap" );

  talents.counter_shot                      = find_talent_spell( talent_tree::CLASS, "Counter Shot" );
  talents.muzzle                            = find_talent_spell( talent_tree::CLASS, "Muzzle" );

  talents.lone_survivor                     = find_talent_spell( talent_tree::CLASS, "Lone Survivor" );
  talents.specialized_arsenal               = find_talent_spell( talent_tree::CLASS, "Specialized Arsenal" );

  talents.explosive_shot                    = find_talent_spell( talent_tree::CLASS, "Explosive Shot" );
  talents.explosive_shot_cast               = find_spell( 212431 );
  talents.explosive_shot_damage             = find_spell( 212680 );

  talents.bursting_shot                     = find_talent_spell( talent_tree::CLASS, "Bursting Shot" );
  talents.trigger_finger                    = find_talent_spell( talent_tree::CLASS, "Trigger Finger" );
  talents.blackrock_munitions               = find_talent_spell( talent_tree::CLASS, "Blackrock Munitions" );
  talents.keen_eyesight                     = find_talent_spell( talent_tree::CLASS, "Keen Eyesight" );

  talents.serrated_tips                     = find_talent_spell( talent_tree::CLASS, "Serrated Tips" );
  talents.born_to_be_wild                   = find_talent_spell( talent_tree::CLASS, "Born To Be Wild" );
  talents.improved_traps                    = find_talent_spell( talent_tree::CLASS, "Improved Traps" );

  talents.high_explosive_trap               = find_talent_spell( talent_tree::CLASS, "High Explosive Trap" );
  talents.implosive_trap                    = find_talent_spell( talent_tree::CLASS, "Implosive Trap" );
  talents.explosive_trap_damage             = find_spell( 236777 );
  talents.unnatural_causes                  = find_talent_spell( talent_tree::CLASS, "Unnatural Causes" );
  talents.unnatural_causes_debuff           = talents.unnatural_causes.ok() ? find_spell( 459529 ) : spell_data_t::not_found();

  // Marksmanship Tree
  if ( specialization() == HUNTER_MARKSMANSHIP )
  {
    specs.multishot                           = find_specialization_spell( "Multi-Shot" );
    specs.eyes_in_the_sky                     = find_specialization_spell( "Eyes in the Sky" );
    specs.spotters_mark_debuff                = specs.eyes_in_the_sky.ok() ? find_spell( 466872 ) : spell_data_t::not_found();
    specs.harriers_cry                        = find_specialization_spell( "Harrier's Cry" );

    talents.aimed_shot                        = find_talent_spell( talent_tree::SPECIALIZATION, "Aimed Shot", HUNTER_MARKSMANSHIP );

    talents.rapid_fire                        = find_talent_spell( talent_tree::SPECIALIZATION, "Rapid Fire", HUNTER_MARKSMANSHIP );
    talents.rapid_fire_tick                   = talents.rapid_fire.ok() ? find_spell( 257045 ) : spell_data_t::not_found();
    talents.rapid_fire_energize               = talents.rapid_fire.ok() ? find_spell( 263585 ) : spell_data_t::not_found();
    talents.precise_shots                     = find_talent_spell( talent_tree::SPECIALIZATION, "Precise Shots", HUNTER_MARKSMANSHIP );
    talents.precise_shots_buff                = talents.precise_shots.ok() ? find_spell( 260242 ) : spell_data_t::not_found();

    talents.streamline                        = find_talent_spell( talent_tree::SPECIALIZATION, "Streamline", HUNTER_MARKSMANSHIP );
    talents.streamline_buff                   = talents.streamline.ok() ? find_spell( 342076 ) : spell_data_t::not_found();
    talents.trick_shots                       = find_talent_spell( talent_tree::SPECIALIZATION, "Trick Shots", HUNTER_MARKSMANSHIP );
    talents.trick_shots_data                  = find_spell( 257621 );
    talents.trick_shots_buff                  = find_spell( 257622 );
    talents.aspect_of_the_hydra               = find_talent_spell( talent_tree::SPECIALIZATION, "Aspect of the Hydra", HUNTER_MARKSMANSHIP );
    talents.ammo_conservation                 = find_talent_spell( talent_tree::SPECIALIZATION, "Ammo Conservation", HUNTER_MARKSMANSHIP );

    talents.penetrating_shots                 = find_talent_spell( talent_tree::SPECIALIZATION, "Penetrating Shots", HUNTER_MARKSMANSHIP );
    talents.improved_spotters_mark            = find_talent_spell( talent_tree::SPECIALIZATION, "Improved Spotter's Mark", HUNTER_MARKSMANSHIP );
    talents.unbreakable_bond                  = find_talent_spell( talent_tree::SPECIALIZATION, "Unbreakable Bond", HUNTER_MARKSMANSHIP );
    talents.lock_and_load                     = find_talent_spell( talent_tree::SPECIALIZATION, "Lock and Load", HUNTER_MARKSMANSHIP );
    
    talents.in_the_rhythm                     = find_talent_spell( talent_tree::SPECIALIZATION, "In the Rhythm", HUNTER_MARKSMANSHIP );
    talents.in_the_rhythm_buff                = talents.in_the_rhythm.ok() ? find_spell( 407405 ) : spell_data_t::not_found();
    talents.surging_shots                     = find_talent_spell( talent_tree::SPECIALIZATION, "Surging Shots", HUNTER_MARKSMANSHIP );
    talents.master_marksman                   = find_talent_spell( talent_tree::SPECIALIZATION, "Master Marksman", HUNTER_MARKSMANSHIP );
    talents.master_marksman_bleed             = talents.master_marksman.ok() ? find_spell( 269576 ) : spell_data_t::not_found();
    talents.quickdraw                         = find_talent_spell( talent_tree::SPECIALIZATION, "Quickdraw", HUNTER_MARKSMANSHIP );

    talents.improved_deathblow                = find_talent_spell( talent_tree::SPECIALIZATION, "Improved Deathblow", HUNTER_MARKSMANSHIP );
    talents.obsidian_arrowhead                = find_talent_spell( talent_tree::SPECIALIZATION, "Obsidian Arrowhead", HUNTER_MARKSMANSHIP );
    talents.on_target                         = find_talent_spell( talent_tree::SPECIALIZATION, "On Target", HUNTER_MARKSMANSHIP );
    talents.on_target_buff                    = talents.on_target.ok() ? find_spell( 474257 ) : spell_data_t::not_found();
    talents.trueshot                          = find_talent_spell( talent_tree::SPECIALIZATION, "Trueshot", HUNTER_MARKSMANSHIP );
    talents.moving_target                     = find_talent_spell( talent_tree::SPECIALIZATION, "Moving Target", HUNTER_MARKSMANSHIP );
    talents.moving_target_buff                = talents.moving_target.ok() ? find_spell( 474293 ) : spell_data_t::not_found();
    talents.precision_detonation              = find_talent_spell( talent_tree::SPECIALIZATION, "Precision Detonation", HUNTER_MARKSMANSHIP );
    talents.precision_detonation_buff         = talents.precision_detonation.ok() ? find_spell( 474199 ) : spell_data_t::not_found();

    talents.razor_fragments                   = find_talent_spell( talent_tree::SPECIALIZATION, "Razor Fragments", HUNTER_MARKSMANSHIP );
    talents.razor_fragments_bleed             = talents.razor_fragments.ok() ? find_spell( 385638 ) : spell_data_t::not_found();
    talents.razor_fragments_buff              = talents.razor_fragments.ok() ? find_spell( 388998 ) : spell_data_t::not_found();
    talents.headshot                          = find_talent_spell( talent_tree::SPECIALIZATION, "Headshot", HUNTER_MARKSMANSHIP );
    talents.deadeye                           = find_talent_spell( talent_tree::SPECIALIZATION, "Deadeye", HUNTER_MARKSMANSHIP );
    talents.no_scope                          = find_talent_spell( talent_tree::SPECIALIZATION, "No Scope", HUNTER_MARKSMANSHIP );
    talents.feathered_frenzy                  = find_talent_spell( talent_tree::SPECIALIZATION, "Feathered Frenzy", HUNTER_MARKSMANSHIP );
    talents.target_acquisition                = find_talent_spell( talent_tree::SPECIALIZATION, "Target Acquisition", HUNTER_MARKSMANSHIP );
    talents.shrapnel_shot                     = find_talent_spell( talent_tree::SPECIALIZATION, "Shrapnel Shot", HUNTER_MARKSMANSHIP );
    talents.shrapnel_shot_debuff              = talents.shrapnel_shot.ok() ? find_spell( 474310 ) : spell_data_t::not_found();
    talents.magnetic_gunpowder                = find_talent_spell( talent_tree::SPECIALIZATION, "Magnetic Gunpowder", HUNTER_MARKSMANSHIP );

    talents.eagles_accuracy                   = find_talent_spell( talent_tree::SPECIALIZATION, "Eagle's Accuracy", HUNTER_MARKSMANSHIP );
    talents.calling_the_shots                 = find_talent_spell( talent_tree::SPECIALIZATION, "Calling the Shots", HUNTER_MARKSMANSHIP );
    talents.bullseye                          = find_talent_spell( talent_tree::SPECIALIZATION, "Bullseye", HUNTER_MARKSMANSHIP );
    talents.bullseye_buff                     = talents.bullseye->effectN( 1 ).trigger();

    talents.improved_streamline               = find_talent_spell( talent_tree::SPECIALIZATION, "Improved Streamline", HUNTER_MARKSMANSHIP );
    talents.focused_aim                       = find_talent_spell( talent_tree::SPECIALIZATION, "Focused Aim", HUNTER_MARKSMANSHIP );
    talents.killer_mark                       = find_talent_spell( talent_tree::SPECIALIZATION, "Killer Mark", HUNTER_MARKSMANSHIP );
    talents.bulletstorm                       = find_talent_spell( talent_tree::SPECIALIZATION, "Bulletstorm", HUNTER_MARKSMANSHIP );
    talents.bulletstorm_buff                  = talents.bulletstorm.ok() ? find_spell( 389020 ) : spell_data_t::not_found();
    talents.tensile_bowstring                 = find_talent_spell( talent_tree::SPECIALIZATION, "Tensile Bowstring", HUNTER_MARKSMANSHIP );
    talents.volley                            = find_talent_spell( talent_tree::SPECIALIZATION, "Volley", HUNTER_MARKSMANSHIP );
    talents.volley_data                       = find_spell( 260243 );
    talents.volley_dmg                        = find_spell( 260247 );
    talents.ohnahran_winds                    = find_talent_spell( talent_tree::SPECIALIZATION, "Ohn'ahran Winds", HUNTER_MARKSMANSHIP );
    talents.ohnahran_winds_debuff             = talents.ohnahran_winds.ok() ? find_spell( 1215057 ) : spell_data_t::not_found();
    talents.small_game_hunter                 = find_talent_spell( talent_tree::SPECIALIZATION, "Small Game Hunter", HUNTER_MARKSMANSHIP );

    talents.windrunner_quiver                 = find_talent_spell( talent_tree::SPECIALIZATION, "Windrunner Quiver", HUNTER_MARKSMANSHIP );
    talents.incendiary_ammunition             = find_talent_spell( talent_tree::SPECIALIZATION, "Incendiary Ammunition", HUNTER_MARKSMANSHIP );
    talents.double_tap                        = find_talent_spell( talent_tree::SPECIALIZATION, "Double Tap", HUNTER_MARKSMANSHIP );
    talents.double_tap_buff                   = talents.double_tap.ok() ? find_spell( 260402 ) : spell_data_t::not_found();
    talents.unerring_vision                   = find_talent_spell( talent_tree::SPECIALIZATION, "Unerring Vision", HUNTER_MARKSMANSHIP );
    talents.kill_zone                         = find_talent_spell( talent_tree::SPECIALIZATION, "Kill Zone", HUNTER_MARKSMANSHIP );
    talents.kill_zone_debuff                  = talents.kill_zone.ok() ? find_spell( 393480 ) : spell_data_t::not_found();
    talents.salvo                             = find_talent_spell( talent_tree::SPECIALIZATION, "Salvo", HUNTER_MARKSMANSHIP );
    talents.bullet_hell                       = find_talent_spell( talent_tree::SPECIALIZATION, "Bullet Hell", HUNTER_MARKSMANSHIP );
  }

  // Beast Mastery Tree
  if ( specialization() == HUNTER_BEAST_MASTERY )
  {
    talents.kill_command                      = find_talent_spell( talent_tree::SPECIALIZATION, "Kill Command", HUNTER_BEAST_MASTERY );
    talents.kill_command_pet                  = talents.kill_command.ok() ? find_spell( 83381 ) : spell_data_t::not_found();

    talents.cobra_shot                        = find_talent_spell( talent_tree::SPECIALIZATION, "Cobra Shot", HUNTER_BEAST_MASTERY );
    talents.animal_companion                  = find_talent_spell( talent_tree::SPECIALIZATION, "Animal Companion", HUNTER_BEAST_MASTERY );
    talents.solitary_companion                = find_talent_spell( talent_tree::SPECIALIZATION, "Solitary Companion", HUNTER_BEAST_MASTERY );
    talents.barbed_shot                       = find_talent_spell( talent_tree::SPECIALIZATION, "Barbed Shot", HUNTER_BEAST_MASTERY );

    talents.pack_tactics                      = find_talent_spell( talent_tree::SPECIALIZATION, "Pack Tactics", HUNTER_BEAST_MASTERY );
    talents.aspect_of_the_beast               = find_talent_spell( talent_tree::SPECIALIZATION, "Aspect of the Beast", HUNTER_BEAST_MASTERY );
    talents.war_orders                        = find_talent_spell( talent_tree::SPECIALIZATION, "War Orders", HUNTER_BEAST_MASTERY );
    talents.thrill_of_the_hunt                = find_talent_spell( talent_tree::SPECIALIZATION, "Thrill of the Hunt", HUNTER_BEAST_MASTERY );

    talents.go_for_the_throat                 = find_talent_spell( talent_tree::SPECIALIZATION, "Go for the Throat", HUNTER_BEAST_MASTERY );
    talents.multishot_bm                      = find_talent_spell( talent_tree::SPECIALIZATION, "Multi-Shot", HUNTER_BEAST_MASTERY );
    talents.laceration                        = find_talent_spell( talent_tree::SPECIALIZATION, "Laceration", HUNTER_BEAST_MASTERY );
    talents.laceration_driver                 = talents.laceration.ok() ? find_spell( 459555 ) : spell_data_t::not_found();
    talents.laceration_bleed                  = talents.laceration.ok() ? find_spell( 459560 ) : spell_data_t::not_found();
    
    talents.barbed_scales                     = find_talent_spell( talent_tree::SPECIALIZATION, "Barbed Scales", HUNTER_BEAST_MASTERY );
    talents.snakeskin_quiver                  = find_talent_spell( talent_tree::SPECIALIZATION, "Snakeskin Quiver", HUNTER_BEAST_MASTERY );
    talents.cobra_senses                      = find_talent_spell( talent_tree::SPECIALIZATION, "Cobra Senses", HUNTER_BEAST_MASTERY );
    talents.alpha_predator                    = find_talent_spell( talent_tree::SPECIALIZATION, "Alpha Predator", HUNTER_BEAST_MASTERY );
    talents.beast_cleave                      = find_talent_spell( talent_tree::SPECIALIZATION, "Beast Cleave", HUNTER_BEAST_MASTERY );
    talents.wild_call                         = find_talent_spell( talent_tree::SPECIALIZATION, "Wild Call", HUNTER_BEAST_MASTERY );
    talents.hunters_prey                      = find_talent_spell( talent_tree::SPECIALIZATION, "Hunter's Prey", HUNTER_BEAST_MASTERY );
    talents.hunters_prey_hidden_buff          = find_spell( 468219 );
    talents.poisoned_barbs                    = find_talent_spell( talent_tree::SPECIALIZATION, "Poisoned Barbs", HUNTER_BEAST_MASTERY );

    talents.stomp                             = find_talent_spell( talent_tree::SPECIALIZATION, "Stomp", HUNTER_BEAST_MASTERY );
    talents.stomp_primary                     = find_spell( 1217528 );
    talents.stomp_cleave                      = find_spell( 201754 );
    talents.serpentine_rhythm                 = find_talent_spell( talent_tree::SPECIALIZATION, "Serpentine Rhythm", HUNTER_BEAST_MASTERY );
    talents.kill_cleave                       = find_talent_spell( talent_tree::SPECIALIZATION, "Kill Cleave", HUNTER_BEAST_MASTERY );
    talents.training_expert                   = find_talent_spell( talent_tree::SPECIALIZATION, "Training Expert", HUNTER_BEAST_MASTERY );
    talents.dire_beast                        = find_talent_spell( talent_tree::SPECIALIZATION, "Dire Beast", HUNTER_BEAST_MASTERY );

    talents.a_murder_of_crows                 = find_talent_spell( talent_tree::SPECIALIZATION, "A Murder of Crows", HUNTER_BEAST_MASTERY );
    talents.a_murder_of_crows_dot             = talents.a_murder_of_crows.ok() ? find_spell( 131894 ) : spell_data_t::not_found();
    talents.barrage                           = find_talent_spell( talent_tree::SPECIALIZATION, "Barrage", HUNTER_BEAST_MASTERY );
    talents.savagery                          = find_talent_spell( talent_tree::SPECIALIZATION, "Savagery", HUNTER_BEAST_MASTERY );
    talents.bestial_wrath                     = find_talent_spell( talent_tree::SPECIALIZATION, "Bestial Wrath", HUNTER_BEAST_MASTERY );
    talents.dire_command                      = find_talent_spell( talent_tree::SPECIALIZATION, "Dire Command", HUNTER_BEAST_MASTERY );
    talents.huntmasters_call                  = find_talent_spell( talent_tree::SPECIALIZATION, "Huntmaster's Call", HUNTER_BEAST_MASTERY );
    talents.dire_cleave                       = find_talent_spell( talent_tree::SPECIALIZATION, "Dire Cleave", HUNTER_BEAST_MASTERY );

    talents.killer_instinct                   = find_talent_spell( talent_tree::SPECIALIZATION, "Killer Instinct", HUNTER_BEAST_MASTERY );
    talents.master_handler                    = find_talent_spell( talent_tree::SPECIALIZATION, "Master Handler", HUNTER_BEAST_MASTERY );
    talents.barbed_wrath                      = find_talent_spell( talent_tree::SPECIALIZATION, "Barbed Wrath", HUNTER_BEAST_MASTERY );
    talents.thundering_hooves                 = find_talent_spell( talent_tree::SPECIALIZATION, "Thundering Hooves", HUNTER_BEAST_MASTERY );
    talents.dire_frenzy                       = find_talent_spell( talent_tree::SPECIALIZATION, "Dire Frenzy", HUNTER_BEAST_MASTERY );

    talents.call_of_the_wild                  = find_talent_spell( talent_tree::SPECIALIZATION, "Call of the Wild", HUNTER_BEAST_MASTERY );
    talents.killer_cobra                      = find_talent_spell( talent_tree::SPECIALIZATION, "Killer Cobra", HUNTER_BEAST_MASTERY );
    talents.scent_of_blood                    = find_talent_spell( talent_tree::SPECIALIZATION, "Scent of Blood", HUNTER_BEAST_MASTERY );
    talents.brutal_companion                  = find_talent_spell( talent_tree::SPECIALIZATION, "Brutal Companion", HUNTER_BEAST_MASTERY );
    talents.bloodshed                         = find_talent_spell( talent_tree::SPECIALIZATION, "Bloodshed", HUNTER_BEAST_MASTERY );
    talents.bloodshed_dot                     = talents.bloodshed.ok() ? find_spell( 321538 ) : spell_data_t::not_found();
    talents.bloodshed_debuff                  = talents.bloodshed.ok() ? find_spell( 346396 ) : spell_data_t::not_found();

    talents.wild_instincts                    = find_talent_spell( talent_tree::SPECIALIZATION, "Wild Instincts", HUNTER_BEAST_MASTERY );
    talents.bloody_frenzy                     = find_talent_spell( talent_tree::SPECIALIZATION, "Bloody Frenzy", HUNTER_BEAST_MASTERY );
    talents.piercing_fangs                    = find_talent_spell( talent_tree::SPECIALIZATION, "Piercing Fangs", HUNTER_BEAST_MASTERY );
    talents.venomous_bite                     = find_talent_spell( talent_tree::SPECIALIZATION, "Venomous Bite", HUNTER_BEAST_MASTERY );
    talents.venomous_bite_debuff              = talents.venomous_bite.ok() ? find_spell( 459668 ) : spell_data_t::not_found();
    talents.shower_of_blood                   = find_talent_spell( talent_tree::SPECIALIZATION, "Shower of Blood", HUNTER_BEAST_MASTERY );

    specs.serpent_sting = find_spell( 271788 );
  }

  // Survival Tree
  if ( specialization() == HUNTER_SURVIVAL )
  {
    talents.kill_command                      = find_talent_spell( talent_tree::SPECIALIZATION, "Kill Command", HUNTER_SURVIVAL );
    talents.kill_command_pet                  = talents.kill_command.ok() ? find_spell( 259277 ) : spell_data_t::not_found();

    talents.wildfire_bomb                     = find_talent_spell( talent_tree::SPECIALIZATION, "Wildfire Bomb", HUNTER_SURVIVAL );
    talents.wildfire_bomb_data                = find_spell( 259495 );
    talents.wildfire_bomb_dmg                 = find_spell( 265157 );
    talents.wildfire_bomb_dot                 = find_spell( 269747 );
    talents.raptor_strike                     = find_talent_spell( talent_tree::SPECIALIZATION, "Raptor Strike", HUNTER_SURVIVAL );
    talents.raptor_strike_eagle               = talents.raptor_strike.ok() ? find_spell( 265189 ) : spell_data_t::not_found();

    talents.guerrilla_tactics                 = find_talent_spell( talent_tree::SPECIALIZATION, "Guerrilla Tactics", HUNTER_SURVIVAL );
    talents.tip_of_the_spear                  = find_talent_spell( talent_tree::SPECIALIZATION, "Tip of the Spear", HUNTER_SURVIVAL );
    talents.tip_of_the_spear_buff             = talents.tip_of_the_spear.ok() ? find_spell( 260286 ) : spell_data_t::not_found();
    talents.tip_of_the_spear_explosive_buff   = talents.tip_of_the_spear.ok() ? find_spell( 460852 ) : spell_data_t::not_found();
    talents.tip_of_the_spear_fote_buff        = talents.tip_of_the_spear.ok() ? find_spell( 471536 ) : spell_data_t::not_found();

    talents.lunge                             = find_talent_spell( talent_tree::SPECIALIZATION, "Lunge", HUNTER_SURVIVAL );
    talents.quick_shot                        = find_talent_spell( talent_tree::SPECIALIZATION, "Quick Shot", HUNTER_SURVIVAL );
    talents.mongoose_bite                     = find_talent_spell( talent_tree::SPECIALIZATION, "Mongoose Bite", HUNTER_SURVIVAL );
    talents.mongoose_bite_eagle               = talents.mongoose_bite.ok() ? find_spell( 265888 ) : spell_data_t::not_found();
    talents.mongoose_fury                     = talents.mongoose_bite.ok() ? find_spell( 259388 ) : spell_data_t::not_found();
    talents.flankers_advantage                = find_talent_spell( talent_tree::SPECIALIZATION, "Flanker's Advantage", HUNTER_SURVIVAL );

    talents.wildfire_infusion                 = find_talent_spell( talent_tree::SPECIALIZATION, "Wildfire Infusion", HUNTER_SURVIVAL );
    talents.wildfire_arsenal                  = find_talent_spell( talent_tree::SPECIALIZATION, "Wildfire Arsenal", HUNTER_SURVIVAL );
    talents.wildfire_arsenal_buff             = talents.wildfire_arsenal.ok() ? find_spell( 1223701 ) : spell_data_t::not_found();
    talents.sulfurlined_pockets               = find_talent_spell( talent_tree::SPECIALIZATION, "Sulfur-Lined Pockets", HUNTER_SURVIVAL );
    talents.sulfurlined_pockets_building_buff = talents.sulfurlined_pockets.ok() ? find_spell( 459830 ) : spell_data_t::not_found();
    talents.sulfurlined_pockets_ready_buff    = talents.sulfurlined_pockets.ok() ? find_spell( 459834 ) : spell_data_t::not_found();
    talents.butchery                          = find_talent_spell( talent_tree::SPECIALIZATION, "Butchery", HUNTER_SURVIVAL );
    talents.flanking_strike                   = find_talent_spell( talent_tree::SPECIALIZATION, "Flanking Strike", HUNTER_SURVIVAL );
    talents.flanking_strike_player            = talents.flanking_strike.ok() ? find_spell( 269752 ) : spell_data_t::not_found();
    talents.flanking_strike_pet               = talents.flanking_strike.ok() ? find_spell( 259516 ) : spell_data_t::not_found();
    talents.bloody_claws                      = find_talent_spell( talent_tree::SPECIALIZATION, "Bloody Claws", HUNTER_SURVIVAL );
    talents.alpha_predator                    = find_talent_spell( talent_tree::SPECIALIZATION, "Alpha Predator", HUNTER_SURVIVAL );
    talents.ranger                            = find_talent_spell( talent_tree::SPECIALIZATION, "Ranger", HUNTER_SURVIVAL );
    
    talents.grenade_juggler                   = find_talent_spell( talent_tree::SPECIALIZATION, "Grenade Juggler", HUNTER_SURVIVAL );
    talents.cull_the_herd                     = find_talent_spell( talent_tree::SPECIALIZATION, "Cull the Herd", HUNTER_SURVIVAL );
    talents.cull_the_herd_debuff              = talents.cull_the_herd.ok() ? find_spell( 1217430 ) : spell_data_t::not_found();
    talents.frenzy_strikes                    = find_talent_spell( talent_tree::SPECIALIZATION, "Frenzy Strikes", HUNTER_SURVIVAL );
    talents.frenzy_strikes_buff               = talents.frenzy_strikes.ok() ? find_spell( 1217377 ) : spell_data_t::not_found();
    talents.merciless_blow                    = find_talent_spell( talent_tree::SPECIALIZATION, "Merciless Blow", HUNTER_SURVIVAL );
    talents.merciless_blow_flanking_bleed     = talents.merciless_blow.ok() ? find_spell( 1217375 ) : spell_data_t::not_found();
    talents.merciless_blow_butchery_bleed     = talents.merciless_blow.ok() ? find_spell( 459870 ) : spell_data_t::not_found();
    talents.vipers_venom                      = find_talent_spell( talent_tree::SPECIALIZATION, "Viper's Venom", HUNTER_SURVIVAL );
    talents.bloodseeker                       = find_talent_spell( talent_tree::SPECIALIZATION, "Bloodseeker", HUNTER_SURVIVAL );

    talents.terms_of_engagement               = find_talent_spell( talent_tree::SPECIALIZATION, "Terms of Engagement", HUNTER_SURVIVAL );
    talents.terms_of_engagement_dmg           = talents.terms_of_engagement.ok() ? find_spell( 271625 ) : spell_data_t::not_found();
    talents.terms_of_engagement_buff          = talents.terms_of_engagement.ok() ? find_spell( 265898 ) : spell_data_t::not_found();
    talents.born_to_kill                      = find_talent_spell( talent_tree::SPECIALIZATION, "Born to Kill", HUNTER_SURVIVAL );
    talents.tactical_advantage                = find_talent_spell( talent_tree::SPECIALIZATION, "Tactical Advantage", HUNTER_SURVIVAL );
    talents.sic_em                            = find_talent_spell( talent_tree::SPECIALIZATION, "Sic 'Em", HUNTER_SURVIVAL );
    talents.contagious_reagents               = find_talent_spell( talent_tree::SPECIALIZATION, "Contagious Reagents", HUNTER_SURVIVAL );
    talents.outland_venom                     = find_talent_spell( talent_tree::SPECIALIZATION, "Outland Venom", HUNTER_SURVIVAL );
    talents.outland_venom_debuff              = talents.outland_venom.ok() ? find_spell( 459941 ) : spell_data_t::not_found();

    talents.explosives_expert                 = find_talent_spell( talent_tree::SPECIALIZATION, "Explosives Expert", HUNTER_SURVIVAL );
    talents.sweeping_spear                    = find_talent_spell( talent_tree::SPECIALIZATION, "Sweeping Spear", HUNTER_SURVIVAL );
    talents.killer_companion                  = find_talent_spell( talent_tree::SPECIALIZATION, "Killer Companion", HUNTER_SURVIVAL );
    
    talents.fury_of_the_eagle                 = find_talent_spell( talent_tree::SPECIALIZATION, "Fury of the Eagle", HUNTER_SURVIVAL );
    talents.coordinated_assault               = find_talent_spell( talent_tree::SPECIALIZATION, "Coordinated Assault", HUNTER_SURVIVAL );
    talents.coordinated_assault_dmg           = talents.coordinated_assault.ok() ? find_spell( 360969 ) : spell_data_t::not_found();
    talents.spearhead                         = find_talent_spell( talent_tree::SPECIALIZATION, "Spearhead", HUNTER_SURVIVAL );
    talents.spearhead_bleed                   = talents.spearhead.ok() ? find_spell( 378957 ) : spell_data_t::not_found();
    talents.spearhead_debuff                  = talents.spearhead.ok() ? find_spell( 1221386 ) : spell_data_t::not_found();
    
    talents.ruthless_marauder                 = find_talent_spell( talent_tree::SPECIALIZATION, "Ruthless Marauder", HUNTER_SURVIVAL );
    talents.ruthless_marauder_buff            = talents.ruthless_marauder.ok() ? find_spell( 470070 ) : spell_data_t::not_found();
    talents.symbiotic_adrenaline              = find_talent_spell( talent_tree::SPECIALIZATION, "Symbiotic Adrenaline", HUNTER_SURVIVAL );
    talents.relentless_primal_ferocity        = find_talent_spell( talent_tree::SPECIALIZATION, "Relentless Primal Ferocity", HUNTER_SURVIVAL );
    talents.relentless_primal_ferocity_buff   = talents.relentless_primal_ferocity.ok() ? find_spell( 459962 ) : spell_data_t::not_found();
    talents.bombardier                        = find_talent_spell( talent_tree::SPECIALIZATION, "Bombardier", HUNTER_SURVIVAL );
    talents.bombardier_buff                   = talents.bombardier.ok() ? find_spell( 459859 ) : spell_data_t::not_found();
    talents.deadly_duo                        = find_talent_spell( talent_tree::SPECIALIZATION, "Deadly Duo", HUNTER_SURVIVAL );

    specs.serpent_sting = find_spell( 259491 );
    specs.harpoon = find_spell( 190925 );
  }

  if ( specialization() == HUNTER_MARKSMANSHIP || specialization() == HUNTER_BEAST_MASTERY )
  {
    // Dark Ranger
    talents.black_arrow = find_talent_spell( talent_tree::HERO, "Black Arrow" );
    talents.black_arrow_spell = talents.black_arrow.ok() ? find_spell( 466930 ) : spell_data_t::not_found();
    talents.black_arrow_dot = talents.black_arrow.ok() ? find_spell( 468572 ) : spell_data_t::not_found();

    talents.bleak_arrows = find_talent_spell( talent_tree::HERO, "Bleak Arrows" );
    talents.bleak_arrows_spell = talents.bleak_arrows.ok() ? find_spell( 467718 ) : spell_data_t::not_found();
    talents.shadow_hounds = find_talent_spell( talent_tree::HERO, "Shadow Hounds" );
    talents.shadow_hounds_summon = talents.shadow_hounds.ok() ? find_spell( 442419 ) : spell_data_t::not_found();
    talents.soul_drinker = find_talent_spell( talent_tree::HERO, "Soul Drinker" );
    talents.the_bell_tolls = find_talent_spell( talent_tree::HERO, "The Bell Tolls" );

    talents.phantom_pain = find_talent_spell( talent_tree::HERO, "Phantom Pain" );
    talents.phantom_pain_spell = talents.phantom_pain.ok() ? find_spell( 468019 ) : spell_data_t::not_found();
    talents.ebon_bowstring = find_talent_spell( talent_tree::HERO, "Ebon Bowstring" );

    talents.banshees_mark = find_talent_spell( talent_tree::HERO, "Banshee's Mark" );
    if ( !talents.a_murder_of_crows.ok() )
      talents.a_murder_of_crows_dot = talents.banshees_mark.ok() ? find_spell( 131894 ) : spell_data_t::not_found();
    talents.shadow_surge  = find_talent_spell( talent_tree::HERO, "Shadow Surge" );
    talents.shadow_surge_spell = talents.shadow_surge.ok() ? find_spell( 444269 ) : spell_data_t::not_found();
    talents.bleak_powder  = find_talent_spell( talent_tree::HERO, "Bleak Powder" );
    talents.bleak_powder_spell = talents.bleak_powder.ok() ? ( specialization() == HUNTER_MARKSMANSHIP ? find_spell( 467914 ) : find_spell( 472084 ) ) : spell_data_t::not_found();
    talents.withering_fire = find_talent_spell( talent_tree::HERO, "Withering Fire" );
    talents.withering_fire_black_arrow = talents.withering_fire.ok() ? find_spell( 468037 ) : spell_data_t::not_found();
    talents.withering_fire_buff = talents.withering_fire.ok() ? find_spell( 466991 ) : spell_data_t::not_found();
  }

  if ( specialization() == HUNTER_BEAST_MASTERY || specialization() == HUNTER_SURVIVAL )
  {
    // Pack Leader
    talents.howl_of_the_pack_leader = find_talent_spell( talent_tree::HERO, "Howl of the Pack Leader" );
    talents.howl_of_the_pack_leader_wyvern_ready_buff = talents.howl_of_the_pack_leader.ok() ? find_spell( 471878 ) : spell_data_t::not_found();
    talents.howl_of_the_pack_leader_boar_ready_buff = talents.howl_of_the_pack_leader.ok() ? find_spell( 472324 ) : spell_data_t::not_found();
    talents.howl_of_the_pack_leader_bear_ready_buff = talents.howl_of_the_pack_leader.ok() ? find_spell( 472325 ) : spell_data_t::not_found();
    talents.howl_of_the_pack_leader_cooldown_buff = talents.howl_of_the_pack_leader.ok() ? find_spell( 471877 ) : spell_data_t::not_found();
    talents.howl_of_the_pack_leader_wyvern_summon = talents.howl_of_the_pack_leader.ok() ? find_spell( 1222271 ) : spell_data_t::not_found();
    talents.howl_of_the_pack_leader_wyvern_buff = talents.howl_of_the_pack_leader.ok() ? find_spell( 471881 ) : spell_data_t::not_found();
    talents.howl_of_the_pack_leader_boar_charge_trigger = talents.howl_of_the_pack_leader.ok() ? find_spell( 472020 ) : spell_data_t::not_found();
    talents.howl_of_the_pack_leader_boar_charge_impact = talents.howl_of_the_pack_leader.ok() ? find_spell( 471936 ) : spell_data_t::not_found();
    talents.howl_of_the_pack_leader_boar_charge_cleave = talents.howl_of_the_pack_leader.ok() ? find_spell( 471938 ) : spell_data_t::not_found();
    talents.howl_of_the_pack_leader_bear_summon = talents.howl_of_the_pack_leader.ok() ? find_spell( 471993 ) : spell_data_t::not_found();
    talents.howl_of_the_pack_leader_bear_buff = talents.howl_of_the_pack_leader.ok() ? find_spell( 1225858 ) : spell_data_t::not_found();
    talents.howl_of_the_pack_leader_bear_bleed = talents.howl_of_the_pack_leader.ok() ? find_spell( 471999 ) : spell_data_t::not_found();

    talents.pack_mentality = find_talent_spell( talent_tree::HERO, "Pack Mentality" );
    talents.dire_summons = find_talent_spell( talent_tree::HERO, "Dire Summons" );
    talents.better_together = find_talent_spell( talent_tree::HERO, "Better Together" );

    talents.ursine_fury = find_talent_spell( talent_tree::HERO, "Ursine Fury" );
    talents.ursine_fury_chance = talents.ursine_fury.ok() ? find_spell( 472478 ) : spell_data_t::not_found();
    talents.envenomed_fangs = find_talent_spell( talent_tree::HERO, "Envenomed Fangs" );
    talents.envenomed_fangs_spell = talents.envenomed_fangs.ok() ? find_spell( 472525 ) : spell_data_t::not_found();
    talents.fury_of_the_wyvern = find_talent_spell( talent_tree::HERO, "Fury of the Wyvern" );
    talents.fury_of_the_wyvern_proc = talents.fury_of_the_wyvern.ok() ? find_spell( 472552 ) : spell_data_t::not_found();
    talents.hogstrider = find_talent_spell( talent_tree::HERO, "Hogstrider" );
    talents.hogstrider_buff = talents.hogstrider.ok() ? find_spell( 472640 ) : spell_data_t::not_found();

    talents.no_mercy = find_talent_spell( talent_tree::HERO, "No Mercy" );

    talents.lead_from_the_front = find_talent_spell( talent_tree::HERO, "Lead From the Front" );
    talents.lead_from_the_front_buff = talents.lead_from_the_front.ok() ? find_spell( 472743 ) : spell_data_t::not_found();
  }

  if ( specialization() == HUNTER_MARKSMANSHIP || specialization() == HUNTER_SURVIVAL )
  {
    // Sentinel
    talents.sentinel = find_talent_spell( talent_tree::HERO, "Sentinel" );
    talents.sentinel_debuff = talents.sentinel.ok() ? find_spell( 450387 ) : spell_data_t::not_found();
    talents.sentinel_tick = talents.sentinel.ok() ? find_spell( 450412 ) : spell_data_t::not_found();

    talents.extrapolated_shots = find_talent_spell( talent_tree::HERO, "Extrapolated Shots" );
    talents.sentinel_precision = find_talent_spell( talent_tree::HERO, "Sentinel Precision" );

    talents.release_and_reload = find_talent_spell( talent_tree::HERO, "Release and Reload" );
    talents.invigorating_pulse = find_talent_spell( talent_tree::HERO, "Invigorating Pulse" );

    talents.sentinel_watch = find_talent_spell( talent_tree::HERO, "Sentinel Watch" );
    talents.eyes_closed = find_talent_spell( talent_tree::HERO, "Eyes Closed" );
    talents.symphonic_arsenal = find_talent_spell( talent_tree::HERO, "Symphonic Arsenal" );
    talents.symphonic_arsenal_spell = talents.symphonic_arsenal.ok() ? find_spell( 451194 ) : spell_data_t::not_found();
    talents.overwatch = find_talent_spell( talent_tree::HERO, "Overwatch" );
    talents.crescent_steel = find_talent_spell( talent_tree::HERO, "Crescent Steel" );
    talents.crescent_steel_debuff = talents.crescent_steel.ok() ? find_spell( 451531 ) : spell_data_t::not_found();

    talents.lunar_storm = find_talent_spell( talent_tree::HERO, "Lunar Storm" );
    talents.lunar_storm_initial_spell = talents.lunar_storm.ok() ? find_spell( 1217459 ) : spell_data_t::not_found();
    talents.lunar_storm_periodic_trigger = talents.lunar_storm.ok() ? find_spell( 450978 ) : spell_data_t::not_found();
    talents.lunar_storm_periodic_spell = talents.lunar_storm.ok() ? find_spell( 450883 ) : spell_data_t::not_found();
    talents.lunar_storm_ready_buff = talents.lunar_storm.ok() ? find_spell( 451805 ) : spell_data_t::not_found();
    talents.lunar_storm_cooldown_buff = talents.lunar_storm.ok() ? find_spell( 451803 ) : spell_data_t::not_found();
  }

  // Mastery
  mastery.master_of_beasts     = find_mastery_spell( HUNTER_BEAST_MASTERY );
  mastery.sniper_training      = find_mastery_spell( HUNTER_MARKSMANSHIP );
  mastery.spirit_bond          = find_mastery_spell( HUNTER_SURVIVAL );
  mastery.spirit_bond_buff     = mastery.spirit_bond.ok() ? find_spell( 459722 ) : spell_data_t::not_found();

  // Spec spells
  specs.critical_strikes     = find_spell( 157443 );
  specs.hunter               = find_spell( 137014 );
  specs.beast_mastery_hunter = find_specialization_spell( "Beast Mastery Hunter" );
  specs.marksmanship_hunter  = find_specialization_spell( "Marksmanship Hunter" );
  specs.survival_hunter      = find_specialization_spell( "Survival Hunter" );

  specs.auto_shot            = find_spell( 75 );
  specs.freezing_trap        = find_class_spell( "Freezing Trap" );
  specs.arcane_shot          = find_class_spell( "Arcane Shot" );
  specs.steady_shot          = find_class_spell( "Steady Shot" );
  specs.steady_shot_energize = find_spell( 77443 );
  specs.flare                = find_class_spell( "Flare" );
  specs.call_pet             = find_spell( 883 );

  // Tier Sets
  tier_set.tww_s1_bm_2pc = sets -> set( HUNTER_BEAST_MASTERY, TWW1, B2 );
  tier_set.tww_s1_bm_4pc = sets -> set( HUNTER_BEAST_MASTERY, TWW1, B4 );
  tier_set.tww_s1_mm_2pc = sets -> set( HUNTER_MARKSMANSHIP, TWW1, B2 );
  tier_set.tww_s1_mm_4pc = sets -> set( HUNTER_MARKSMANSHIP, TWW1, B4 );
  tier_set.tww_s1_sv_2pc = sets -> set( HUNTER_SURVIVAL, TWW1, B2 );
  tier_set.tww_s1_sv_4pc = sets -> set( HUNTER_SURVIVAL, TWW1, B4 );

  tier_set.tww_s2_bm_2pc = sets -> set( HUNTER_BEAST_MASTERY, TWW2, B2 );
  tier_set.tww_s2_bm_4pc = sets -> set( HUNTER_BEAST_MASTERY, TWW2, B4 );
  tier_set.tww_s2_mm_2pc = sets -> set( HUNTER_MARKSMANSHIP, TWW2, B2 );
  tier_set.tww_s2_mm_4pc = sets -> set( HUNTER_MARKSMANSHIP, TWW2, B4 );
  tier_set.tww_s2_sv_2pc = sets -> set( HUNTER_SURVIVAL, TWW2, B2 );
  tier_set.tww_s2_sv_4pc = sets -> set( HUNTER_SURVIVAL, TWW2, B4 );

  // Cooldowns
  cooldowns.target_acquisition->duration = talents.target_acquisition->internal_cooldown();
  cooldowns.salvo->duration = talents.volley->duration();

  cooldowns.ruthless_marauder->duration = talents.ruthless_marauder->internal_cooldown();

  cooldowns.bleak_powder->duration = talents.bleak_powder->internal_cooldown();
  cooldowns.banshees_mark->duration = talents.banshees_mark->internal_cooldown();

  cooldowns.no_mercy->duration = talents.no_mercy->internal_cooldown();
}

void hunter_t::init_base_stats()
{
  if ( base.distance < 1 )
  {
    base.distance = 40;
    if ( specialization() == HUNTER_SURVIVAL )
      base.distance = 5;
  }

  player_t::init_base_stats();

  base.attack_power_per_strength = 0;
  base.attack_power_per_agility  = 1;
  base.spell_power_per_intellect = 1;

  resources.base_regen_per_second[ RESOURCE_FOCUS ] = 5;
  for ( auto spell : { specs.marksmanship_hunter, specs.survival_hunter, talents.pack_tactics } )
  {
    for ( const spelleffect_data_t& effect : spell -> effects() )
    {
      if ( effect.ok() && effect.type() == E_APPLY_AURA && effect.subtype() == A_MOD_POWER_REGEN_PERCENT )
        resources.base_regen_per_second[ RESOURCE_FOCUS ] *= 1 + effect.percent();
    }
  }

  resources.base[RESOURCE_FOCUS] = 100;
}

void hunter_t::create_actions()
{
  // Since without an apl defined use of Serpent Sting we can end up in a situation where no "serpent_sting" action is created 
  // for the "active_dot" expression to look up the dot by, force an action to be made here that may or may not go unused.
  get_background_action<attacks::serpent_sting_t>( "serpent_sting" );

  player_t::create_actions();

  if ( talents.dire_command.ok() )
    actions.dire_command = new spells::dire_command_summon_t( this );

  if ( talents.laceration.ok() )
    actions.laceration = new attacks::laceration_t( this );
  
  if ( talents.a_murder_of_crows.ok() || talents.banshees_mark.ok() )
    actions.a_murder_of_crows = new spells::a_murder_of_crows_t( this );

  if ( talents.howl_of_the_pack_leader.ok() )
    actions.boar_charge = new attacks::boar_charge_t( this );

  if ( talents.sentinel.ok() )
    actions.sentinel = new attacks::sentinel_t( this );

  if ( talents.symphonic_arsenal.ok() )
    actions.symphonic_arsenal = new attacks::symphonic_arsenal_t( this );

  if ( talents.lunar_storm.ok() )
  {
    actions.lunar_storm_initial = new attacks::lunar_storm_initial_t( this );
    actions.lunar_storm_periodic = new attacks::lunar_storm_periodic_t( this );
  }

  if ( talents.snakeskin_quiver.ok() )
    actions.snakeskin_quiver = new attacks::cobra_shot_snakeskin_quiver_t( this );

  if ( talents.phantom_pain.ok() )
    actions.phantom_pain = new attacks::phantom_pain_t( this );
}

void hunter_t::create_buffs()
{
  player_t::create_buffs();

  // Hunter Tree

  buffs.deathblow =
    make_buff( this, "deathblow", talents.deathblow_buff )
      ->set_activated( false );

  // Marksmanship Tree

  buffs.precise_shots = 
    make_buff( this, "precise_shots", talents.precise_shots_buff )
      ->set_default_value_from_effect( 1 )
      ->apply_affecting_aura( talents.windrunner_quiver );

  buffs.streamline =
    make_buff( this, "streamline", talents.streamline_buff )
      ->set_default_value( talents.streamline_buff->effectN( 1 ).percent() + talents.improved_streamline->effectN( 1 ).percent() );

  buffs.trick_shots =
    make_buff( this, "trick_shots", talents.trick_shots_buff );
  
  buffs.lock_and_load =
    make_buff( this, "lock_and_load", talents.lock_and_load -> effectN( 1 ).trigger() )
      ->set_trigger_spell( talents.lock_and_load );

  buffs.in_the_rhythm = 
    make_buff( this, "in_the_rhythm", talents.in_the_rhythm_buff )
      ->set_default_value( talents.in_the_rhythm_buff->effectN( 1 ).base_value() );

  buffs.on_target =
    make_buff( this, "on_target", talents.on_target_buff )
      ->set_default_value_from_effect( 1 )
      ->set_pct_buff_type( STAT_PCT_BUFF_HASTE )
      ->set_stack_behavior( buff_stack_behavior::ASYNCHRONOUS );

  buffs.trueshot =
    make_buff( this, "trueshot", talents.trueshot )
      ->set_cooldown( 0_s )
      ->set_refresh_behavior( buff_refresh_behavior::EXTEND )
      ->add_invalidate( cache_e::CACHE_CRIT_CHANCE )
      ->set_stack_change_callback(
        [ this ]( buff_t*, int, int cur ) {
          cooldowns.aimed_shot->adjust_recharge_multiplier();
          cooldowns.rapid_fire->adjust_recharge_multiplier();
          if ( cur == 0 ) 
            state.tensile_bowstring_extension = 0_s;
        } );

  buffs.moving_target =
    make_buff( this, "moving_target", talents.moving_target_buff )
      ->set_default_value_from_effect( 1 );

  buffs.precision_detonation_hidden =
    make_buff( this, "precision_detonation", talents.precision_detonation_buff )
      ->set_default_value_from_effect( 1 )
      ->set_quiet( true );

  buffs.razor_fragments =
    make_buff( this, "razor_fragments", talents.razor_fragments_buff )
      ->set_default_value_from_effect( 1 )
      ->set_activated( false );

  buffs.bullseye =
    make_buff( this, "bullseye", talents.bullseye_buff )
      ->set_default_value_from_effect( 1 )
      ->set_max_stack( std::max( as<int>( talents.bullseye -> effectN( 2 ).base_value() ), 1 ) )
      ->set_chance( talents.bullseye.ok() );

  buffs.bulletstorm =
    make_buff( this, "bulletstorm", talents.bulletstorm_buff )
      ->set_default_value_from_effect( 1 )
      ->set_refresh_behavior( buff_refresh_behavior::DISABLED )
      ->modify_max_stack( as<int>( talents.incendiary_ammunition->effectN( 2 ).base_value() ) );

  buffs.double_tap =
    make_buff( this, "double_tap", talents.double_tap_buff )
      ->set_default_value_from_effect( 1 );

  buffs.volley =
    make_buff( this, "volley", talents.volley_data )
      -> set_cooldown( 0_ms )
      -> set_period( 0_ms ) // disable ticks as an optimization
      -> set_refresh_behavior( buff_refresh_behavior::DURATION );

  // Beast Mastery Tree

  const spell_data_t* barbed_shot = find_spell( 246152 );
  for ( size_t i = 0; i < buffs.barbed_shot.size(); i++ )
  {
    buffs.barbed_shot[ i ] =
      make_buff( this, fmt::format( "barbed_shot_{}", i + 1 ), barbed_shot )
        -> set_default_value( barbed_shot -> effectN( 1 ).resource( RESOURCE_FOCUS ) )
        -> set_tick_callback(
          [ this ]( buff_t* b, int, timespan_t ) {
            resource_gain( RESOURCE_FOCUS, b -> default_value, gains.barbed_shot, actions.barbed_shot );
          } );
  }

  buffs.thrill_of_the_hunt =
    make_buff( this, "thrill_of_the_hunt", talents.thrill_of_the_hunt -> effectN( 1 ).trigger() )
      -> apply_affecting_aura( talents.savagery )
      -> set_default_value_from_effect( 1 )
      -> set_max_stack( std::max( 1, as<int>( talents.thrill_of_the_hunt -> effectN( 2 ).base_value() ) ) )
      -> set_trigger_spell( talents.thrill_of_the_hunt );

  buffs.dire_beast =
    make_buff( this, "dire_beast", find_spell( 120679 ) -> effectN( 2 ).trigger() )
      -> modify_duration( talents.dire_frenzy -> effectN( 1 ).time_value() )
      -> set_default_value_from_effect( 1 )
      -> set_pct_buff_type( STAT_PCT_BUFF_HASTE );

  buffs.bestial_wrath =
    make_buff( this, "bestial_wrath", talents.bestial_wrath )
      -> set_cooldown( 0_ms )
      -> set_default_value_from_effect( 1 );

  buffs.call_of_the_wild =
    make_buff( this, "call_of_the_wild", talents.call_of_the_wild )
      -> set_cooldown( 0_ms )
      -> set_tick_callback(
        [ this ]( buff_t*, int, timespan_t ) {
          pets.cotw_stable_pet.spawn( talents.call_of_the_wild -> effectN( 2 ).trigger() -> duration(), 1 );

          double percent_reduction = talents.call_of_the_wild -> effectN( 3 ).base_value() / 100.0; 
          cooldowns.kill_command -> adjust( -( cooldowns.kill_command -> duration * percent_reduction ) );
          cooldowns.barbed_shot -> adjust( -( cooldowns.barbed_shot -> duration * percent_reduction ) );

          if ( talents.bloody_frenzy.ok() )
          {
            //In-game this (re)application of beast_cleave happens multiple times a second, since the regular Beast Cleave buff is longer than the time between ticks, we can get by with just refreshing once per tick
            //In 11.0 it has been changed to use the player's remaining duration of beast_cleave, instead of the remaining duration of call_of_the_wild - this change is to support covering_fire functionality
            timespan_t duration = buffs.beast_cleave -> remains();
            for ( auto pet : pets::active<pets::stable_pet_t>( pets.main, pets.animal_companion ) )
            {
              pet -> actions.stomp -> execute();
              if ( duration > 0_ms )
              {
                pet -> buffs.beast_cleave -> trigger( duration );
              }
            }
            for ( auto pet : ( pets.cotw_stable_pet.active_pets() ) )
            {
              pet -> actions.stomp -> execute();
              if ( duration > 0_ms )
              {
                pet -> buffs.beast_cleave -> trigger( duration );
              }
            }
          }
        } );

  buffs.beast_cleave = 
    make_buff( this, "beast_cleave", find_spell( 268877 ) )
    -> apply_affecting_effect( talents.beast_cleave -> effectN( 2 ) );

  buffs.serpentine_rhythm = 
    make_buff( this, "serpentine_rhythm", find_spell( 468703 ) )
    -> set_default_value_from_effect( 1 )
    -> set_chance( talents.serpentine_rhythm.ok() );

  buffs.serpentine_blessing = 
    make_buff( this, "serpentine_blessing", find_spell( 468704 ) )
    -> set_default_value_from_effect( 1 )
    -> set_chance( talents.serpentine_rhythm.ok() );

  buffs.a_murder_of_crows = 
    make_buff( this, "a_murder_of_crows", talents.a_murder_of_crows->effectN( 1 ).trigger() );

  buffs.huntmasters_call = 
    make_buff( this, "huntmasters_call", find_spell( 459731 ) );

  buffs.summon_fenryr = 
    make_buff( this, "summon_fenryr", find_spell ( 459735 ) )
    -> modify_duration( talents.dire_frenzy -> effectN( 1 ).time_value() )
    -> set_default_value_from_effect( 2 )
    -> set_pct_buff_type( STAT_PCT_BUFF_HASTE );

  buffs.summon_hati = 
    make_buff( this, "summon_hati", find_spell( 459738 ) )
      -> modify_duration( talents.dire_frenzy -> effectN( 1 ).time_value() )
      -> set_default_value_from_effect( 2 );

  // Survival Tree

  buffs.tip_of_the_spear =
    make_buff( this, "tip_of_the_spear", talents.tip_of_the_spear_buff )
      ->set_chance( talents.tip_of_the_spear.ok() );

  buffs.tip_of_the_spear_explosive =
    make_buff( this, "tip_of_the_spear_explosive", talents.tip_of_the_spear_explosive_buff )
      ->set_chance( talents.tip_of_the_spear.ok() );

  buffs.tip_of_the_spear_fote =
    make_buff( this, "tip_of_the_spear_fote", talents.tip_of_the_spear_fote_buff )
      ->set_chance( talents.tip_of_the_spear.ok() );
  
  buffs.mongoose_fury =
    make_buff( this, "mongoose_fury", talents.mongoose_fury )
      ->set_default_value_from_effect( 1 )
      ->set_refresh_behavior( buff_refresh_behavior::DISABLED );
  
  buffs.wildfire_arsenal =
    make_buff( this, "wildfire_arsenal", talents.wildfire_arsenal_buff )
      ->set_default_value_from_effect( 1 );

  buffs.frenzy_strikes =
    make_buff( this, "frenzy_strikes", talents.frenzy_strikes_buff )
      ->set_default_value_from_effect( 1 )
      ->add_invalidate( CACHE_AUTO_ATTACK_SPEED );

  buffs.bloodseeker =
    make_buff( this, "bloodseeker", find_spell( 260249 ) )
      -> set_default_value_from_effect( 1 )
      -> add_invalidate( CACHE_AUTO_ATTACK_SPEED );

  buffs.sulfurlined_pockets_building =
    make_buff( this, "sulfur_lined_pockets_building", talents.sulfurlined_pockets_building_buff );

  buffs.sulfurlined_pockets_ready =
    make_buff( this, "sulfur_lined_pockets", talents.sulfurlined_pockets_ready_buff );

  buffs.terms_of_engagement =
    make_buff( this, "terms_of_engagement", talents.terms_of_engagement_buff )
      -> set_default_value_from_effect( 1, 1 / 5.0 )
      -> set_affects_regen( true );

  buffs.aspect_of_the_eagle =
    make_buff( this, "aspect_of_the_eagle", find_spell( 186289 ) )
      -> set_cooldown( 0_ms );

  buffs.coordinated_assault =
    make_buff( this, "coordinated_assault", talents.coordinated_assault )
      ->set_default_value( talents.coordinated_assault->effectN( 1 ).percent() )
      ->set_cooldown( 0_ms )
      ->set_stack_change_callback( [ this ]( buff_t*, int, int cur ) {
        if ( cur == 0 )
        {
          buffs.relentless_primal_ferocity->expire();

          if ( talents.bombardier.ok() )
          {
            buffs.bombardier->trigger();
            cooldowns.explosive_shot->reset( true );
          }
        }
      } );

  buffs.ruthless_marauder =
    make_buff( this, "ruthless_marauder", talents.ruthless_marauder_buff )
      ->set_pct_buff_type( STAT_PCT_BUFF_HASTE )
      ->set_default_value_from_effect( 1 );

  buffs.relentless_primal_ferocity =
    make_buff( this, "relentless_primal_ferocity", talents.relentless_primal_ferocity_buff )
      ->set_default_value_from_effect( 1 )
      ->set_pct_buff_type( STAT_PCT_BUFF_HASTE )
      ->set_quiet( true );

  buffs.bombardier = 
    make_buff( this, "bombardier", talents.bombardier_buff );

  // Pet family buffs

  buffs.endurance_training =
    make_buff( this, "endurance_training", find_spell( 264662 ) )
      -> set_default_value_from_effect( 2 )
      -> apply_affecting_aura( talents.aspect_of_the_beast )
      -> set_stack_change_callback(
          []( buff_t* b, int old, int cur ) {
            player_t* p = b -> player;
            if ( cur == 0 )
              p -> resources.initial_multiplier[ RESOURCE_HEALTH ] /= 1 + b -> default_value;
            else if ( old == 0 )
              p -> resources.initial_multiplier[ RESOURCE_HEALTH ] *= 1 + b -> default_value;
            p -> recalculate_resource_max( RESOURCE_HEALTH );
          } );

  buffs.pathfinding =
    make_buff( this, "pathfinding", find_spell( 264656 ) )
      -> set_default_value_from_effect( 2 )
      -> apply_affecting_aura( talents.aspect_of_the_beast )
      -> add_invalidate( CACHE_RUN_SPEED );

  buffs.predators_thirst =
    make_buff( this, "predators_thirst", find_spell( 264663 ) )
      -> set_default_value_from_effect( 2 )
      -> apply_affecting_aura( talents.aspect_of_the_beast )
      -> add_invalidate( CACHE_LEECH );

  // Tier Set Bonuses

  buffs.harmonize =
    make_buff( this, "harmonize", find_spell( 457072 ) )
      -> set_default_value_from_effect( 1 );

  buffs.jackpot
    = make_buff( this, "jackpot", tier_set.tww_s2_mm_2pc->effectN( 2 ).trigger() )
      ->set_default_value_from_effect( 1 );

  buffs.winning_streak = 
    make_buff( this, "winning_streak", tier_set.tww_s2_sv_2pc->effectN( 1 ).trigger() )
      ->set_default_value_from_effect( 1 ) // Damage increase per stack to wildfire bomb
      ->set_chance( 1.0 );

  buffs.strike_it_rich = 
    make_buff( this, "strike_it_rich", find_spell( 1216879 ) ) 
      ->set_default_value_from_effect( 1 ); // Damage increase to mongoose/raptor strike

  // Hero Talents

  buffs.howl_of_the_pack_leader_wyvern = 
    make_buff( this, "howl_of_the_pack_leader_wyvern", talents.howl_of_the_pack_leader_wyvern_ready_buff )
      ->set_stack_change_callback(
        [ this ]( buff_t* b, int, int cur ) {
          if ( cur == 0 && !buffs.howl_of_the_pack_leader_cooldown->check() )
            buffs.howl_of_the_pack_leader_cooldown->trigger();
        } );

  buffs.howl_of_the_pack_leader_boar = 
    make_buff( this, "howl_of_the_pack_leader_boar", talents.howl_of_the_pack_leader_boar_ready_buff )
      ->set_stack_change_callback(
        [ this ]( buff_t* b, int, int cur ) {
          if ( cur == 0 && !buffs.howl_of_the_pack_leader_cooldown->check() )
            buffs.howl_of_the_pack_leader_cooldown->trigger();
        } );

  buffs.howl_of_the_pack_leader_bear = 
    make_buff( this, "howl_of_the_pack_leader_bear", talents.howl_of_the_pack_leader_bear_ready_buff )
      ->set_stack_change_callback(
        [ this ]( buff_t* b, int, int cur ) {
          if ( cur == 0 && !buffs.howl_of_the_pack_leader_cooldown->check() )
            buffs.howl_of_the_pack_leader_cooldown->trigger();
        } );

  buffs.howl_of_the_pack_leader_cooldown = 
    make_buff( this, "howl_of_the_pack_leader_cooldown", talents.howl_of_the_pack_leader_cooldown_buff )
      ->apply_affecting_aura( talents.better_together )
      ->set_stack_change_callback(
        [ this ]( buff_t* b, int, int cur ) {
          if ( cur == 0 )
            trigger_howl_of_the_pack_leader();
        } );

  buffs.wyverns_cry = 
    make_buff( this, "wyverns_cry", talents.howl_of_the_pack_leader_wyvern_buff )
      ->set_default_value_from_effect( 1 )
      ->set_stack_change_callback(
        [ this ]( buff_t* b, int, int cur ) {
          if ( cur == 0 )
            state.fury_of_the_wyvern_extension = 0_s;
        } );

  buffs.hogstrider =
    make_buff( this, "hogstrider", talents.hogstrider_buff )
      ->set_default_value_from_effect( 1 );
  
  buffs.lead_from_the_front =
    make_buff( this, "lead_from_the_front", talents.lead_from_the_front_buff )
      ->set_default_value_from_effect( specialization() == HUNTER_BEAST_MASTERY ? 4 : 5 );

  buffs.eyes_closed = make_buff( this, "eyes_closed", talents.eyes_closed->effectN( 1 ).trigger() );

  buffs.lunar_storm_ready = make_buff( this, "lunar_storm_ready", talents.lunar_storm_ready_buff );
  
  buffs.lunar_storm_cooldown = make_buff( this, "lunar_storm_cooldown", talents.lunar_storm_cooldown_buff )
    ->set_stack_change_callback(
      [ this ]( buff_t* b, int, int cur ) {
        if ( cur == 0 )
          buffs.lunar_storm_ready->trigger();
      } );

  buffs.withering_fire =
    make_buff( this, "withering_fire", talents.withering_fire_buff );

  if ( specialization() == HUNTER_BEAST_MASTERY )
    buffs.withering_fire->set_tick_callback( [ this ]( buff_t*, int, timespan_t ) { trigger_deathblow(); } );
}

void hunter_t::init_gains()
{
  player_t::init_gains();

  gains.barbed_shot               = get_gain( "Barbed Shot" );
  gains.dire_beast                = get_gain( "Dire Beast" );

  gains.terms_of_engagement       = get_gain( "Terms of Engagement" );

  gains.invigorating_pulse        = get_gain( "Invigorating Pulse" );
}

void hunter_t::init_position()
{
  player_t::init_position();

  if ( specialization() == HUNTER_SURVIVAL )
  {
    base.position = POSITION_BACK;
    position_str = util::position_type_string( base.position );
  }
  else
  {
    if ( base.position == POSITION_FRONT )
    {
      base.position = POSITION_RANGED_FRONT;
      position_str = util::position_type_string( base.position );
    }
    else if ( initial.position == POSITION_BACK )
    {
      base.position = POSITION_RANGED_BACK;
      position_str = util::position_type_string( base.position );
    }
  }

  sim -> print_debug( "{}: Position adjusted to {}", name(), position_str );
}

void hunter_t::init_procs()
{
  player_t::init_procs();

  if ( talents.dire_command.ok() )
    procs.dire_command = get_proc( "Dire Command" );

  if ( talents.snakeskin_quiver.ok() )
    procs.snakeskin_quiver = get_proc( "Snakeskin Quiver" );

  if ( talents.wild_call.ok() )
    procs.wild_call = get_proc( "Wild Call" );
  
  if ( talents.deathblow.ok() )
    procs.deathblow = get_proc( "Deathblow" );

  if ( talents.precision_detonation.ok() )
    procs.precision_detonation = get_proc( "Precision Detonations" );

  if ( tier_set.tww_s2_mm_4pc.ok() )
    procs.tww_s2_mm_4pc_explosive = get_proc( "TWW S2 MM 4pc Explosive" );

  if ( talents.sentinel.ok() )
  {
    procs.sentinel_stacks = get_proc( "Sentinel Stacks" );
    procs.sentinel_implosions = get_proc( "Sentinel Implosions" );
  }

  if ( talents.extrapolated_shots.ok() )
    procs.extrapolated_shots_stacks = get_proc( "Extrapolated Shots Stacks" );

  if ( talents.release_and_reload.ok() )
    procs.release_and_reload_stacks = get_proc( "Release and Reload Stacks" );

  if ( talents.crescent_steel.ok() )
    procs.crescent_steel_stacks = get_proc( "Crescent Steel Stacks" );

  if ( talents.overwatch.ok() )
    procs.overwatch_implosions = get_proc( "Overwatch Implosion" );
}

void hunter_t::init_rng()
{
  player_t::init_rng();
  
  rppm.shadow_hounds = get_rppm( "Shadow Hounds", talents.shadow_hounds );
  rppm.shadow_surge = get_rppm( "Shadow Surge", talents.shadow_surge );

}

void hunter_t::init_scaling()
{
  player_t::init_scaling();

  scaling -> disable( STAT_STRENGTH );
}

void hunter_t::init_assessors()
{
  player_t::init_assessors();

  if ( talents.terms_of_engagement.ok() )
    assessor_out_damage.add( assessor::TARGET_DAMAGE - 1, [this]( result_amount_type, action_state_t* s ) {
      if ( s -> result_amount > 0 )
        get_target_data( s -> target ) -> damaged = true;
      return assessor::CONTINUE;
    } );

  if ( talents.overwatch.ok() )
    assessor_out_damage.add( assessor::TARGET_DAMAGE + 1, [ this ]( result_amount_type, action_state_t* s ) {
      hunter_td_t* target_data = get_target_data( s->target );
      if ( !target_data->sentinel_imploding && target_data->debuffs.sentinel->check() > 3 && s->target->health_percentage() < talents.overwatch->effectN( 1 ).base_value() )
      {
        for ( player_t* t : sim->target_non_sleeping_list )
        {
          if ( t->is_enemy() && !t->demise_event )
          {
            hunter_td_t* td = get_target_data( t );
            if ( !td->sentinel_imploding && td->cooldowns.overwatch->up() )
            {
              sim->print_debug( "Damage to {} with {} Sentinel stacks at {}% triggers Overwatch on {}", s->target->name(),
                          target_data->debuffs.sentinel->check(), s->target->health_percentage(), t->name() );

              procs.overwatch_implosions->occur();
              trigger_sentinel_implosion( td );
              td->cooldowns.overwatch->start();
            }
          }
        }
      }
      return assessor::CONTINUE;
    } );

  if ( talents.crescent_steel.ok() )
    assessor_out_damage.add( assessor::TARGET_DAMAGE + 1, [ this ]( result_amount_type, action_state_t* s ) {
      if ( s->target->health_percentage() < talents.crescent_steel->effectN( 1 ).base_value() )
        get_target_data( s->target )->debuffs.crescent_steel->trigger();
      return assessor::CONTINUE;
    } );
}

void hunter_t::apply_affecting_auras( action_t& action )
{
  player_t::apply_affecting_auras(action);

  action.apply_affecting_aura( specs.hunter );
  action.apply_affecting_aura( specs.beast_mastery_hunter );
  action.apply_affecting_aura( specs.marksmanship_hunter );
  action.apply_affecting_aura( specs.survival_hunter );
}

void hunter_t::init_action_list()
{
  if ( main_hand_weapon.group() == WEAPON_RANGED )
  {
    const weapon_e type = main_hand_weapon.type;
    if ( type != WEAPON_BOW && type != WEAPON_CROSSBOW && type != WEAPON_GUN )
    {
      sim -> error( "Player {} does not have a proper weapon type at the Main Hand slot: {}.",
                    name(), util::weapon_subclass_string( items[ main_hand_weapon.slot ].parsed.data.item_subclass ) );
      if ( specialization() != HUNTER_SURVIVAL )
        sim -> cancel();
    }
  }

  if ( specialization() == HUNTER_SURVIVAL && main_hand_weapon.group() != WEAPON_2H )
  {
    sim -> error( "Player {} does not have a proper weapon at the Main Hand slot: {}.",
                  name(), main_hand_weapon.type );
  }

  if ( action_list_str.empty() )
  {
    clear_action_priority_lists();

    switch ( specialization() )
    {
    case HUNTER_BEAST_MASTERY:
      if ( is_ptr() )
        hunter_apl::beast_mastery_ptr( this );
      else
        hunter_apl::beast_mastery( this );
      break;
    case HUNTER_MARKSMANSHIP:
      if ( is_ptr() )
        hunter_apl::marksmanship_ptr( this );
      else
        hunter_apl::marksmanship( this );
      break;
    case HUNTER_SURVIVAL:
      if ( is_ptr() )
        hunter_apl::survival_ptr( this );
      else
        hunter_apl::survival( this );
      break;
    default:
      get_action_priority_list( "default" ) -> add_action( "arcane_shot" );
      break;
    }

    use_default_action_list = true;
  }

  player_t::init_action_list();
}

void hunter_t::init_special_effects()
{
  player_t::init_special_effects();

  if ( talents.bullseye.ok() )
  {
    struct bullseye_cb_t : public dbc_proc_callback_t
    {
      double threshold;

      bullseye_cb_t( const special_effect_t& e, double threshold ) : dbc_proc_callback_t( e.player, e ),
        threshold( threshold )
      {
      }

      void trigger( action_t* a, action_state_t* state ) override
      {
        if ( state -> target -> health_percentage() >= threshold )
          return;

        dbc_proc_callback_t::trigger( a, state );
      }
    };

    auto const effect = new special_effect_t( this );
    effect -> name_str = "bullseye";
    effect -> spell_id = talents.bullseye -> id();
    effect -> custom_buff = buffs.bullseye;
    effect -> proc_flags2_ = PF2_ALL_HIT;
    special_effects.push_back( effect );

    auto cb = new bullseye_cb_t( *effect, talents.bullseye -> effectN( 1 ).base_value() );
    cb -> initialize();
  }

  if ( talents.master_marksman.ok() )
  {
    struct master_marksman_cb_t : public dbc_proc_callback_t
    {
      double bleed_amount;
      action_t* bleed;

      master_marksman_cb_t( const special_effect_t& e, double amount, action_t* bleed ) : dbc_proc_callback_t( e.player, e ),
        bleed_amount( amount ), bleed( bleed )
      {
      }

      void execute( action_t* a, action_state_t* s ) override
      {
        dbc_proc_callback_t::execute( a, s );

        double amount = s -> result_amount * bleed_amount;
        if ( amount > 0 )
          residual_action::trigger( bleed, s -> target, amount );
      }
    };

    auto const effect = new special_effect_t( this );
    effect -> name_str = "master_marksman";
    effect -> spell_id = talents.master_marksman -> id();
    effect -> proc_flags2_ = PF2_CRIT;
    special_effects.push_back( effect );

    auto cb = new master_marksman_cb_t( *effect, talents.master_marksman -> effectN( 1 ).percent(), new attacks::master_marksman_t( this ) );
    cb -> initialize();
  }

  if ( talents.sentinel.ok() )
  {
    struct sentinel_cb_t : public dbc_proc_callback_t
    {
      hunter_t* player;

      sentinel_cb_t( const special_effect_t& e, hunter_t* p )
        : dbc_proc_callback_t( p, e ), player( p )
      {
      }

      void execute( action_t* a, action_state_t* s ) override
      {
        dbc_proc_callback_t::execute( a, s );

        player->trigger_sentinel( s->target, false, player->procs.sentinel_stacks );
      }
    };

    auto const effect    = new special_effect_t( this );
    effect->name_str     = "sentinel";
    effect->spell_id     = talents.sentinel->id();
    effect->proc_flags2_ = PF2_ALL_HIT;
    special_effects.push_back( effect );

    auto cb = new sentinel_cb_t( *effect, this );
    cb->initialize();
  }

  if ( tier_set.tww_s2_bm_2pc.ok() )
  {
    struct jackpot_cb_t : public dbc_proc_callback_t
    {
      hunter_t* player;
      action_t* barbed_shot;

      jackpot_cb_t( const special_effect_t& e, hunter_t* p ) : dbc_proc_callback_t( p, e ), 
        player( p ), 
        barbed_shot( p->get_background_action<attacks::barbed_shot_tww_s2_bm_2pc_t>( "barbed_shot_tww_s2_bm_2pc" ) )
      {
      }

      void execute( action_t* a, action_state_t* s ) override
      {
        dbc_proc_callback_t::execute( a, s );

        if ( barbed_shot && player->talents.barbed_shot.ok() )
          barbed_shot->execute_on_target( s->target );

        if ( player->tier_set.tww_s2_bm_4pc.ok())
          player->pets.main->buffs.potent_mutagen->trigger();
      }
    };

    auto const effect = new special_effect_t( this );
    effect->name_str = "jackpot";
    effect->spell_id = tier_set.tww_s2_bm_2pc->id();
    effect->proc_flags2_ = PF2_ALL_HIT;
    special_effects.push_back( effect );

    auto cb = new jackpot_cb_t( *effect, this );
    cb->initialize();
  }

  if ( tier_set.tww_s2_mm_2pc.ok() )
  {
    auto const effect = new special_effect_t( this );
    effect->name_str = "jackpot";
    effect->spell_id = tier_set.tww_s2_mm_2pc->id();
    effect->custom_buff = buffs.jackpot;
    effect->proc_flags2_ = PF2_ALL_HIT;
    special_effects.push_back( effect );

    auto cb = new dbc_proc_callback_t( this, *effect );
    cb->initialize();
  }

  if ( tier_set.tww_s2_sv_2pc.ok() )
  {
    auto const effect = new special_effect_t( this );
    effect->name_str = "winning_streak";
    effect->spell_id = tier_set.tww_s2_sv_2pc->id();
    effect->custom_buff = buffs.winning_streak;
    effect->proc_flags2_ = PF2_LANDED;
    special_effects.push_back( effect );

    auto cb = new dbc_proc_callback_t( this, *effect );
    cb->initialize();
  }
}

void hunter_t::init_finished()
{
  player_t::init_finished();
}

void hunter_t::reset()
{
  player_t::reset();

  // Active
  pets.main = nullptr;
  state = {};
}

void hunter_t::merge( player_t& other )
{
  player_t::merge( other );

  cd_waste.merge( static_cast<hunter_t&>( other ).cd_waste );
}

void hunter_t::arise()
{
  player_t::arise();
}

void hunter_t::combat_begin()
{
  if ( talents.bloodseeker.ok() && sim -> player_no_pet_list.size() > 1 )
  {
    make_repeating_event( *sim, 1_s, [ this ] { trigger_bloodseeker_update(); } );
  }

  if ( talents.outland_venom.ok() )
    make_repeating_event( *sim, talents.outland_venom_debuff->effectN( 2 ).period(),
                          [ this ] { trigger_outland_venom_update(); } );

  buffs.lunar_storm_ready->trigger();

  buffs.howl_of_the_pack_leader_cooldown->trigger();

  player_t::combat_begin();
}

void hunter_t::datacollection_begin()
{
  if ( active_during_iteration )
    cd_waste.datacollection_begin();

  player_t::datacollection_begin();
}

void hunter_t::datacollection_end()
{
  if ( requires_data_collection() )
    cd_waste.datacollection_end();

  player_t::datacollection_end();
}

double hunter_t::composite_melee_crit_chance() const
{
  double crit = player_t::composite_melee_crit_chance();

  crit += specs.critical_strikes -> effectN( 1 ).percent();
  crit += talents.keen_eyesight -> effectN( 1 ).percent();

  if ( buffs.trueshot->check() )
    crit += talents.trueshot->effectN( 4 ).percent() + talents.unerring_vision->effectN( 1 ).percent();

  return crit;
}

double hunter_t::composite_spell_crit_chance() const
{
  double crit = player_t::composite_spell_crit_chance();

  crit += specs.critical_strikes->effectN( 1 ).percent();
  crit += talents.keen_eyesight->effectN( 1 ).percent();

  if ( buffs.trueshot->check() )
    crit += talents.trueshot->effectN( 4 ).percent() + talents.unerring_vision->effectN( 1 ).percent();

  return crit;
}

double hunter_t::composite_rating_multiplier( rating_e r ) const
{
  double rm = player_t::composite_rating_multiplier( r );

  switch ( r )
  {
    case RATING_MELEE_CRIT:
    case RATING_RANGED_CRIT:
    case RATING_SPELL_CRIT:
      rm *= 1.0 + talents.serrated_tips -> effectN( 1 ).percent();
      break;
    default:
      break;
  }

  return rm;
}

double hunter_t::composite_melee_auto_attack_speed() const
{
  double s = player_t::composite_melee_auto_attack_speed();

  if ( buffs.bloodseeker -> check() )
    s /= 1 + buffs.bloodseeker -> check_stack_value();

  if ( talents.trigger_finger->ok() )
  {
    // TODO 23/1/25: Trigger Finger is maybe applying another attack speed mod from the effect 2 script separately (multiplicative), so there is always double the expected amount
    // only one effect is doubled while petless, so mm sees triple the expected amount
    s /= 1 + talents.trigger_finger->effectN( 1 ).percent();

    if ( pets.main )
      s /= 1 + talents.trigger_finger->effectN( 2 ).percent();
    else
      s /= 1 + talents.trigger_finger->effectN( 2 ).percent() * ( 1 + talents.trigger_finger->effectN( 3 ).percent() );
  }

  s /= 1 + buffs.frenzy_strikes->check_value();
  
  return s;
}

double hunter_t::composite_player_critical_damage_multiplier( const action_state_t* s ) const
{
  double m = player_t::composite_player_critical_damage_multiplier( s );

  if ( talents.penetrating_shots -> effectN( 1 ).has_common_school( s -> action -> school ) )
    m *= 1.0 + talents.penetrating_shots -> effectN( 2 ).percent() * cache.attack_crit_chance();

  return m;
}

double hunter_t::composite_player_multiplier( school_e school ) const
{
  double m = player_t::composite_player_multiplier( school );

  return m;
}

double hunter_t::composite_player_target_multiplier( player_t* target, school_e school ) const
{
  double d = player_t::composite_player_target_multiplier( target, school );

  auto td = get_target_data( target );

  if ( td->debuffs.kill_zone->has_common_school( school ) )
    d *= 1 + td->debuffs.kill_zone->check_value();

  if ( td->debuffs.lunar_storm->has_common_school( school ) )
    d *= 1 + td->debuffs.lunar_storm->check_value();

  return d;
}

double hunter_t::composite_player_pet_damage_multiplier( const action_state_t* s, bool guardian ) const
{
  double m = player_t::composite_player_pet_damage_multiplier( s, guardian );

  if ( mastery.spirit_bond->ok() )
    m *= 1.0 + cache.mastery_value() * ( 1 + mastery.spirit_bond_buff->effectN( 1 ).percent() );

  if ( mastery.master_of_beasts->ok() )
    m *= 1.0 + cache.mastery_value();

  m *= 1 + specs.beast_mastery_hunter -> effectN( guardian ? 6 : 3 ).percent();
  m *= 1 + specs.survival_hunter -> effectN( guardian ? 7 : 3 ).percent();
  m *= 1 + specs.marksmanship_hunter -> effectN( guardian ? 7 : 3 ).percent();

  if ( !guardian )
  {
    if ( buffs.coordinated_assault->check() )
      m *= 1 + talents.coordinated_assault->effectN( 4 ).percent();
    
    m *= 1 + talents.training_expert->effectN( 1 ).percent();

    m *= 1 + buffs.summon_hati->check_value();
    m *= 1 + buffs.serpentine_blessing->check_value();
    m *= 1 + buffs.wyverns_cry->check_stack_value();
    m *= 1 + buffs.lead_from_the_front->check_value();
    m *= 1 + buffs.harmonize->check_value();
  }

  return m;
}

double hunter_t::composite_player_target_pet_damage_multiplier( player_t* target, bool guardian ) const
{
  double m = player_t::composite_player_target_pet_damage_multiplier( target, guardian );

  auto td = get_target_data( target );

  if ( td->debuffs.kill_zone->check() )
    m *= 1 + talents.kill_zone_debuff->effectN( guardian ? 4 : 3 ).percent();

  if ( td->debuffs.lunar_storm->check() )
    m *= 1 + talents.lunar_storm_periodic_spell->effectN( 2 ).trigger()->effectN( guardian ? 3 : 2 ).percent();

  if ( !guardian )
    m *= 1 + td->debuffs.wild_instincts->check_stack_value();

  return m;
}

double hunter_t::composite_leech() const
{
  double l = player_t::composite_leech();

  l += buffs.predators_thirst -> check_value();

  return l;
}

void hunter_t::invalidate_cache( cache_e c )
{
  player_t::invalidate_cache( c );

  switch ( c )
  {
  case CACHE_MASTERY:
    if ( sim -> distance_targeting_enabled && mastery.sniper_training.ok() )
    {
      // Marksman is a unique butterfly, since mastery changes the max range of abilities.
      // We need to regenerate every target cache.
      // XXX: Do we? We don't change action range anywhere.
      for ( action_t* action : action_list )
        action -> target_cache.is_valid = false;
    }
    break;
  default: break;
  }
}

void hunter_t::regen( timespan_t periodicity )
{
  player_t::regen( periodicity );

  if ( resources.is_infinite( RESOURCE_FOCUS ) )
    return;

  if ( buffs.terms_of_engagement -> check() )
    resource_gain( RESOURCE_FOCUS, buffs.terms_of_engagement -> check_value() * periodicity.total_seconds(), gains.terms_of_engagement );
}

double hunter_t::resource_gain( resource_e type, double amount, gain_t* g, action_t* action )
{
  double actual_amount = player_t::resource_gain( type, amount, g, action );

  if ( action && type == RESOURCE_FOCUS && amount > 0 )
  {
    /**
     * If the gain event has an action specified we treat it as an "energize" effect.
     * Focus energize effects are a bit special in that they can grant only integral amounts
     * of focus flooring the total calculated amount.
     * That means we can't just simply multiply stuff and trigger gains in the presence of non-integral
     * mutipliers. Which Trueshot is, at 50%. We have to calculate the fully multiplied value, floor
     * that and distribute the amounts & gains accordingly.
     * To keep gains attribution "fair" we distribute the additional gain to all of the present
     * multipliers according to their "weight".
     */

    assert( g != player_t::gains.resource_regen[ type ] );

    std::array<std::pair<double, gain_t*>, 3> mul_gains;
    size_t mul_gains_count = 0;
    double mul_sum = 0;

    const double initial_amount = floor( amount );
    amount = initial_amount;

    const double additional_amount = floor( amount ) - initial_amount;
    if ( additional_amount > 0 )
    {
      for ( const auto& data : util::make_span( mul_gains ).subspan( 0, mul_gains_count ) )
        actual_amount += player_t::resource_gain( RESOURCE_FOCUS, additional_amount * ( data.first / mul_sum ), data.second, action );
    }
  }

  return actual_amount;
}

double hunter_t::matching_gear_multiplier( attribute_e attr ) const
{
  if ( attr == ATTR_AGILITY )
    return 0.05;

  return 0;
}

double hunter_t::stacking_movement_modifier() const
{
  double ms = player_t::stacking_movement_modifier();

  ms += buffs.pathfinding -> check_value();

  return ms;
}

void hunter_t::create_options()
{
  player_t::create_options();

  add_option( opt_string( "summon_pet", options.summon_pet_str ) );
  add_option( opt_timespan( "hunter.pet_attack_speed", options.pet_attack_speed,
                            0.5_s, 4_s ) );
  add_option( opt_timespan( "hunter.pet_basic_attack_delay", options.pet_basic_attack_delay,
                            0_ms, 0.6_s ) );
}

std::string hunter_t::create_profile( save_e stype )
{
  std::string profile_str = player_t::create_profile( stype );

  const options_t defaults{};
  auto print_option = [&] ( auto ref, util::string_view name ) {
    if ( std::invoke( ref, options ) != std::invoke( ref, defaults ) )
      fmt::format_to( std::back_inserter( profile_str ), "{}={}\n", name, std::invoke( ref, options ) );
  };

  print_option( &options_t::summon_pet_str, "summon_pet" );
  print_option( &options_t::pet_attack_speed, "hunter.pet_attack_speed" );
  print_option( &options_t::pet_basic_attack_delay, "hunter.pet_basic_attack_delay" );

  return profile_str;
}

void hunter_t::copy_from( player_t* source )
{
  player_t::copy_from( source );
  options = debug_cast<hunter_t*>( source ) -> options;
}

stat_e hunter_t::convert_hybrid_stat( stat_e s ) const
{
  // this converts hybrid stats that either morph based on spec or only work
  // for certain specs into the appropriate "basic" stats
  switch ( s )
  {
  case STAT_STR_AGI_INT:
  case STAT_AGI_INT:
  case STAT_STR_AGI:
    return STAT_AGILITY;
  case STAT_STR_INT:
    return STAT_NONE;
  case STAT_SPIRIT:
    return STAT_NONE;
  case STAT_BONUS_ARMOR:
    return STAT_NONE;
  default: return s;
  }
}

void hunter_t::moving()
{
  // Override moving() so that it doesn't suppress auto_shot and only interrupts the few shots that cannot be used while moving.
  if ( ( executing && !executing -> usable_moving() ) || ( channeling && !channeling -> usable_moving() ) )
    player_t::interrupt();
}

/* Report Extension Class
 * Here you can define class specific report extensions/overrides
 */
class hunter_report_t: public player_report_extension_t
{
public:
  hunter_report_t( hunter_t& player ):
    p( player )
  {
  }

  void html_customsection( report::sc_html_stream& os ) override
  {
    os << "\t\t\t\t<div class=\"player-section custom_section\">\n";

    cdwaste::print_html_report( p, p.cd_waste, os );

    os << "\t\t\t\t\t</div>\n";
  }
private:
  hunter_t& p;
};

// HUNTER MODULE INTERFACE ==================================================

struct hunter_module_t: public module_t
{
  hunter_module_t(): module_t( HUNTER ) {}

  player_t* create_player( sim_t* sim, util::string_view name, race_e r = RACE_NONE ) const override
  {
    auto p = new hunter_t( sim, name, r );
    p -> report_extension = std::unique_ptr<player_report_extension_t>( new hunter_report_t( *p ) );
    return p;
  }

  bool valid() const override { return true; }

  void static_init() const override
  {
  }

  void init( player_t* ) const override
  {
  }

  void register_hotfixes() const override
  {
  }

  void combat_begin( sim_t* ) const override {}
  void combat_end( sim_t* ) const override {}
};

} // UNNAMED NAMESPACE

const module_t* module_t::hunter()
{
  static hunter_module_t m;
  return &m;
}
