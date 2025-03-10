// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "action/action.hpp"

#include "action/action_callback.hpp"
#include "action/action_state.hpp"
#include "action/dot.hpp"
#include "buff/buff.hpp"
#include "dbc/data_enums.hh"
#include "dbc/dbc.hpp"
#include "dbc/sc_spell_info.hpp"
#include "player/action_priority_list.hpp"
#include "player/actor_target_data.hpp"
#include "player/covenant.hpp"
#include "player/expansion_effects.hpp"  // try to implement leyshocks_grand_compilation as a callback
#include "player/pet.hpp"
#include "player/player.hpp"
#include "player/player_collected_data.hpp"
#include "player/player_event.hpp"
#include "player/stats.hpp"
#include "sim/cooldown.hpp"
#include "sim/event.hpp"
#include "sim/expressions.hpp"
#include "sim/proc.hpp"
#include "sim/sim.hpp"
#include "util/generic.hpp"
#include "util/io.hpp"
#include "util/util.hpp"

#include <utility>

// ==========================================================================
// Action
// ==========================================================================

namespace
{  // anonymous namespace

/**
 * Hack to bypass some of the full execution chain to be able to re-use normal actions with
 * specialized execution modes (off gcd, cast while casting). Will directly execute the action
 * (instead of going through schedule_execute processing), and parts of our execution chain where
 * relevant (e.g., line cooldown, stats tracking, gcd triggering for cast while casting).
 */
void do_execute( action_t* action, execute_type type )
{
  // Schedule off gcd or cast while casting ready event before the action executes.
  // This prevents the action from scheduling ready events with non-zero delay
  // (for example as a result of the cooldown thresholds update).
  if ( type == execute_type::OFF_GCD )
  {
    action->player->schedule_off_gcd_ready( timespan_t::zero() );
  }
  else if ( type == execute_type::CAST_WHILE_CASTING )
  {
    action->player->schedule_cwc_ready( timespan_t::zero() );
  }

  // Check if the target has died or gone out of range between now and when this was queued
  // If this is the case, we shouldn't continue with attempting to execute it
  if ( !action->target_ready( action->target ) )
  {
    action->sim->print_debug( "{} skipping queued do_execute for {} due to failing target_ready() check", *action->player, *action );
  }
  else
  {
    if ( !action->quiet )
    {
      action->player->iteration_executed_foreground_actions++;
      action->total_executions++;
      action->player->sequence_add( action, action->target, action->sim->current_time() );
    }
    action->execute();
    action->line_cooldown->start();

    // If the ability has a GCD, we need to start it
    action->start_gcd();
  }

  if ( action->player->queueing == action )
  {
    action->player->queueing = nullptr;
  }
}

struct queued_action_execute_event_t : public event_t
{
  action_t* action;
  execute_type type;

  queued_action_execute_event_t( action_t* a, timespan_t t, execute_type type_ )
    : event_t( *a->sim, t ), action( a ), type( type_ )
  { }

  const char* name() const override
  { return "Queued-Action-Execute"; }

  void execute() override
  {
    action->queue_event = nullptr;

    // Sanity check assert to catch violations. Will only trigger (if ever) with off gcd actions,
    // and even then only in the case of bugs.
    assert( action->cooldown->ready <= sim().current_time() );

    // On very very rare occasions, charge-based cooldowns (which update through an event, but
    // indicate readiness through cooldown_t::ready) can have their recharge event and the
    // queued-action-execute event occur on the same timestamp in such a way that the events flip to
    // the wrong order (queued-action-execute comes before recharge event). If this is the case, we
    // need to flip them around to ensure that the sim internal state checks do not fail. The
    // solution is to simply recreate the queued-action-execute event on the same timestamp, which
    // will once again flip the ordering (i.e., lets the recharge event occur first).
    if ( action->cooldown->charges > 1 && action->cooldown->current_charge == 0 && action->cooldown->recharge_event &&
         action->cooldown->recharge_event->remains() == timespan_t::zero() )
    {
      action->queue_event = make_event<queued_action_execute_event_t>( sim(), action, timespan_t::zero(), type );
      // Note, processing ends here
      return;
    }

    if ( type == execute_type::FOREGROUND )
    {
      player_t* actor = action->player;
      if ( !action->ready() || !action->target_ready( action->target ) )
      {
        if ( action->starved_proc )
        {
          action->starved_proc->occur();
        }
        actor->queueing = nullptr;
        if ( !actor->readying )
        {
          actor->schedule_ready( actor->available() );
        }

        // This is an extremely rare event, only seen in a handful of specs with abilities on cooldown that have
        // conditional activation requirements or dynamic cost adjustments.
        if ( action->queue_failed_proc )
          action->queue_failed_proc->occur();

        actor->iteration_executed_foreground_actions--;
        action->total_executions--;

        // If it's the first iteration (where we capture sample sequence) adjust the captured sequence to indicate the
        // queue failed
        if ( ( sim().iterations <= 1 && sim().current_iteration == 0 ) ||
             ( sim().iterations > 1 && actor->nth_iteration() == 1 ) )
        {
          // Find the last action sequence entry that matches the current action
          auto& seq = actor->collected_data.action_sequence;
          auto it = std::find_if( seq.rbegin(), seq.rend(),
                                  [ this ]( const player_collected_data_t::action_sequence_data_t& s ) {
                                    return s.action == action && !s.queue_failed;
                                  } );
          if ( it != seq.rend() )
            ( *it ).queue_failed = true;
        }
      }
      else
      {
        action->schedule_execute();
      }
    }
    // Other execute types are specialized, and need separate handling
    else
    {
      do_execute( action, type );
    }
  }
};

// Action Execute Event =====================================================

struct action_execute_event_t : public player_event_t
{
  action_t* action;
  action_state_t* execute_state;
  bool has_cast_time;

  action_execute_event_t( action_t* a, timespan_t time_to_execute, action_state_t* state = nullptr )
    : player_event_t( *a->player, time_to_execute ),
      action( a ),
      execute_state( state ),
      has_cast_time( time_to_execute > timespan_t::zero() )
  {
    if ( sim().debug )
    {
      sim().print_debug( "New Action Execute Event: {} {} time_to_execute={} (target={}, marker={})", *p(), *a,
                         time_to_execute, ( state ) ? state->target->name() : a->target->name(),
                         ( a->marker ) ? a->marker : '0' );
    }
  }

  const char* name() const override
  {
    return "Action-Execute";
  }
#ifndef NDEBUG
  const char* debug() const override { return action ? action->name() : name(); }
#endif
  ~action_execute_event_t() override
  {
    // Ensure we properly release the carried execute_state even if this event
    // is never executed.
    if ( execute_state )
    {
      action_state_t::release( execute_state );
    }
  }

  void execute() override
  {
    player_t* target = action->target;

    // Pass the carried execute_state to the action. This saves us a few
    // cycles, as we don't need to make a copy of the state to pass to
    // action -> pre_execute_state.
    if ( execute_state )
    {
      target                    = execute_state->target;
      action->pre_execute_state = execute_state;
      execute_state             = nullptr;
    }

    action->execute_event = nullptr;

    // Note, presumes that if the action is instant, it will still be ready, since it was ready on
    // the (near) previous event. Does check target sleepiness, since technically there can be
    // several damage events on the same timestamp one of which will kill the target.
    bool can_execute = true;
    if ( has_cast_time )
      can_execute = ( action->background || action->ready() ) && action->target_ready( target );
    else
      can_execute = !target->is_sleeping();

    if ( sim().distance_targeting_enabled && !action->execute_targeting( action ) )
      can_execute = false;

    // If auto attacks are triggered during a channel, they are rescheduled normally but deal no damage
    if ( !action->special && !action->proc && p()->channeling && p()->channeling->interrupt_auto_attack )
    {
      can_execute = false;
      p()->procs.delayed_aa_channel->occur();

      if ( action->repeating )
      {
        action->schedule_execute();
      }
    }

    if ( can_execute )
    {
      // Action target must follow any potential pre-execute-state target if it differs from the
      // current (default) target of the action.
      action->set_target( target );
      action->execute();
    }
    else
    {
      // Release assigned pre_execute_state, since we are not calling action->execute() (that does
      // it automatically)
      if ( action->pre_execute_state )
      {
        action_state_t::release( action->pre_execute_state );
        action->pre_execute_state = nullptr;
      }
    }

    assert( !action->pre_execute_state );

    if ( action->background )
      return;

    if ( !p()->channeling )
    {
      if ( p()->readying )
      {
        throw std::runtime_error( fmt::format( "Non-channeling action {} for {} is trying to overwrite "
          "player-ready-event upon execute.", action->name(), *p() ) );
      }

      p()->schedule_ready( timespan_t::zero() );
    }

    if ( p()->channeling )
    {
      p()->current_execute_type = execute_type::CAST_WHILE_CASTING;
      p()->schedule_cwc_ready( timespan_t::zero() );
    }
    else if ( p()->gcd_ready > sim().current_time() )
    {
      // We are not channeling and there's still time left on GCD.
      p()->current_execute_type = execute_type::OFF_GCD;
      assert( p()->off_gcd == nullptr );
      p()->schedule_off_gcd_ready( timespan_t::zero() );
    }
  }
};

}  // unnamed namespace

action_t::options_t::options_t()
  : moving( -1 ),
    wait_on_ready( -1 ),
    max_cycle_targets(),
    target_number(),
    interrupt(),
    chain(),
    cycle_targets(),
    cycle_players(),
    interrupt_immediate(),
    if_expr_str(),
    target_if_str(),
    interrupt_if_expr_str(),
    early_chain_if_expr_str(),
    cancel_if_expr_str(),
    sync_str(),
    target_str()
{
}
action_t::action_t( action_e ty, util::string_view token, player_t* p )
  : action_t(ty, token, p, spell_data_t::nil())
{

}

action_t::action_t( action_e ty, util::string_view token, player_t* p, const spell_data_t* s )
  : s_data( s ? s : spell_data_t::nil() ),
    s_data_reporting( spell_data_t::nil() ),
    sim( p->sim ),
    type( ty ),
    name_str( util::tokenize_fn( token ) ),
    name_str_reporting(),
    player( p ),
    target( p->target ),
    item(),
    weapon(),
    default_target( p->target ),
    school( SCHOOL_NONE ),
    original_school( SCHOOL_NONE ),
    id(),
    internal_id( p->get_action_id( name_str ) ),
    resource_current( RESOURCE_NONE ),
    aoe(),
    dual(),
    callbacks( true ),
    suppress_caster_procs(),
    suppress_target_procs(),
    enable_proc_from_suppressed(),
    allow_class_ability_procs(),
    not_a_proc(),
    special(),
    channeled(),
    sequence(),
    quiet(),
    background(),
    use_off_gcd(),
    use_while_casting(),
    usable_while_casting(),
    interrupt_auto_attack( true ),
    reset_auto_attack(),
    ignore_false_positive(),
    action_skill( p->base.skill ),
    direct_tick(),
    treat_as_periodic(),
    ignores_armor(),
    repeating(),
    harmful( true ),
    proc(),
    is_interrupt(),
    is_precombat(),
    initialized(),
    may_hit( true ),
    may_miss( true ),
    may_dodge(),
    may_parry(),
    may_glance(),
    may_block(),
    may_crit(),
    tick_may_crit(),
    tick_zero(),
    tick_on_application(),
    hasted_ticks(),
    consume_per_tick_(),
    rolling_periodic(),
    split_aoe_damage(),
    reduced_aoe_targets( 0.0 ),
    full_amount_targets( 0 ),
    normalize_weapon_speed(),
    ground_aoe(),
    round_base_dmg( true ),
    dynamic_tick_action( true ),  // WoD updates everything on tick by default. If you need snapshotted values for a
                                  // periodic effect, use persistent multipliers.
    track_cd_waste(),
    cd_waste_data(),
    interrupt_immediate_occurred(),
    hit_any_target(),
    ground_aoe_duration( timespan_t::zero() ),
    ap_type( attack_power_type::NONE ),
    dot_behavior( DOT_REFRESH_DURATION ),
    ability_lag( 0_ms, 0_ms ),
    min_gcd(),
    gcd_type( gcd_haste_type::NONE ),
    trigger_gcd( p->base_gcd ),
    range( -1.0 ),
    radius( -1.0 ),
    weapon_power_mod(),
    attack_power_mod(),
    spell_power_mod(),
    amount_delta(),
    base_execute_time(),
    base_tick_time(),
    dot_duration(),
    hasted_dot_duration(),
    dot_max_stack( 1 ),
    dot_ignore_stack(),
    base_costs(),
    secondary_costs(),
    base_costs_per_tick(),
    base_dd_min(),
    base_dd_max(),
    base_td(),
    base_dd_multiplier( 1.0 ),
    base_td_multiplier( 1.0 ),
    base_multiplier( 1.0 ),
    base_hit(),
    base_crit(),
    crit_chance_multiplier( 1.0 ),
    crit_bonus_multiplier( 1.0 ),
    crit_bonus(),
    base_dd_adder(),
    base_ta_adder(),
    weapon_multiplier( 0.0 ),
    chain_multiplier( 1.0 ),
    chain_bonus_damage(),
    base_aoe_multiplier( 1.0 ),
    base_recharge_multiplier( 1.0 ),
    base_recharge_rate_multiplier( 1.0 ),
    dynamic_recharge_multiplier( 1.0 ),
    dynamic_recharge_rate_multiplier( 1.0 ),
    base_teleport_distance(),
    travel_speed(),
    travel_delay(),
    min_travel_time(),
    energize_amount(),
    movement_directionality( movement_direction_type::NONE ),
    parent_dot(),
    child_action(),
    tick_action(),
    execute_action(),
    impact_action(),
    gain( p->get_gain( name_str ) ),
    energize_type( action_energize::NONE ),
    energize_resource( RESOURCE_NONE ),
    cooldown( p->get_cooldown( name_str, this ) ),
    internal_cooldown( p->get_cooldown( name_str + "_internal", this ) ),
    stats( p->get_stats( name_str, this ) ),
    execute_event(),
    queue_event(),
    time_to_execute(),
    time_to_travel(),
    last_resource_cost(),
    num_targets_hit(),
    marker(),
    last_used(),
    option(),
    interrupt_global(),
    if_expr(),
    target_if_mode( TARGET_IF_NONE ),
    target_if_expr(),
    interrupt_if_expr(),
    early_chain_if_expr(),
    cancel_if_expr( nullptr ),
    sync_action(),
    signature_str(),
    target_specific_dot( false ),
    target_specific_debuff( false ),
    target_debuff( spell_data_t::nil() ),
    action_list(),
    starved_proc(),
    queue_failed_proc(),
    total_executions(),
    line_cooldown( new cooldown_t( "line_cd", *p ) ),
    signature(),
    execute_state(),
    pre_execute_state(),
    snapshot_flags(),
    update_flags( STATE_TGT_MUL_DA | STATE_TGT_MUL_TA | STATE_TGT_CRIT ),
    target_cache(),
    options(),
    state_cache(),
    travel_events()
{
  assert( option.cycle_targets == 0 );
  assert( !name_str.empty() && "Abilities must have valid name_str entries!!" );

  if ( sim->initialized && player->nth_iteration() > 0 )
  {
    sim->errorf( "Player %s action %s created after simulator initialization.", player->name(), name() );
  }
  if ( player->nth_iteration() > 0 )
  {
    sim->errorf( "Player %s creating action %s ouside of the first iteration", player->name(), name() );
    assert( false );
  }

  if ( sim->debug )
    sim->out_debug.print( "{} creates {} ({})", *player, *this,
                           ( data().ok() ? data().id() : -1 ) );

  if ( !player->initialized )
  {
    sim->errorf( "Actions must not be created before player_t::init().  Culprit: %s %s\n", player->name(), name() );
    sim->cancel();
  }

  player->action_list.push_back( this );

  if ( data().ok() )
  {
    parse_spell_data( data() );
    player->apply_affecting_auras(*this);
  }

  if ( s_data == spell_data_t::not_found() )
  {
    // this is super-spammy, may just want to disable this after we're sure this section is working as intended.
    if ( sim->debug )
    {
      sim->error(
        "Player {} attempting to use action {} without the required talent, spec, class, race, or level; ignoring.\n",
        player->name(), name() );
    }

    background = true;
  }

  add_option( opt_string( "if", option.if_expr_str ) );
  add_option( opt_string( "interrupt_if", option.interrupt_if_expr_str ) );
  add_option( opt_string( "early_chain_if", option.early_chain_if_expr_str ) );
  add_option( opt_string( "cancel_if", option.cancel_if_expr_str ) );
  add_option( opt_bool( "interrupt", option.interrupt ) );
  add_option( opt_bool( "interrupt_global", interrupt_global ) );
  add_option( opt_bool( "chain", option.chain ) );
  add_option( opt_bool( "cycle_targets", option.cycle_targets ) );
  add_option( opt_bool( "cycle_players", option.cycle_players ) );
  add_option( opt_int( "max_cycle_targets", option.max_cycle_targets ) );
  add_option( opt_string( "target_if", option.target_if_str ) );
  add_option( opt_bool( "moving", option.moving ) );
  add_option( opt_string( "sync", option.sync_str ) );
  add_option( opt_bool( "wait_on_ready", option.wait_on_ready ) );
  add_option( opt_string( "target", option.target_str ) );
  add_option( opt_timespan( "line_cd", line_cooldown->duration ) );
  add_option( opt_float( "action_skill", action_skill ) );
  // Interrupt_immediate forces a channeled action to interrupt on tick (if requested), even if the
  // GCD has not elapsed.
  add_option( opt_bool( "interrupt_immediate", option.interrupt_immediate ) );
  add_option( opt_bool( "use_off_gcd", use_off_gcd ) );
  add_option( opt_bool( "use_while_casting", use_while_casting ) );
}

action_t::~action_t()
{
  delete execute_state;
  delete pre_execute_state;

  while ( state_cache != nullptr )
  {
    action_state_t* s = state_cache;
    state_cache       = s->next;
    delete s;
  }
}

static bool is_direct_damage_effect( const spelleffect_data_t& effect )
{
  static constexpr effect_type_t types[] = {
    E_HEAL, E_SCHOOL_DAMAGE, E_HEALTH_LEECH,
    E_NORMALIZED_WEAPON_DMG, E_WEAPON_DAMAGE, E_WEAPON_PERCENT_DAMAGE
  };
  return range::contains( types, effect.type() );
}

static bool is_periodic_damage_effect( const spelleffect_data_t& effect )
{
  static constexpr effect_subtype_t subtypes[] = {
    A_PERIODIC_DAMAGE, A_PERIODIC_LEECH, A_PERIODIC_HEAL, A_PERIODIC_HEAL_PCT
  };
  return effect.type() == E_APPLY_AURA &&
         range::contains( subtypes, effect.subtype() );
}

bool action_t::has_direct_damage_effect( const spell_data_t& spell )
{
  return range::any_of( spell.effects(), is_direct_damage_effect );
}

bool action_t::has_periodic_damage_effect( const spell_data_t& spell )
{
  return range::any_of( spell.effects(), is_periodic_damage_effect );
}

/**
 * Parse spell data values and write them into corresponding action_t members.
 */
void action_t::parse_spell_data( const spell_data_t& spell_data )
{
  if ( !spell_data.ok() )
  {
    sim->errorf( "%s %s: parse_spell_data: no spell to parse.\n", player->name(), name() );
    return;
  }

  id                = spell_data.id();
  base_execute_time = spell_data.cast_time();
  range             = spell_data.max_range();
  travel_delay      = spell_data.missile_delay();
  min_travel_time   = spell_data.missile_min_duration();
  trigger_gcd       = spell_data.gcd();
  school            = spell_data.get_school_type();

  // parse attributes
  suppress_caster_procs       = spell_data.flags( spell_attribute::SX_SUPPRESS_CASTER_PROCS );
  suppress_target_procs       = spell_data.flags( spell_attribute::SX_SUPPRESS_TARGET_PROCS );
  enable_proc_from_suppressed = spell_data.flags( spell_attribute::SX_ENABLE_PROCS_FROM_SUPPRESSED );
  tick_may_crit               = spell_data.flags( spell_attribute::SX_TICK_MAY_CRIT );
  // check for either spell or melee haste flag. separate if distinction becomes relevant.
  hasted_ticks                = spell_data.flags( spell_attribute::SX_DOT_HASTED ) ||
                                spell_data.flags( spell_attribute::SX_DOT_HASTED_MELEE );
  tick_on_application         = spell_data.flags( spell_attribute::SX_TICK_ON_APPLICATION );
  hasted_dot_duration         = spell_data.flags( spell_attribute::SX_DURATION_HASTED );
  rolling_periodic            = spell_data.flags( spell_attribute::SX_ROLLING_PERIODIC );
  treat_as_periodic           = spell_data.flags( spell_attribute::SX_TREAT_AS_PERIODIC );
  ignores_armor               = spell_data.flags( spell_attribute::SX_TREAT_AS_PERIODIC );  // TODO: better way to parse this?
  may_miss                    = !spell_data.flags( spell_attribute::SX_ALWAYS_HIT );
  allow_class_ability_procs   = spell_data.flags( spell_attribute::SX_ALLOW_CLASS_ABILITY_PROCS );
  not_a_proc                  = spell_data.flags( spell_attribute::SX_NOT_A_PROC );

  if ( spell_data.flags( spell_attribute::SX_REFRESH_EXTENDS_DURATION ) )
    dot_behavior = dot_behavior_e::DOT_REFRESH_PANDEMIC;

  if ( spell_data.flags( spell_attribute::SX_FIXED_TRAVEL_TIME ) )
    travel_delay += spell_data.missile_speed();
  else
    travel_speed = spell_data.missile_speed();

  if ( has_direct_damage_effect( spell_data ) )
    may_crit = !spell_data.flags( spell_attribute::SX_CANNOT_CRIT );

  if ( has_periodic_damage_effect( spell_data ) && spell_data.max_stacks() > 1 )
    dot_max_stack = spell_data.max_stacks();

  cooldown->duration = timespan_t::zero();

  // Default Weapon Assignment
  if ( spell_data.flags( spell_attribute::SX_REQ_MAIN_HAND ) )
  {
    weapon = &( player->main_hand_weapon );
  }
  else if ( spell_data.flags( spell_attribute::SX_REQ_OFF_HAND ) )
  {
    weapon = &( player->off_hand_weapon );
  }

  if ( spell_data.charge_cooldown() > timespan_t::zero() )
  {
    cooldown->duration = spell_data.charge_cooldown();
    cooldown->charges  = spell_data.charges();
    if ( spell_data.internal_cooldown() > timespan_t::zero() )
    {
      internal_cooldown->duration = spell_data.internal_cooldown();
    } else if ( spell_data.cooldown() > timespan_t::zero() )
    {
      internal_cooldown->duration = spell_data.cooldown();
    }
  }
  else if ( spell_data.cooldown() > timespan_t::zero() )
  {
    cooldown->duration = spell_data.cooldown();
  }

  // -1 is uncapped, <-1 is "unknown", 1 is 'limit 1'
  if ( spell_data.max_targets() == -1 || spell_data.max_targets() > 1 )
    aoe = spell_data.max_targets();

  const auto spell_powers = spell_data.powers();
  if ( spell_powers.size() == 1 && spell_powers.front().aura_id() == 0 )
  {
    resource_current = spell_powers.front().resource();
  }
  else
  {
    // Find the first power entry without a aura id
    auto it = range::find( spell_powers, 0U, &spellpower_data_t::aura_id );
    if ( it != spell_powers.end() )
    {
      resource_current = it->resource();
    }
  }

  for ( const spellpower_data_t& pd : spell_powers )
  {
    if ( pd._cost != 0 )
      base_costs[ pd.resource() ] = pd.cost();
    else
      base_costs[ pd.resource() ] = floor( pd.cost() * player->resources.base[ pd.resource() ] );

    secondary_costs[ pd.resource() ] = pd.max_cost();

    if ( pd._cost_per_tick != 0 )
      base_costs_per_tick[ pd.resource() ] = pd.cost_per_tick();
    else
      base_costs_per_tick[ pd.resource() ] = floor( pd.cost_per_tick() * player->resources.base[ pd.resource() ] );
  }

  for ( const spelleffect_data_t& ed : spell_data.effects() )
  {
    parse_effect_data( ed );
  }
}

void action_t::parse_effect_direct_mods( const spelleffect_data_t& spelleffect_data, bool item_scaling )
{
  spell_power_mod.direct  = spelleffect_data.sp_coeff();
  attack_power_mod.direct = spelleffect_data.ap_coeff();
  amount_delta            = spelleffect_data.m_delta();

  if ( !item_scaling )
  {
    if ( !spelleffect_data.sp_coeff() && !spelleffect_data.ap_coeff() )
    {
      base_dd_min = spelleffect_data.min( player, player->level() );
      base_dd_max = spelleffect_data.max( player, player->level() );
    }
  }
  else
  {
    base_dd_min = spelleffect_data.min( item );
    base_dd_max = spelleffect_data.max( item );
  }

  radius = spelleffect_data.radius_max();
}

void action_t::parse_effect_periodic_mods( const spelleffect_data_t& spelleffect_data, bool item_scaling )
{
  spell_power_mod.tick = spelleffect_data.sp_coeff();
  attack_power_mod.tick = spelleffect_data.ap_coeff();

  if ( !item_scaling )
  {
    if ( !spelleffect_data.sp_coeff() && !spelleffect_data.ap_coeff() )
    {
      base_td = spelleffect_data.average( player, player->level() );
    }
  }
  else
  {
    base_td = spelleffect_data.average( item );
  }

  radius = spelleffect_data.radius_max();

  dot_ignore_stack = spelleffect_data.flags( spelleffect_attribute::EX_SUPPRESS_STACKING );
}

void action_t::parse_effect_period( const spelleffect_data_t& spelleffect_data )
{
  if ( spelleffect_data.period() > timespan_t::zero() )
  {
    base_tick_time = spelleffect_data.period();
    dot_duration   = spelleffect_data.spell()->duration();
  }
}

// action_t::parse_effect_data ==============================================
void action_t::parse_effect_data( const spelleffect_data_t& spelleffect_data )
{
  if ( !spelleffect_data.ok() )
  {
    return;
  }

  // Only use item level-based scaling if there's no max scaling level defined for the spell
  bool item_scaling = item && data().max_scaling_level() == 0;

  // Technically, there could be both a single target and an aoe effect in a single spell, but that
  // probably will never happen.
  if ( spelleffect_data.chain_target() > 1 )
  {
    aoe = spelleffect_data.chain_target();
  }

  switch ( spelleffect_data.type() )
  {
    // Direct Damage
    case E_SCHOOL_DAMAGE:
    case E_HEALTH_LEECH:
      parse_effect_direct_mods( spelleffect_data, item_scaling );
      break;

    case E_NORMALIZED_WEAPON_DMG:
      normalize_weapon_speed = true;
      SC_FALLTHROUGH;
    case E_WEAPON_DAMAGE:
      if ( weapon == nullptr )
      {
        weapon = &( player->main_hand_weapon );
      }
      base_dd_min = item_scaling ? spelleffect_data.min( item ) : spelleffect_data.min( player, player->level() );
      base_dd_max = item_scaling ? spelleffect_data.max( item ) : spelleffect_data.max( player, player->level() );
      radius      = spelleffect_data.radius_max();
      break;

    case E_WEAPON_PERCENT_DAMAGE:
      if ( weapon == nullptr )
      {
        weapon = &( player->main_hand_weapon );
      }
      weapon_multiplier = item_scaling ? spelleffect_data.min( item ) : spelleffect_data.min( player, player->level() );
      radius            = spelleffect_data.radius_max();
      break;

      // Dot
    case E_PERSISTENT_AREA_AURA:
      radius = spelleffect_data.radius_max();
      if ( radius < 0 )
        radius = spelleffect_data.radius();
      break;
    case E_APPLY_AURA:
      switch ( spelleffect_data.subtype() )
      {
        case A_PERIODIC_DAMAGE:
        case A_PERIODIC_LEECH:
          parse_effect_periodic_mods( spelleffect_data, item_scaling );
          SC_FALLTHROUGH;
        case A_PERIODIC_ENERGIZE:
          if ( spelleffect_data.subtype() == A_PERIODIC_ENERGIZE && energize_type == action_energize::NONE && spelleffect_data.period() > timespan_t::zero() )
          {
            energize_type     = action_energize::PER_TICK;
            energize_resource = spelleffect_data.resource_gain_type();
            energize_amount   = spelleffect_data.resource( energize_resource );
          }
          SC_FALLTHROUGH;
        case A_PERIODIC_TRIGGER_SPELL_WITH_VALUE:
        case A_PERIODIC_HEALTH_FUNNEL:
        case A_PERIODIC_MANA_LEECH:
        case A_PERIODIC_DAMAGE_PERCENT:
        case A_PERIODIC_DUMMY:
        case A_PERIODIC_TRIGGER_SPELL:
          parse_effect_period( spelleffect_data );
          break;
        case A_SCHOOL_ABSORB:
          spell_power_mod.direct  = spelleffect_data.sp_coeff();
          attack_power_mod.direct = spelleffect_data.ap_coeff();
          amount_delta            = spelleffect_data.m_delta();
          base_dd_min = item_scaling ? spelleffect_data.min( item ) : spelleffect_data.min( player, player->level() );
          base_dd_max = item_scaling ? spelleffect_data.max( item ) : spelleffect_data.max( player, player->level() );
          radius      = spelleffect_data.radius_max();
          break;
        default:
          break;
      }
      break;
    case E_ENERGIZE:
      if ( energize_type == action_energize::NONE )
      {
        energize_type     = action_energize::ON_HIT;
        energize_resource = spelleffect_data.resource_gain_type();
        energize_amount   = spelleffect_data.resource( energize_resource );
      }
      break;
    case E_179: // Spawn Area Triggers
      ground_aoe_duration = spelleffect_data.spell()->duration();
      break;

    default:
      break;
  }
}

// action_t::set_school =====================================================

void action_t::set_school( school_e new_school )
{
  if ( school != new_school )
  {
    sim->print_debug( "{} changing school for {} from {} to {}", *player, *this, school, new_school );
    school = new_school;
  }

  // Decompose school into base types. Note that if get_school() is overridden (e.g., to dynamically
  // alter spell school), then base_schools must be manually updated, to cover the dynamic case.
  base_schools.clear();
  for ( school_e target_school = SCHOOL_ARCANE; target_school < SCHOOL_MAX_PRIMARY; ++target_school )
  {
    if ( dbc::is_school( new_school, target_school ) )
    {
      base_schools.push_back( target_school );
    }
  }
}

void action_t::set_school_override( school_e new_school )
{
  assert( original_school == SCHOOL_NONE && "Cannot override a school that is already overridden." );
  sim->print_debug( "{} adding school override for {} of {}", *player, *this, new_school );
  original_school = get_school();
  set_school( new_school );
}

void action_t::clear_school_override()
{
  assert( original_school != SCHOOL_NONE && "No school override currently exists" );
  sim->print_debug( "{} clearing school override for {} of {}", *player, *this, get_school() );
  set_school( original_school );
  original_school = SCHOOL_NONE;
}

// action_t::parse_target_str ===============================================

void action_t::parse_target_str()
{
  // FIXME: Move into constructor when parse_action is called from there.
  if ( !option.target_str.empty() )
  {
    if ( option.target_str[ 0 ] >= '0' && option.target_str[ 0 ] <= '9' )
    {
      option.target_number = util::to_int( option.target_str );
      player_t* p          = find_target_by_number( option.target_number );
      // Numerical targeting is intended to be dynamic, so don't give an error message if we can't find the target yet
      if ( p )
        target = p;
    }
    else if ( util::str_compare_ci( option.target_str, "self" ) )
      target = this->player;
    else
    {
      player_t* p = sim->find_player( option.target_str );

      if ( p )
        target = p;
      else
        sim->error( "{} {}: Unable to locate target for action '{}'.\n", player->name(), name(),
                     signature_str );
    }
  }
}

// action_t::parse_options ==================================================

void action_t::parse_options( util::string_view options_str )
{
  try
  {
    opts::parse( sim, name(), options, options_str,
      [ this ]( opts::parse_status status, util::string_view name, util::string_view value ) {
        // Fail parsing if strict parsing is used and the option is not found
        if ( sim->strict_parsing && status == opts::parse_status::NOT_FOUND )
        {
          return opts::parse_status::FAILURE;
        }

        // .. otherwise, just warn that there's an unknown option
        if ( status == opts::parse_status::NOT_FOUND )
        {
          sim->error( "Warning: Unknown '{}' option '{}' with value '{}' for {}, ignoring",
            this->name(), name, value, player->name() );
        }

        return status;
      } );

    parse_target_str();
  }
  catch ( const std::exception& e )
  {
    sim->error( "{} {}: Unable to parse options str '{}': {}", player->name(), name(), options_str, e.what() );
    sim->cancel();
  }
}

bool action_t::verify_actor_level() const
{
  if ( ! background && data().id() && !data().is_level( player->true_level ) && data().level() <= MAX_LEVEL )
  {
    sim->errorf( "Player %s attempting to use action %s without the required level (%d < %d).\n", player->name(),
                 name(), player->true_level, data().level() );
    return false;
  }

  return true;
}

bool action_t::verify_actor_spec() const
{
  std::vector<specialization_e> spec_list;
  specialization_e _s = player->specialization();
  if ( data().id() && player->dbc->ability_specialization( data().id(), spec_list ) &&
       range::find( spec_list, _s ) == spec_list.end() )
  {
    // Note that this check can produce false positives for talent abilities which have a different spec set in their
    // talent data from that in the spell data pointed to.
    sim->errorf( "Player %s attempting to use action %s without the required spec.\n", player->name(), name() );

    return false;
  }

  return true;
}

bool action_t::verify_actor_weapon() const
{
  if ( !data().ok() || data().equipped_class() != ITEM_CLASS_WEAPON || player -> is_pet() || player -> is_enemy() )
  {
    return true;
  }

  const unsigned mask = data().equipped_subclass_mask();
  if ( data().flags( spell_attribute::SX_REQ_MAIN_HAND ) &&
       !( mask & ( 1U << util::translate_weapon( player->main_hand_weapon.type ) ) ) )
  {
    std::vector<std::string> types;
    for ( auto wt = ITEM_SUBCLASS_WEAPON_AXE; wt < ITEM_SUBCLASS_WEAPON_FISHING_POLE; ++wt )
    {
      if ( mask & ( 1U << static_cast<unsigned>( wt ) ) )
      {
        types.emplace_back(util::weapon_subclass_string( wt ) );
      }
    }
    sim->error( "Player {} attempting to use action {} without the required main-hand weapon "
                "(requires {}, wielded {}).\n",
      player->name(), name(), fmt::join( types, ", " ),
      util::weapon_subclass_string( util::translate_weapon( player->main_hand_weapon.type ) ) );
    return false;
  }

  if ( data().flags( spell_attribute::SX_REQ_OFF_HAND ) &&
       !( mask & ( 1U << util::translate_weapon( player->off_hand_weapon.type ) ) ) )
  {
    std::vector<std::string> types;
    for ( auto wt = ITEM_SUBCLASS_WEAPON_AXE; wt < ITEM_SUBCLASS_WEAPON_FISHING_POLE; ++wt )
    {
      if ( mask & ( 1U << static_cast<unsigned>( wt ) ) )
      {
        types.emplace_back(util::weapon_subclass_string( wt ) );
      }
    }
    sim->error( "Player {} attempting to use action {} without the required off-hand weapon "
                "(requires {}, wielded {}).\n",
      player->name(), name(), fmt::join( types, ", " ),
      util::weapon_subclass_string( util::translate_weapon( player->off_hand_weapon.type ) ) );
    return false;
  }

  return true;
}

// action_t::base_cost ======================================================

double action_t::base_cost() const
{
  resource_e cr = current_resource();
  double c = base_costs[ cr ].base;

  if ( secondary_costs[ cr ] != 0 )
  {
    c += secondary_costs[ cr ];
  }

  return c;
}

/**
 * Resource cost of the action for current_resource()
 */
double action_t::cost() const
{
  if ( !harmful && is_precombat )
    return 0;

  auto cr = current_resource();
  const auto& bc = base_costs[ cr ];
  auto mul = bc.pct_mul * cost_pct_multiplier();

  if ( mul <= 0 )
  {
    if ( sim->debug )
      sim->out_debug.print( "{} action_t::cost: cost=FREE resource={}", *this, cr );

    return 0;
  }

  auto base = bc.base;
  auto add = bc.flat_add + cost_flat_modifier();
  double c = ( base + add ) * mul;

  // For now, treat secondary cost as "maximum of player current resource, min + max cost". Entirely possible we need to
  // add some additional functionality (such as an overridable method) to determine the cost, if the default behavior is
  // not universal.

  // Also for now, cost reductions to base cost are assumed to not apply to secondary cost, such that the 'min' cost can
  // be modified but the 'max' cost cannot. There are currently no spells with secondary cost that gets their cost
  // modified so this assumption remains untested. Fix accordingly if it is proven incorrect in the future.

  if ( auto sec = secondary_costs[ cr ] )
  {
    auto cur = player->resources.current[ cr ];
    if ( cur >= c )
    {
      c = std::min( c + sec, cur );
    }
  }

  if ( c < 0 )
    c = 0;

  if ( sim->debug )
  {
    sim->out_debug.print( "{} action_t::cost: base={} add={} mul={} secondary_cost={} cost={} resource={}", *this, base,
                          add, mul, secondary_costs[ cr ], c, cr );
  }

  return c;
}

double action_t::cost_flat_modifier() const
{
  return -player->current.resource_reduction[ get_school() ];
}

double action_t::cost_pct_multiplier() const
{
  if ( player->buffs.courageous_primal_diamond_lucidity && current_resource() == RESOURCE_MANA &&
       player->buffs.courageous_primal_diamond_lucidity->check() )
  {
    return 0.0;
  }

  return 1.0;
}

double action_t::cost_per_tick( resource_e r ) const
{
  return base_costs_per_tick[ r ];
}

// action_t::execute_time ===================================================

timespan_t action_t::execute_time() const
{
  auto base = base_execute_time.base;

  auto mul = base_execute_time.pct_mul * execute_time_pct_multiplier();
  if ( mul <= 0 )
    return 0_ms;

  base += base_execute_time.flat_add + execute_time_flat_modifier();
  if ( base <= 0_ms )
    return 0_ms;

  // TODO: assumed to be rounded to ms like tick_time(), confirm if possible.
  return timespan_t::from_millis( std::round( static_cast<double>( base.total_millis() ) * mul ) );
}

timespan_t action_t::execute_time_flat_modifier() const
{
  return 0_ms;
}

double action_t::execute_time_pct_multiplier() const
{
  return 1.0;
}

// action_t::gcd ============================================================

timespan_t action_t::gcd() const
{
  if ( trigger_gcd == timespan_t::zero() )
    return timespan_t::zero();

  timespan_t gcd_ = trigger_gcd;
  switch ( gcd_type )
  {
    // Note, HASTE_ANY should never be used for actions. It does work as a crutch though, since
    // action_t::composite_haste will return the correct haste value.
    case gcd_haste_type::HASTE:
    case gcd_haste_type::SPELL_HASTE:
    case gcd_haste_type::ATTACK_HASTE:
      gcd_ *= composite_haste();
      break;
    case gcd_haste_type::SPELL_CAST_SPEED:
      gcd_ *= player->cache.spell_cast_speed();
      break;
    case gcd_haste_type::AUTO_ATTACK_SPEED:
      gcd_ *= player->cache.auto_attack_speed();
      break;
    case gcd_haste_type::NONE:
    default:
      break;
  }

  if ( gcd_ < min_gcd )
  {
    gcd_ = min_gcd;
  }

  return gcd_;
}

timespan_t action_t::cooldown_duration() const
{
  return cooldown ? cooldown->cooldown_duration( cooldown ) : timespan_t::zero();
}

/** False Positive skill chance, executes command regardless of expression. */
double action_t::false_positive_pct() const
{
  double failure_rate = 0.0;

  if ( action_skill == 1 && player->current.skill_debuff == 0 )
    return failure_rate;

  if ( !player->in_combat || background || player->strict_sequence || ignore_false_positive )
    return failure_rate;

  failure_rate = ( 1 - action_skill ) / 2;
  failure_rate += player->current.skill_debuff / 2;

  if ( dot_duration > timespan_t::zero() )
  {
    if ( dot_t* d = find_dot( target ) )
    {
      if ( d->remains() < dot_duration / 2 )
        failure_rate *= 1 - d->remains() / ( dot_duration / 2 );
    }
  }

  return failure_rate;
}

double action_t::false_negative_pct() const
{
  double failure_rate = 0.0;

  if ( action_skill == 1 && player->current.skill_debuff == 0 )
    return failure_rate;

  if ( !player->in_combat || background || player->strict_sequence )
    return failure_rate;

  failure_rate = ( 1 - action_skill ) / 2;

  failure_rate += player->current.skill_debuff / 2;

  return failure_rate;
}

timespan_t action_t::travel_time() const
{
  if ( travel_speed == 0 && travel_delay == 0 )
    return timespan_t::from_seconds( min_travel_time );

  double t = travel_delay;

  if ( travel_speed > 0 )
  {
    double distance;
    distance = player->get_player_distance( *target );

    if ( execute_state && execute_state->target )
      distance += execute_state->target->height;

    if ( distance > 0 )
      t += distance / travel_speed;
  }

  double v = sim->travel_variance;

  if ( v )
    t = rng().gauss( t, v );

  t = std::max( t, min_travel_time );

  return timespan_t::from_seconds( t );
}

double action_t::total_crit_bonus( const action_state_t* state ) const
{
  double crit_multiplier_buffed = composite_player_critical_multiplier( state );

  double base_crit_bonus = crit_bonus;
  if ( sim->pvp_mode )
    base_crit_bonus += sim->pvp_rules->effectN( 3 ).percent();

  double damage_bonus = composite_crit_damage_bonus_multiplier() * composite_target_crit_damage_bonus_multiplier( state->target );

  double bonus = ( ( 1.0 + base_crit_bonus ) * crit_multiplier_buffed - 1.0 ) * damage_bonus;

  if ( sim->debug )
  {
    sim->print_debug( "{} crit_bonus for {}: total={} base={} mult_buffed={} damage_bonus_mult={}", *player, *this,
                      bonus, crit_bonus, crit_multiplier_buffed, damage_bonus );
  }

  return bonus;
}

double action_t::calculate_weapon_damage( double attack_power ) const
{
  if ( !weapon || weapon_multiplier <= 0 )
    return 0;

  // The weapon damage roll (and its bonus damage) is affected by attak power modifiers just like regular attack power
  double capm             = player -> composite_attack_power_multiplier();
  double dmg              = capm * ( sim->averaged_range( weapon->min_dmg, weapon->max_dmg ) + weapon->bonus_dmg );
  timespan_t weapon_speed = normalize_weapon_speed ? weapon->get_normalized_speed() : weapon->swing_time;
  double power_damage     = weapon_speed.total_seconds() * weapon_power_mod * attack_power;
  double total_dmg        = dmg + power_damage;

  sim->print_debug("{} weapon damage for {}: base=({} to {}) total={} weapon_damage={} bonus_damage={} multiplier={} "
      "speed={} power_damage={} ap={}",
      *player, *this, weapon->min_dmg, weapon->max_dmg, total_dmg, dmg, weapon->bonus_dmg, capm,
      weapon_speed, power_damage, attack_power );

  return total_dmg;
}

double action_t::calculate_tick_amount( action_state_t* state, double dot_multiplier ) const
{
  double amount = base_ta( state );

  if ( !amount && !spell_tick_power_coefficient( state ) && !attack_tick_power_coefficient( state ) )
    return 0;

  // Base amount rounded to some decimal, but the exact precision is currently unknown. For now assume 3 digits as that
  // is what AP/SP multipliers seem to be rounded to.
  amount = std::round( amount * 1000 ) * 0.001;
  // Assuming both flat value tick amount and coeff tick amount are rolled into rolling periodics. Adjust if disproven.
  amount += bonus_ta( state );
  amount += state->composite_spell_power() * spell_tick_power_coefficient( state );
  amount += state->composite_attack_power() * attack_tick_power_coefficient( state );
  amount *= state->composite_ta_multiplier();
  amount *= state->composite_rolling_ta_multiplier();

  double init_tick_amount = amount;

  if ( !sim->average_range )
    amount = floor( amount + rng().real() );

  // Record raw amount to state
  state->result_raw = amount;

  amount *= dot_multiplier;

  // Record total amount to state
  state->result_total = amount;

  // Apply crit damage bonus immediately to periodic damage since there is no travel time (and
  // subsequent impact).
  amount = calculate_crit_damage_bonus( state );

  if ( sim->debug )
  {
    sim->print_debug(
        "{} tick amount for {} on {}: amount={} initial_amount={} base={} bonus={} s_mod={} s_power={} a_mod={} "
        "a_power={} mult={}, tick_mult={}",
        *player, *this, *state->target, amount, init_tick_amount, base_ta( state ), bonus_ta( state ),
        spell_tick_power_coefficient( state ), state->composite_spell_power(), attack_tick_power_coefficient( state ),
        state->composite_attack_power(), state->composite_ta_multiplier(), dot_multiplier );
  }

  return amount;
}

double action_t::calculate_direct_amount( action_state_t* state ) const
{
  double amount = sim->averaged_range( base_da_min( state ), base_da_max( state ) );

  if ( round_base_dmg )
    amount = floor( amount + 0.5 );

  if ( amount == 0 && weapon_multiplier == 0 && attack_direct_power_coefficient( state ) == 0 &&
       spell_direct_power_coefficient( state ) == 0 )
    return 0;

  double base_direct_amount = amount;
  double weapon_amount      = 0;

  if ( weapon_multiplier > 0 )
  {
    // x% weapon damage + Y
    // e.g. Obliterate, Shred, Backstab
    amount += calculate_weapon_damage( state->attack_power );
    amount *= weapon_multiplier;
    weapon_amount = amount;
  }
  amount += spell_direct_power_coefficient( state ) * ( state->composite_spell_power() );
  amount += attack_direct_power_coefficient( state ) * ( state->composite_attack_power() );

  // OH penalty, this applies to any OH attack even if is not based on weapon damage
  double weapon_slot_modifier = 1.0;
  if ( weapon && weapon->slot == SLOT_OFF_HAND )
  {
    weapon_slot_modifier = 0.5;
    amount *= weapon_slot_modifier;
    weapon_amount *= weapon_slot_modifier;
  }

  // Bonus direct damage historically appears to bypass the OH penalty for yellow attacks in-game
  // White damage bonuses (such as Jeweled Signet of Melandrus and older weapon enchants) do not
  if ( !special )
    amount += bonus_da( state ) * weapon_slot_modifier;
  else
    amount += bonus_da( state );

  amount *= state->composite_da_multiplier();

  // damage variation in WoD is based on the delta field in the spell data, applied to entire amount
  double delta_mod = amount_delta_modifier( state );
  if ( !sim->average_range && delta_mod > 0 )
    amount *= 1 + delta_mod / 2 * sim->averaged_range( -1.0, 1.0 );

  // AoE with decay per target
  if ( state->chain_target > 0 && chain_multiplier != 1.0 )
    amount *= pow( chain_multiplier, state->chain_target );

  if ( state->chain_target > 0 && chain_bonus_damage != 0.0 )
    amount *= std::max( 1.0 + chain_bonus_damage * state->chain_target, 0.0 );

  // AoE with static reduced damage per target
  if ( state->chain_target > 0 )
    amount *= base_aoe_multiplier;

  // Spell splits damage across all targets equally
  if ( state->action->split_aoe_damage )
    amount /= state->n_targets;

  // New Shadowlands AoE damage reduction based on total target count
  // The square root factor reaches its minimum when the number of targets is equal
  // to sim->max_aoe_enemies (usually 20), after that it remains constant.
  if ( state->chain_target >= state->action->full_amount_targets &&
       state->action->reduced_aoe_targets > 0.0 &&
       as<double>( state->n_targets ) > state->action->reduced_aoe_targets )
  {
    amount *= std::sqrt( state->action->reduced_aoe_targets / std::min<int>( sim->max_aoe_enemies, state->n_targets ) );
  }

  amount *= composite_aoe_multiplier( state );

  // Spell goes over the maximum number of AOE targets - ignore for enemies
  // Note that this split damage factor DOES affect spells that are supposed
  // to do full damage to the main target.
  if ( state->n_targets > static_cast<size_t>( sim->max_aoe_enemies ) &&
       !state->action->player->is_enemy() )
  {
    amount *= sim->max_aoe_enemies / static_cast<double>( state->n_targets );
  }

  // Record initial amount to state
  state->result_raw = amount;

  if ( state->result == RESULT_GLANCE )
  {
    double delta_skill = ( state->target->level() - player->level() ) * 5.0;

    if ( delta_skill < 0.0 )
      delta_skill = 0.0;

    double max_glance = 1.3 - 0.03 * delta_skill;

    if ( max_glance > 0.99 )
      max_glance = 0.99;
    else if ( max_glance < 0.2 )
      max_glance = 0.20;

    double min_glance = 1.4 - 0.05 * delta_skill;

    if ( min_glance > 0.91 )
      min_glance = 0.91;
    else if ( min_glance < 0.01 )
      min_glance = 0.01;

    if ( min_glance > max_glance )
    {
      double temp = min_glance;
      min_glance  = max_glance;
      max_glance  = temp;
    }

    amount *= sim->averaged_range( min_glance, max_glance );  // 0.75 against +3 targets.
  }

  if ( !sim->average_range )
    amount = floor( amount + rng().real() );

  if ( amount < 0 )
  {
    amount = 0;
  }

  if ( sim->debug )
  {
    sim->print_debug(
        "{} direct amount for {}: amount={} initial_amount={} weapon={} base={} s_mod={} s_power={} "
        "a_mod={} a_power={} mult={} w_mult={} w_slot_mod={} bonus_da={}",
        *player, *this, amount, state->result_raw, weapon_amount, base_direct_amount,
        spell_direct_power_coefficient( state ), state->composite_spell_power(),
        attack_direct_power_coefficient( state ), state->composite_attack_power(), state->composite_da_multiplier(),
        weapon_multiplier, weapon_slot_modifier, bonus_da( state ) );
  }

  // Record total amount to state
  if ( result_is_miss( state->result ) )
  {
    state->result_total = 0.0;
    return 0.0;
  }
  else
  {
    state->result_total = amount;
    return amount;
  }
}

double action_t::calculate_crit_damage_bonus( action_state_t* state ) const
{
  if ( state->result == RESULT_CRIT )
  {
    state->result_crit_bonus = total_crit_bonus( state );
    state->result_total *= 1.0 + state->result_crit_bonus;
  }
  else
  {
    state->result_crit_bonus = 0.0;
  }

  return state->result_total;
}

result_amount_type action_t::report_amount_type( const action_state_t* state ) const
{ return state -> result_type; }

double action_t::composite_total_attack_power() const
{
  return player->composite_total_attack_power_by_type( get_attack_power_type() );
}

double action_t::composite_total_spell_power() const
{
  double spell_power = 0;
  double tmp;

  for ( auto base_school : base_schools )
  {
    tmp = player->composite_total_spell_power( base_school );
    if ( tmp > spell_power )
      spell_power = tmp;
  }

  return spell_power;
}

double action_t::composite_target_armor( player_t* t ) const
{
  return player->composite_player_target_armor( t );
}

double action_t::composite_target_crit_chance( player_t* t ) const
{
  return player->composite_player_target_crit_chance( t );
}

double action_t::composite_target_multiplier( player_t* t ) const
{
  return player->composite_player_target_multiplier( t, get_school() );
}

void action_t::consume_resource()
{
  resource_e cr = current_resource();

  if ( cr == RESOURCE_NONE || base_cost() == 0 || proc )
    return;

  last_resource_cost = cost();

  player->resource_loss( cr, last_resource_cost, nullptr, this );

  sim->print_log("{} consumes {} {} for {} ({})",
      *player, last_resource_cost, cr, *this, player->resources.current[ cr ] );

  stats->consume_resource( cr, last_resource_cost );
}

timespan_t action_t::cooldown_base_duration( const cooldown_t& cd ) const
{ return cd.duration; }

int action_t::num_targets() const
{
  int count = 0;
  for ( size_t i = 0, actors = sim->actor_list.size(); i < actors; i++ )
  {
    player_t* t = sim->actor_list[ i ];

    if ( !t->is_sleeping() && t->is_enemy() )
      count++;
  }

  return count;
}

size_t action_t::available_targets( std::vector<player_t*>& tl ) const
{
  tl.clear();
  if ( !target->is_sleeping() && target->is_enemy() )
    tl.push_back( target );

  for ( auto* t : sim->target_non_sleeping_list )
  {
     if ( t->is_enemy() && ( t != target ) )
    {
      tl.push_back( t );
    }
  }

  if ( sim->debug && !sim->distance_targeting_enabled )
  {
    sim->print_debug("{} regenerated target cache for {} ({})", *player, signature_str, *this );
    for ( size_t i = 0; i < tl.size(); i++ )
    {
      sim->print_debug( "[{}, {} (id={})]", i, *tl[ i ], tl[ i ]->actor_index );
    }
  }

  return tl.size();
}

std::vector<player_t*>& action_t::target_list() const
{
  // Check if target cache is still valid. If not, recalculate it
  if ( !target_cache.is_valid )
  {
    available_targets( target_cache.list );  // This grabs the full list of targets, which will also pickup various
                                             // awfulness that some classes have.. such as prismatic crystal.
    if ( sim->distance_targeting_enabled )
      check_distance_targeting( target_cache.list );
    target_cache.is_valid = true;
  }

  return target_cache.list;
}

player_t* action_t::find_target_by_number( int number ) const
{
  std::vector<player_t*>& tl = target_list();
  size_t total_targets       = tl.size();

  for ( size_t i = 0, j = 1; i < total_targets; i++ )
  {
    player_t* t = tl[ i ];

    int n = ( t == player->target ) ? 1 : as<int>( ++j );

    if ( n == number )
      return t;
  }

  return nullptr;
}

// action_t::calculate_block_result =========================================
// moved here now that we found out that spells can be blocked (Holy Shield)
// block_chance() and crit_block_chance() govern whether any given attack can
// be blocked or not (zero return if not)

block_result_e action_t::calculate_block_result( action_state_t* s ) const
{
  block_result_e block_result = BLOCK_RESULT_UNBLOCKED;

  // 2019-06-02: Looking at logs from Uldir, Battle of Dazar'alor and Crucible of Storms,
  // It appears that non players can't block attacks or abilities anymore
  // Non-player Parry and Miss seem unchanged
  if ( s -> target -> is_enemy() )
  {
    return BLOCK_RESULT_UNBLOCKED;
  }

  // Blocks also get a their own roll, and glances/crits can be blocked.
  if ( result_is_hit( s->result ) && may_block && ( player->position() == POSITION_FRONT ) &&
       !( s->result == RESULT_NONE ) )
  {
    double block_total = block_chance( s );

    if ( block_total > 0 )
    {
      double crit_block = crit_block_chance( s );

      // Roll once for block, then again for crit block if the block succeeds
      if ( rng().roll( block_total ) )
      {
        if ( rng().roll( crit_block ) )
          block_result = BLOCK_RESULT_CRIT_BLOCKED;
        else
          block_result = BLOCK_RESULT_BLOCKED;
      }
    }
  }

  sim->print_debug("{} result for {} is {}", *player, *this, block_result );

  return block_result;
}

// action_t::execute ========================================================

void action_t::execute()
{
#ifndef NDEBUG
  if ( !initialized )
  {
    throw std::runtime_error(
        fmt::format( "{} {} action_t::execute: is not initialized.\n", *player, *this ) );
  }
#endif

  if ( &data() == spell_data_t::not_found() )
  {
    sim->errorf( "Player %s could not find spell data for action %s\n", player->name(), name() );
    sim->cancel();
  }

  int num_targets = n_targets();
  if ( num_targets == 0 && target->is_sleeping() )
    return;

  if ( sim->log && !dual )
  {
    sim->print_log("{} performs {} ({})",
        *player, *this, player->resources.current[ player->primary_resource() ] );
  }

  hit_any_target               = false;
  num_targets_hit              = 0;
  interrupt_immediate_occurred = false;

  if ( harmful && !player->in_combat )
    player->enter_combat();

  // Handle tick_action initial state snapshotting, primarily for handling STATE_MUL_PERSISTENT
  if ( tick_action )
  {
    if ( !tick_action->execute_state )
      tick_action->execute_state = tick_action->get_state();

    tick_action->snapshot_state( tick_action->execute_state, amount_type( tick_action->execute_state, tick_action->direct_tick ) );
  }

  if ( num_targets == -1 || num_targets > 0 )  // aoe
  {
    std::vector<player_t*>& tl = target_list();
    const int max_targets = as<int>( tl.size() );
    num_targets           = ( num_targets < 0 ) ? max_targets : std::min( max_targets, num_targets );

    for ( int t = 0; t < num_targets; t++ )
    {
      action_state_t* s = get_state( pre_execute_state );
      s->target         = tl[ t ];
      s->n_targets      = as<unsigned>( num_targets );
      s->chain_target   = t;
      if ( !pre_execute_state )
      {
        snapshot_state( s, amount_type( s ) );
      }
      // Even if pre-execute state is defined, we need to snapshot target-specific state variables
      // for aoe spells.
      else
      {
        snapshot_internal( s, snapshot_flags & STATE_TARGET, pre_execute_state->result_type );
      }
      s->result       = calculate_result( s );
      s->block_result = calculate_block_result( s );

      s->result_amount = calculate_direct_amount( s );

      if ( sim->debug )
        s->debug();

      schedule_travel( s );
    }
  }
  else  // single target
  {
    num_targets = 1;

    action_state_t* s = get_state( pre_execute_state );
    s->target         = target;
    s->n_targets      = 1;
    s->chain_target   = 0;
    if ( !pre_execute_state )
      snapshot_state( s, amount_type( s ) );
    s->result       = calculate_result( s );
    s->block_result = calculate_block_result( s );

    s->result_amount = calculate_direct_amount( s );

    if ( sim->debug )
      s->debug();

    schedule_travel( s );
  }

  if ( player->resource_regeneration == regen_type::DYNAMIC)
  {
    player->do_dynamic_regen( true );
  }

  update_ready();  // Based on testing with warrior mechanics, Blizz updates cooldowns before consuming resources.
                   // This is very rarely relevant.
  consume_resource();

  if ( !dual && ( !player->first_cast || !harmful ) )
    stats->add_execute( time_to_execute, target );

  if ( pre_execute_state )
    action_state_t::release( pre_execute_state );

  if ( data().id() )
  {
    expansion::bfa::trigger_leyshocks_grand_compilation( data().id(), player );
  }

  // The rest of the execution depends on actually executing on target. Note that execute_state
  // can be nullptr if there are not valid targets to hit on.
  if ( num_targets > 0 )
  {
    if ( composite_teleport_distance( execute_state ) > 0 )
      do_teleport( execute_state );

    if ( execute_state && execute_action && result_is_hit( execute_state->result ) )
    {
      assert( !execute_action->pre_execute_state );
      execute_action->set_target( execute_state->target );
      execute_action->execute();
    }

    if ( callbacks )
    {
      // Proc generic abilities on execute.
      proc_types pt;
      proc_types2 pt2;

      if ( execute_state && ( !suppress_caster_procs || enable_proc_from_suppressed ) &&
           ( pt = execute_state->proc_type() ) != PROC1_INVALID )
      {
        // "On spell cast", only performed for foreground actions
        if ( ( pt2 = execute_state->cast_proc_type2() ) != PROC2_INVALID )
          player->trigger_callbacks( pt, pt2, this, execute_state );

        // "On an execute result"
        if ( ( pt2 = execute_state->execute_proc_type2() ) != PROC2_INVALID )
          player->trigger_callbacks( pt, pt2, this, execute_state );

        // "On interrupt cast result"
        if ( ( pt2 = execute_state->interrupt_proc_type2() ) != PROC2_INVALID )
          player->trigger_callbacks( pt, pt2, this, execute_state );
      }

      // Special handling for "Cast Successful" procs
      // TODO: What happens when there is a PROC1 type handled above in addition to Cast Successful?
      if ( execute_state && ( !suppress_caster_procs || enable_proc_from_suppressed ) )
      {
        pt = PROC1_CAST_SUCCESSFUL;

        // "On spell cast", only performed for foreground actions
        if ( ( pt2 = execute_state->cast_proc_type2() ) != PROC2_INVALID )
          player->trigger_callbacks( pt, pt2, this, execute_state );

        // "On an execute result"
        if ( ( pt2 = execute_state->execute_proc_type2() ) != PROC2_INVALID )
          player->trigger_callbacks( pt, pt2, this, execute_state );

        // "On interrupt cast result"
        if ( ( pt2 = execute_state->interrupt_proc_type2() ) != PROC2_INVALID )
          player->trigger_callbacks( pt, pt2, this, execute_state );
      }
    }
  }

  // Restore the default target after execution. This is required so that
  // target caches do not get into an inconsistent state, if the target of this
  // action (defined by a number) spawns/despawns dynamically during an
  // iteration.
  if ( option.target_number > 0 && target != default_target )
  {
    target = default_target;
  }

  switch ( energize_type_() )
  {
    case action_energize::ON_HIT:
      if ( !hit_any_target )
        break;
      SC_FALLTHROUGH;

    case action_energize::ON_CAST:
      if ( auto amount = composite_energize_amount( execute_state ) )
        gain_energize_resource( energize_resource_(), amount, energize_gain( execute_state ) );
      break;

    case action_energize::PER_HIT:
      if ( auto amount = composite_energize_amount( execute_state ) * num_targets_hit )
        gain_energize_resource( energize_resource_(), amount, energize_gain( execute_state ) );
      break;

    default:
      break;
  }

  if ( repeating && !proc )
    schedule_execute();

  // Some channels fully reset the player's auto attack timer until the end of the channel
  // TODO: Further confirm exact "lost contact" delay time on channeling AA rescheduling
  //       This now appears that it may be latency-based rather than the old 500ms delay
  //       Roughly should be 2*world_lag as per the normal casting delay, as logs show ~150-200ms
  if ( execute_state && !background && special && channeled && !proc && reset_auto_attack )
  {
    timespan_t total_delay = composite_dot_duration( execute_state ) + ( 2 * rng().gauss( player->world_lag) );
    player->reset_auto_attacks( total_delay, player->procs.reset_aa_channel );
  }

  last_used = sim->current_time();
}

void action_t::tick( dot_t* d )
{
  assert( !d->target->is_sleeping() );

  // Always update the state of the base dot. This is required to allow tick action-based dots to
  // update the driver's state per tick (for example due to haste changes -> tick time).
  update_state( d->state, amount_type( d->state, true ) );

  if ( tick_action )
  {
    // 6/22/2018 -- Update logic to use the state of the tick_action rather than the base DoT
    //              This ensures that composite calculations on the tick_action are not ignored
    //              Re-use the execute_state so that STATE_MUL_PERSISTENT snapshots are maintained
    action_state_t* tick_state = tick_action->get_state( tick_action->execute_state );
    if ( tick_action->pre_execute_state )
    {
      tick_state->copy_state( tick_action->pre_execute_state );
      action_state_t::release( tick_action->pre_execute_state );
    }

    tick_state->target = d->target;
    tick_action->set_target( d->target );

    if ( dynamic_tick_action )
    {
      auto flags_ = tick_action->update_flags;

      // ticks actions that are also rolling periodics need to force update composite_rolling_ta_multiplier on every
      // tick_action execute
      if ( tick_action->rolling_periodic )
        flags_ |= STATE_ROLLING_TA;

      tick_action->update_state( tick_state, flags_, amount_type( tick_state, tick_action->direct_tick ) );
    }

    tick_action->schedule_execute( tick_state );

    sim->print_log("{} {} ticks ({} of {}) {}",
        *player, *this, d->current_tick, d->num_ticks(), *d->target );
  }
  else
  {
    d->state->result = RESULT_HIT;

    if ( tick_may_crit && rng().roll( d->state->composite_crit_chance() ) )
      d->state->result = RESULT_CRIT;

    auto stack = dot_ignore_stack ? 1 : d->current_stack();

    d->state->result_amount = calculate_tick_amount( d->state, d->get_tick_factor() * stack );

    assess_damage( amount_type( d->state, true ), d->state );

    if ( sim->debug )
      d->state->debug();
  }

  if ( energize_type_() == action_energize::PER_TICK && d->get_tick_factor() >= 1.0)
  {
    // Partial tick is not counted for resource gain
    gain_energize_resource( energize_resource_(), composite_energize_amount( d->state ), gain );
  }

  stats->add_tick( d->time_to_tick(), d->state->target );

  player->trigger_ready();
}

void action_t::last_tick( dot_t* d )
{
  if ( get_school() == SCHOOL_PHYSICAL )
  {
    buff_t* b = d->state->target->debuffs.bleeding;
    if ( b )
    {
      if ( b->current_value > 0 )
        b->current_value -= 1.0;
      if ( b->current_value == 0 )
        b->expire();
    }
  }

  if ( channeled && player->channeling == this )
  {
    player->channeling = nullptr;

    // Retarget this channel skill, since during the channel a retargeting event may have occurred.
    // The comparison is made against the actor's "current target", which can be considered the
    // current baseline target all actions share (with some exceptions, such as fixed targeting).
    if ( option.target_number == 0 && target != player->target )
    {
      sim->print_debug( "{} adjust channel target on last tick, current={}, new={}", *player, *target,
                        *player->target );
      target = player->target;
    }
  }
}

void action_t::assess_damage( result_amount_type rt, action_state_t* state )
{
  // Execute outbound damage assessor pipeline on the state object
  player->assessor_out_damage.execute( rt, state );

  // TODO: Should part of this move to assessing, priority_iteration_damage for example?
  if ( state->result_raw > 0 || result_is_miss( state->result ) )
  {
    if ( sim->fight_style == FIGHT_STYLE_DUNGEON_SLICE || sim->fight_style == FIGHT_STYLE_DUNGEON_ROUTE )
    {
      if ( state->target->is_boss() )
      {
        player->priority_iteration_dmg += state->result_amount;
      }
    }
    else if ( state->target == sim->target ||
              ( sim->merge_enemy_priority_dmg && state->target->is_boss() ) )
    {
      player->priority_iteration_dmg += state->result_amount;
    }

    record_data( state );
  }
}

void action_t::record_data( action_state_t* data )
{
  if ( !stats )
    return;

  stats->add_result( data->result_amount, data->result_total, report_amount_type( data ), data->result,
                     ( may_block || player->position() != POSITION_BACK ) ? data->block_result : BLOCK_RESULT_UNKNOWN,
                     data->target );
}

// Should be called only by foreground action executions (i.e., Player-Ready event calls
// player_t::execute_action() ). Background actions should (and are) directly call
// action_t::schedule_execute. Off gcd actions will either directly execute the action, or schedule
// a queued off-gcd execution.
void action_t::queue_execute( execute_type et )
{
  auto queue_delay = cooldown->queue_delay();
  if ( queue_delay > timespan_t::zero() )
  {
    queue_event      = make_event<queued_action_execute_event_t>( *sim, this, queue_delay, et );
    player->queueing = this;
  }
  else
  {
    if ( et == execute_type::FOREGROUND )
    {
      schedule_execute();
    }
    else
    {
      // If the charge cooldown is recharging on the same timestamp, we need to create a zero-time
      // event to execute the (queued) action, so that the charge cooldown can regenerate.
      if ( cooldown->charges > 1 && cooldown->current_charge == 0 && cooldown->recharge_event &&
           cooldown->recharge_event->remains() == timespan_t::zero() )
      {
        queue_event      = make_event<queued_action_execute_event_t>( *sim, this, timespan_t::zero(), et );
        player->queueing = this;
      }
      else
      {
        do_execute( this, et );
      }
    }
  }
}

void action_t::start_gcd()
{
  auto current_gcd = gcd();
  if ( current_gcd == timespan_t::zero() )
  {
    return;
  }

  // Setup the GCD ready time, and associated haste-related values
  player->gcd_ready      = sim->current_time() + current_gcd;
  player->gcd_type = gcd_type;
  switch ( gcd_type )
  {
    case gcd_haste_type::SPELL_HASTE:
      player->gcd_current_haste_value = player->cache.spell_haste();
      break;
    case gcd_haste_type::ATTACK_HASTE:
      player->gcd_current_haste_value = player->cache.attack_haste();
      break;
    case gcd_haste_type::SPELL_CAST_SPEED:
      player->gcd_current_haste_value = player->cache.spell_cast_speed();
      break;
    case gcd_haste_type::AUTO_ATTACK_SPEED:
      player->gcd_current_haste_value = player->cache.auto_attack_speed();
      break;
    default:
      break;
  }

  if ( player->action_queued && sim->strict_gcd_queue )
  {
    player->gcd_ready -= sim->queue_gcd_reduction;
  }
}

void action_t::schedule_execute( action_state_t* state )
{
  if ( target->is_sleeping() )
  {
    sim->print_debug( "{} action={} attempted to schedule on a dead target {}",
      *player, *this, *target );

    if ( state )
    {
      action_state_t::release( state );
    }
    return;
  }

  sim->print_log( "{} schedules execute for {}", *player, *this );

  time_to_execute = execute_time();

  execute_event = start_action_execute_event( time_to_execute, state );

  if ( trigger_gcd > timespan_t::zero() )
    player->off_gcdactions.clear();

  if ( !background )
  {
    // We were queueing this on an almost finished cooldown, so queueing is over, and we begin
    // executing this action.
    if ( player->queueing == this )
    {
      player->queueing = nullptr;
    }

    player->executing = this;

    start_gcd();

    if ( time_to_execute > timespan_t::zero() )
    {
      player->current_execute_type = execute_type::CAST_WHILE_CASTING;
      assert( player->cast_while_casting_poll_event == nullptr );
      player->schedule_cwc_ready( timespan_t::zero() );
    }

    // While an ability is casting, the auto_attack is paused
    // So we simply reschedule the auto_attack by the ability's cast time
    if ( special && time_to_execute > timespan_t::zero() && !proc && ( interrupt_auto_attack || reset_auto_attack ) )
    {
      if( reset_auto_attack )
        player->reset_auto_attacks( time_to_execute, player->procs.reset_aa_cast );
      else
        player->delay_auto_attacks( time_to_execute, player->procs.delayed_aa_cast );
    }

    if ( player->resource_regeneration == regen_type::DYNAMIC )
    {
      player->do_dynamic_regen( true );
    }
  }
}

void action_t::reschedule_execute( timespan_t time )
{
  sim->print_log( "{} reschedules execute for {}", *player, *this );

  timespan_t delta_time = sim->current_time() + time - execute_event->occurs();

  time_to_execute += delta_time;

  if ( delta_time > timespan_t::zero() )
  {
    execute_event->reschedule( time );
  }
  else  // Impossible to reschedule events "early".  Need to be canceled and re-created.
  {
    action_state_t* state = debug_cast<action_execute_event_t*>( execute_event )->execute_state;
    event_t::cancel( execute_event );
    execute_event = start_action_execute_event( time, state );
  }
}

void action_t::update_ready( timespan_t cd_duration /* = timespan_t::min() */ )
{
  if ( cd_waste_data )
    cd_waste_data->add( cd_duration, time_to_execute );

  if ( ( cd_duration > 0_ms ||
         ( cd_duration == timespan_t::min() && cooldown_duration() > 0_ms ) ) &&
       !dual )
  {
    timespan_t delay = 0_ms;

    if ( !background && !proc )
    { /*This doesn't happen anymore due to the gcd queue, in WoD if an ability has a cooldown of 20 seconds,
      it is usable exactly every 20 seconds with proper Lag tolerance set in game.
      The only situation that this could happen is when world lag is over 400, as blizzard does not allow
      custom lag tolerance to go over 400.
      */
      delay = rng().gauss( player->world_lag );
      if ( delay > 400_ms )
      {
        delay -= 400_ms;  // Even high latency players get some benefit from CLT.
        sim->print_debug( "{} delaying the cooldown finish of {} by {}", *player, *this, delay );
      }
      else
        delay = 0_ms;
    }

    cooldown->start( this, cd_duration, delay );

    if ( sim->debug )
    {
      sim->print_debug( "{} starts cooldown for {} ({}, {}/{}). Duration={} Delay={}. {}.", *player, *this, *cooldown,
                        cooldown->current_charge, cooldown->charges,
                        cd_duration == timespan_t::min() ? cooldown_duration() : cd_duration, delay,
                        cooldown->ready > sim->current_time() ? fmt::format( "Will be ready at {}", cooldown->ready )
                                                              : "Ready now" );
    }

    if ( internal_cooldown->duration > 0_ms )
    {
      internal_cooldown->start( this );

      sim->print_debug( "{} starts internal_cooldown for {} ({}). Will be ready at {}", *player, *this,
                        *internal_cooldown, internal_cooldown->ready );
    }
  }
}

bool action_t::usable_moving() const
{
  if ( player->buffs.norgannons_sagacity && player->buffs.norgannons_sagacity->check() )
    return true;

  if ( execute_time() > 0_ms )
    return false;

  if ( channeled )
    return false;

  if ( range > 0 && range <= 5 )
    return false;

  return true;
}

bool action_t::usable_precombat() const
{
  if ( !harmful )
    return true;

  if ( this->travel_time() > 0_ms || this->base_execute_time > 0_ms )
    return true;

  return false;
}

bool action_t::target_ready( player_t* candidate_target )
{
  // Ensure target is valid to execute on
  if ( candidate_target->is_sleeping() )
    return false;

  if ( candidate_target->debuffs.invulnerable &&
       candidate_target->debuffs.invulnerable->check() && harmful )
    return false;

  if ( sim->distance_targeting_enabled && range > 0 &&
       player->get_player_distance( *candidate_target ) > range + candidate_target->combat_reach )
    return false;

  return true;
}

bool action_t::select_target()
{
  if ( target_if_mode != TARGET_IF_NONE )
  {
    player_t* potential_target = select_target_if_target();
    if ( potential_target )
    {
      // If the target changes, we need to regenerate the target cache to get the new primary target
      // as the first element of target_list. Only do this for abilities that are aoe.
      if ( is_aoe() && potential_target != target )
      {
        target_cache.is_valid = false;
      }
      if ( !child_action.empty() )
      {  // If spell_targets is used on the child instead of the parent action, we need to reset the cache for that
         // action as well.
        for ( auto* child : child_action )
          child->target_cache.is_valid = false;
      }
      target = potential_target;
    }
    else
      return false;
  }

  if ( option.cycle_targets && sim->target_non_sleeping_list.size() > 1 )
  {
    player_t* saved_target = target;
    option.cycle_targets   = false;
    bool found_ready       = false;

    // Note, need to take a copy of the original target list here, instead of a reference. Otherwise
    // if spell_targets (or any expression that uses the target list) modifies it, the loop below
    // may break, since the number of elements on the vector is not the same as it originally was
    std::vector<player_t*> ctl = target_list();
    size_t num_targets         = ctl.size();

    if ( ( option.max_cycle_targets > 0 ) && ( (size_t)option.max_cycle_targets < num_targets ) )
      num_targets = option.max_cycle_targets;

    for ( size_t i = 0; i < num_targets; i++ )
    {
      target = ctl[ i ];
      if ( action_ready() )
      {
        found_ready = true;
        break;
      }
    }

    option.cycle_targets = true;

    if ( found_ready )
    {
      // If the target changes, we need to regenerate the target cache to get the new primary target
      // as the first element of target_list. Only do this for abilities that are aoe.
      if ( n_targets() > 1 && target != saved_target )
      {
        target_cache.is_valid = false;
      }
      return true;
    }

    target = saved_target;

    return false;
  }

  if ( option.cycle_players )  // Used when healing players in the raid.
  {
    player_t* saved_target = target;
    option.cycle_players   = false;
    bool found_ready       = false;

    const auto& tl = sim->player_no_pet_list.data();

    size_t num_targets = tl.size();

    if ( ( option.max_cycle_targets > 0 ) && ( (size_t)option.max_cycle_targets < num_targets ) )
      num_targets = option.max_cycle_targets;

    for ( size_t i = 0; i < num_targets; i++ )
    {
      target = tl[ i ];
      if ( action_ready() )
      {
        found_ready = true;
        break;
      }
    }

    option.cycle_players = true;

    if ( found_ready )
      return true;

    target = saved_target;

    return false;
  }

  if ( option.target_number )
  {
    player_t* saved_target  = target;
    int saved_target_number = option.target_number;
    option.target_number    = 0;

    target = find_target_by_number( saved_target_number );

    bool is_ready = false;

    if ( target )
      is_ready = action_ready();

    option.target_number = saved_target_number;

    if ( is_ready )
      return true;

    target = saved_target;

    return false;
  }

  // Normal casting (no cycle_targets, cycle_players, target_number, or target_if specified). Check
  // that we can cast on the target
  return target ? target_ready( target ) : false;
}

bool action_t::action_ready()
{
  // Check that the ability itself is usable, before going on to other user-input related readiness
  // checks. Note, no target-based stuff should be done here
  if ( !ready() )
  {
    return false;
  }

  // Can't find any target to cast on
  if ( !select_target() )
  {
    return false;
  }

  // If cycle_targets was used, then we know that the ability is already usable, no need to test the
  // rest of the readiness
  if ( option.cycle_targets && sim->target_non_sleeping_list.size() > 1 )
  {
    return true;
  }

  if ( action_skill != 1 || player->current.skill_debuff != 0 )
  {
    if ( rng().roll( false_positive_pct() ) )
      return true;

    if ( rng().roll( false_negative_pct() ) )
      return false;
  }

  if ( !has_movement_directionality() )
    return false;

  if ( line_cooldown->down() )
    return false;

  if ( sync_action && !sync_action->action_ready() )
    return false;

  if ( option.moving != -1 && option.moving != ( player->is_moving() ? 1 : 0 ) )
    return false;

  if ( if_expr && !if_expr->success() )
    return false;

  return true;
}

// Properties that govern if the spell itself is executable, without considering any kind of user
// options
bool action_t::ready()
{
  // Check conditions that do NOT pertain to the target before cycle_targets
  if ( !cooldown->is_ready() )
    return false;

  if ( internal_cooldown->down() )
    return false;

  if ( player->is_moving() && !usable_moving() )
    return false;

  auto resource = current_resource();
  if ( resource != RESOURCE_NONE && !player->resource_available( resource, cost() ) )
  {
    if ( starved_proc )
      starved_proc->occur();
    return false;
  }

  if ( usable_while_casting )
  {
    if ( execute_time() > timespan_t::zero() )
    {
      return false;
    }

    // Don't allow cast-while-casting spells that trigger the GCD to be ready if the GCD is still
    // ongoing (during the cast)
    if ( ( player->executing || player->channeling ) && gcd() > timespan_t::zero() &&
         player->gcd_ready > sim->current_time() )
    {
      return false;
    }
  }

  return true;
}

void action_t::init()
{
  if ( initialized )
    return;

  if ( !verify_actor_level() || !verify_actor_spec() || !verify_actor_weapon() )
  {
    background = true;
  }

  assert( !( impact_action && tick_action ) &&
          "Both tick_action and impact_action should not be used in a single action." );

  assert( !( n_targets() && channeled ) && "DONT create a channeled aoe spell!" );

  if ( !option.sync_str.empty() )
  {
    sync_action = player->find_action( option.sync_str );

    if ( !sync_action )
    {
      throw std::runtime_error(fmt::format("Unable to find sync action '{}' for primary action.",
          option.sync_str ));
    }
  }

  if ( option.cycle_targets && option.target_number )
  {
    option.target_number = 0;
    sim->errorf(
        "Player %s trying to use both cycle_targets and a numerical target for action %s - defaulting to "
        "cycle_targets\n",
        player->name(), name() );
  }

  if ( tick_action )
  {
    tick_action->direct_tick = true;
    tick_action->dual        = true;
    tick_action->stats       = stats;
    tick_action->parent_dot  = target->get_dot( name_str, player );
    if ( tick_action->parent_dot && range > 0 && tick_action->radius > 0 && tick_action->is_aoe() )
      // If the parent spell has a range, the tick_action has a radius and is an aoe spell, then the tick action likely
      // also has a range. This will allow distance_target_t to correctly determine spells that radiate from the target,
      // instead of the player.
      tick_action->range = range;
    stats->action_list.push_back( tick_action );
  }

  stats->school = get_school();

  if ( quiet )
    stats->quiet = true;

  if ( rolling_periodic )
  {
    // Rolling Periodic refresh behavior overrides other behaviors.
    dot_behavior = dot_behavior_e::DOT_ROLLING;
    snapshot_flags |= STATE_ROLLING_TA;
  }

  if ( may_crit || tick_may_crit )
    snapshot_flags |= STATE_CRIT | STATE_TGT_CRIT;

  if ( has_periodic_damage_effect( data() ) ||
       ( ( base_td > 0 || spell_power_mod.tick > 0 || attack_power_mod.tick > 0 || rolling_periodic ) &&
         dot_duration > 0_ms ) )
  {
    snapshot_flags |= STATE_MUL_TA | STATE_TGT_MUL_TA | STATE_MUL_PERSISTENT | STATE_VERSATILITY;
  }

  if ( has_direct_damage_effect( data() ) || base_dd_min > 0 || spell_power_mod.direct > 0 ||
       attack_power_mod.direct > 0 || weapon_multiplier > 0 )
  {
    snapshot_flags |= STATE_MUL_DA | STATE_TGT_MUL_DA | STATE_MUL_PERSISTENT | STATE_VERSATILITY;
  }

  if ( player->is_pet() && ( snapshot_flags & ( STATE_MUL_DA | STATE_MUL_TA | STATE_TGT_MUL_DA | STATE_TGT_MUL_TA |
                                                STATE_MUL_PERSISTENT | STATE_VERSATILITY ) ) )
  {
    snapshot_flags |= STATE_MUL_PET | STATE_TGT_MUL_PET;
  }

  if ( school == SCHOOL_PHYSICAL )
    snapshot_flags |= STATE_TGT_ARMOR;

  if ( data().flags( spell_attribute::SX_DISABLE_PLAYER_MULT ) ||
       data().flags( spell_attribute::SX_DISABLE_PLAYER_HEALING_MULT ) )
  {
    snapshot_flags &= ~( STATE_VERSATILITY | STATE_MUL_PLAYER_DAM | STATE_MUL_PET );
  }

  if ( data().flags( spell_attribute::SX_DISABLE_TARGET_MULT ) )
  {
    snapshot_flags &= ~( STATE_TGT_MUL_TA | STATE_TGT_MUL_DA | STATE_TGT_ARMOR | STATE_TGT_MUL_PET );
    update_flags &= ~( STATE_TGT_MUL_TA | STATE_TGT_MUL_DA | STATE_TGT_ARMOR | STATE_TGT_MUL_PET );
  }

  // TODO: accomodate negative mults such as damage reduction
  if ( data().flags( spell_attribute::SX_DISABLE_TARGET_POSITIVE_MULT ) )
  {
    snapshot_flags &= ~( STATE_TGT_MUL_TA | STATE_TGT_MUL_DA );
    update_flags &= ~( STATE_TGT_MUL_TA | STATE_TGT_MUL_DA );
  }

  if ( ( spell_power_mod.direct > 0 || spell_power_mod.tick > 0 ) )
  {
    snapshot_flags |= STATE_SP;
  }

  if ( ( weapon_power_mod > 0 || attack_power_mod.direct > 0 || attack_power_mod.tick > 0 ) )
  {
    snapshot_flags |= STATE_AP;
  }

  if ( dot_duration > timespan_t::zero() && ( hasted_ticks || channeled ) )
    snapshot_flags |= STATE_HASTE;

  // WOD: Dot Snapshoting is gone
  update_flags |= snapshot_flags;

  // WOD: Yank out persistent multiplier from update flags, so they get
  // snapshot once at the application of the spell
  update_flags &= ~STATE_MUL_PERSISTENT;

  // The Rolling Periodic multiplier is only updated when the DoT is applied or refreshed
  update_flags &= ~STATE_ROLLING_TA;

  // Channeled dots get haste snapshoted
  if ( channeled )
  {
    update_flags &= ~STATE_HASTE;
  }

  // If any of the periodic effects have EX_COMPUTE_ON_CAST (i.e. snapshot) remove damage update flags
  // NOTE: Only player-scoped damage modifiers are snapshotted, target-scoped modifiers are still dynamic.
  for ( const auto& eff : data().effects() )
  {
    if ( is_periodic_damage_effect( eff ) && eff.flags( spelleffect_attribute::EX_COMPUTE_ON_CAST ) )
    {
      update_flags &= ~( STATE_AP | STATE_SP | STATE_MUL_TA | STATE_VERSATILITY );
      break;
    }
  }

  // Figure out BfA attack power mode based on information assigned to the action object. Note that
  // this only defines the ap type, the ability may not necessarily use attack power at all, however
  // that is not possible to know at init time with 100% accuracy.
  // Only overwrite this if the default attack_power_type::AP_NONE value is still set, since modules may set manually.
  if ( ap_type == attack_power_type::NONE )
  {
    // Weapon multiplier is set. The power calculation for the ability uses base ap only, as the
    // weapon base damage is incorporated into the weapon damage%. Hardly ever used in BfA.
    if ( weapon_multiplier > 0 )
    {
      ap_type = attack_power_type::NO_WEAPON;
    }
    // Offhand weapon is used in the ability, use off hand weapon dps
    else if ( weapon && weapon->slot == SLOT_OFF_HAND )
    {
      ap_type = attack_power_type::WEAPON_OFFHAND;
    }
    // All else fails, use the player's default ap type
    else
    {
      ap_type = player->default_ap_type();
    }
  }

  if ( !( background || sequence ) && ( action_list && action_list->name_str == "precombat" ) )
  {
    if ( usable_precombat() )
    {
      player->precombat_action_list.push_back( this );
    }
    else
    {
      throw std::runtime_error( "Can only add harmful action with travel or cast-time to precombat action list." );
    }
  }
  else if ( action_list && action_list->name_str != "precombat" )
  {
    action_priority_list_t* apl = player->find_action_priority_list( action_list->name_str );
    if ( !( background || sequence ) )
    {
      apl->foreground_action_list.push_back( this );
    }
    // Special case for disabled actions that are preceded by pool_resource,for_next=1 lines
    // If we are skipping adding the action to the foreground_action_list above, we also need to disable the pool_resource entry
    else if ( !apl->foreground_action_list.empty() &&
              apl->foreground_action_list.back()->name_str == "pool_resource"  &&
              util::str_in_str_ci( apl->foreground_action_list.back()->signature_str, "for_next=1" ) )
    {
      sim->print_debug( "{} pruning action '{}' due to action {} being disabled", *player, apl->foreground_action_list.back()->signature_str, *this );
      apl->foreground_action_list.back()->background = true;
      apl->foreground_action_list.pop_back();
    }
  }

  if ( action_list && action_list->name_str == "precombat" )
    is_precombat = true;

  initialized = true;

#ifndef NDEBUG
  if ( sim->distance_targeting_enabled )
    sim->print_debug( "{} - radius={} range={}", *this, radius, range );
#endif

  consume_per_tick_ =
      range::any_of( base_costs_per_tick, []( const double& d ) { return d != 0; } );

  // Setup default target in init
  default_target = target;

  // Re-initialize base schools as modules often set schools directly in their constructors
  set_school( get_school() );

  // Initialize dot - so we can access it from expressions
  if ( dot_duration /*composite_dot_duration( nullptr )*/ > timespan_t::zero() ||
       ( tick_zero || tick_on_application ) )
  {
    get_dot( target );
  }

  // Make sure spells that have GCD shorter than the global min GCD trigger
  // the correct (short) GCD.
  min_gcd = std::min( min_gcd, trigger_gcd );

  if ( use_off_gcd && trigger_gcd == 0_ms )
  {
    cooldown->add_execute_type( execute_type::OFF_GCD );
    internal_cooldown->add_execute_type( execute_type::OFF_GCD );
  }

  if ( usable_while_casting && use_while_casting )
  {
    cooldown->add_execute_type( execute_type::CAST_WHILE_CASTING );
    internal_cooldown->add_execute_type( execute_type::CAST_WHILE_CASTING );
  }

  // Normal foreground actions get marked too, unused for now though
  if ( cooldown->execute_types_mask == 0 && !background )
  {
    cooldown->add_execute_type( execute_type::FOREGROUND );
    internal_cooldown->add_execute_type( execute_type::FOREGROUND );
  }

  // Make sure background is set for triggered actions.
  // Leads to double-readying of the player otherwise.
  assert( ( !execute_action || execute_action->background ) && "Execute action needs to be set to background." );
  assert( ( !tick_action || tick_action->background ) && "Tick action needs to be set to background." );
  assert( ( !impact_action || impact_action->background ) && "Impact action needs to be set to background." );
}

void action_t::init_finished()
{
  if ( !option.target_if_str.empty() )
  {
    std::string::size_type offset = option.target_if_str.find( ':' );
    if ( offset != std::string::npos )
    {
      std::string target_if_type_str = option.target_if_str.substr( 0, offset );
      option.target_if_str.erase( 0, offset + 1 );
      if ( util::str_compare_ci( target_if_type_str, "max" ) )
      {
        target_if_mode = TARGET_IF_MAX;
      }
      else if ( util::str_compare_ci( target_if_type_str, "min" ) )
      {
        target_if_mode = TARGET_IF_MIN;
      }
      else if ( util::str_compare_ci( target_if_type_str, "first" ) )
      {
        target_if_mode = TARGET_IF_FIRST;
      }
      else
      {
        throw std::invalid_argument(fmt::format("Unknown target_if mode '{}' for choose_target. Valid values are 'min', 'max', 'first'.",
                     target_if_type_str ));
      }
    }
    else if ( !option.target_if_str.empty() )
    {
      target_if_mode = TARGET_IF_FIRST;
    }

    if ( !option.target_if_str.empty() )
    {
       target_if_expr = expr_t::parse( this, option.target_if_str, sim->optimize_expressions );
       if ( !target_if_expr )
       {
         throw std::invalid_argument(fmt::format("Could not parse target if expression from '{}'", option.target_if_str));
       }
    }
  }

  // Collect this object into a list of dynamic targeting actions so they can be managed separate of
  // the total action object list
  if ( option.cycle_targets || target_if_expr )
  {
    player->dynamic_target_action_list.insert( this );
  }

  if ( !option.if_expr_str.empty() )
  {
    if_expr = expr_t::parse( this, option.if_expr_str, sim->optimize_expressions );
    if ( if_expr == nullptr )
    {
      throw std::invalid_argument(fmt::format("Could not parse if expression from '{}'", option.if_expr_str));
    }
  }

  if ( !option.interrupt_if_expr_str.empty() )
  {
    interrupt_if_expr = expr_t::parse( this, option.interrupt_if_expr_str, sim->optimize_expressions );
    if ( !interrupt_if_expr )
    {
      throw std::invalid_argument(fmt::format("Could not parse interrupt if expression from '{}'", option.interrupt_if_expr_str));
    }
  }

  if ( !option.early_chain_if_expr_str.empty() )
  {
    early_chain_if_expr = expr_t::parse( this, option.early_chain_if_expr_str, sim->optimize_expressions );
    if ( !early_chain_if_expr )
    {
      throw std::invalid_argument(fmt::format("Could not parse chain if expression from '{}'", option.early_chain_if_expr_str));
    }
  }

  if ( !option.cancel_if_expr_str.empty() )
  {
    cancel_if_expr = expr_t::parse( this, option.cancel_if_expr_str, sim->optimize_expressions );
    if ( !cancel_if_expr )
    {
      throw std::invalid_argument( fmt::format( "Could not parse cancel if expression from '{}'",
            option.cancel_if_expr_str ) );
    }
  }

  if ( track_cd_waste )
    cd_waste_data = player->get_cooldown_waste_data( cooldown );
}

void action_t::reset()
{
  if ( pre_execute_state )
  {
    action_state_t::release( pre_execute_state );
  }
  cooldown->reset_init();
  internal_cooldown->reset_init();
  line_cooldown->reset_init();
  execute_event                = nullptr;
  queue_event                  = nullptr;
  interrupt_immediate_occurred = false;
  travel_events.clear();
  target    = default_target;
  last_used = timespan_t::min();

  target_cache.is_valid = false;

  dynamic_recharge_multiplier      = 1.0;
  dynamic_recharge_rate_multiplier = 1.0;

  if ( if_expr )
  {
    expr_t::optimize_expression( if_expr, *sim );
    if ( ( player->nth_iteration() - sim->optimize_expressions ) >= 0 && action_list && if_expr->always_false() )
    {
      std::vector<action_t*>::iterator i =
          std::find( action_list->foreground_action_list.begin(), action_list->foreground_action_list.end(), this );
      if ( i != action_list->foreground_action_list.end() )
      {
        action_list->foreground_action_list.erase( i );
      }

      player->dynamic_target_action_list.erase( this );
    }
  }
  expr_t::optimize_expression( target_if_expr, *sim );
  expr_t::optimize_expression( interrupt_if_expr, *sim );
  expr_t::optimize_expression( early_chain_if_expr, *sim );
  expr_t::optimize_expression( cancel_if_expr, *sim );
}

void action_t::cancel()
{
  sim->print_debug( "{} {} is canceled", *player, *this );

  if ( channeled )
  {
    if ( dot_t* d = find_dot( target ) )
      d->cancel();
  }

  bool was_busy = false;

  if ( player->queueing == this )
  {
    was_busy         = true;
    player->queueing = nullptr;
  }
  if ( player->executing == this )
  {
    was_busy          = true;
    player->executing = nullptr;
  }
  if ( player->channeling == this )
  {
    was_busy           = true;
    player->channeling = nullptr;
  }

  event_t::cancel( execute_event );
  event_t::cancel( queue_event );

  player->debuffs.casting->expire();

  if ( was_busy && player->arise_time >= timespan_t::zero() && !player->readying )
    player->schedule_ready();
}

void action_t::interrupt_action()
{
  sim->print_debug( "{} {} is interrupted", *player, *this );

  if ( player->executing == this )
    player->executing = nullptr;
  if ( player->queueing == this )
    player->queueing = nullptr;

  if ( player->channeling == this )
  {
    // Forcefully interrupting a channel should not incur the channel lag.
    interrupt_immediate_occurred = true;

    dot_t* dot = get_dot( execute_state->target );
    assert( dot->is_ticking() );

    // Some channels fully reset the player's auto attack timer until the end of the channel
    // If we interrupt the channel, we need to unpause this early by the remaining delay time
    if ( !background && special && reset_auto_attack )
    {
      player->delay_auto_attacks( -dot->remains() );
    }

    dot->cancel();
  }

  if ( !background && execute_event )
  {
    // Interrupting a cast resets GCD, allowing the player to start doing
    // something else right away. The delay between interrupting a cast
    // and starting a new cast seems to be twice the current latency.
    player->gcd_ready = std::min( player->gcd_ready, sim->current_time() + 2 * rng().gauss( player->world_lag ) );

    // While an ability is casting, the auto_attack is paused during schedule_execute
    // If we interrupt the cast, we need to unpause this early by the remaining delay time
    if ( special && !proc && ( interrupt_auto_attack || reset_auto_attack ) )
    {
      player->delay_auto_attacks( -execute_event->remains() );
    }
  }

  event_t::cancel( execute_event );
  event_t::cancel( queue_event );

  player->debuffs.casting->expire();
}

void action_t::check_spec( specialization_e necessary_spec )
{
  if ( player->specialization() != necessary_spec )
  {
    sim->errorf( "Player %s attempting to execute action %s without %s spec.\n", player->name(), name(),
                 dbc::specialization_string( necessary_spec ) );

    background = true;  // prevent action from being executed
  }
}

// action_t::check_spec =====================================================

void action_t::check_spell( const spell_data_t* sp )
{
  if ( !sp->ok() )
  {
    sim->errorf( "Player %s attempting to execute action %s without spell ok().\n", player->name(), name() );

    background = true;  // prevent action from being executed
  }
}

std::unique_ptr<expr_t> action_t::create_expression( std::string_view name )
{
  class action_expr_t : public expr_t
  {
  public:
    action_t& action;

    action_expr_t( std::string_view name, action_t& a ) : expr_t( name ), action( a )
    {
    }
  };

  class action_state_expr_t : public action_expr_t
  {
  public:
    action_state_t* state;
    action_state_expr_t( std::string_view name, action_t& a ) : action_expr_t( name, a ), state( a.get_state() )
    {
    }

    ~action_state_expr_t() override
    {
      delete state;
    }
  };

  class amount_expr_t : public action_state_expr_t
  {
  public:
    result_amount_type amount_type;
    result_e result_type;
    bool average_crit;

    amount_expr_t( std::string_view name, result_amount_type at, action_t& a, result_e rt = RESULT_NONE )
      : action_state_expr_t( name, a ), amount_type( at ), result_type( rt ), average_crit( false )
    {
      if ( result_type == RESULT_NONE )
      {
        result_type  = RESULT_HIT;
        average_crit = true;
      }

      state->n_targets    = 1;
      state->chain_target = 0;
      state->result       = result_type;
    }

    double evaluate() override
    {
      state->target = action.target;
      action.snapshot_state( state, amount_type );
      double a;
      if ( amount_type == result_amount_type::DMG_OVER_TIME || amount_type == result_amount_type::HEAL_OVER_TIME )
        a = action.calculate_tick_amount( state, 1.0 /* Assumes full tick & one stack */ );
      else
      {
        state->result_amount = action.calculate_direct_amount( state );
        if ( state->result == RESULT_CRIT )
        {
          state->result_amount = action.calculate_crit_damage_bonus( state );
        }
        if ( amount_type == result_amount_type::DMG_DIRECT )
          state->target->target_mitigation( action.get_school(), amount_type, state );
        a = state->result_amount;
      }

      if ( average_crit )
      {
        a *= 1.0 + clamp( state->crit_chance + state->target_crit_chance, 0.0, 1.0 ) *
                       action.composite_player_critical_multiplier( state );
      }
      return a;
    }
  };

  if ( name == "cast_time" )
    return make_mem_fn_expr( name, *this, &action_t::execute_time );

  if ( name == "ready" )
    return make_mem_fn_expr( name, *this, &action_t::ready );

  if ( name == "usable" )
    return make_mem_fn_expr( name, *cooldown, &cooldown_t::is_ready );

  if ( name == "cost" )
    return make_mem_fn_expr( name, *this, &action_t::cost );

  if ( name == "target" )
    return make_fn_expr( name, [this] { return target->actor_index; } );

  if ( name == "gcd" )
    return make_mem_fn_expr( name, *this, &action_t::gcd );

  if ( name == "cooldown" )
    return make_fn_expr( name, [this] { return cooldown_duration().total_seconds(); } );

  if ( name == "travel_time" )
    return make_mem_fn_expr( name, *this, &action_t::travel_time );

  if ( name == "available_targets" )
    return make_fn_expr( name, [ this ] { return target_list().size(); } );

  if ( name == "usable_in" )
  {
    return make_fn_expr( name, [this]() {
      if ( !cooldown->is_ready() )
      {
        return cooldown->remains().total_seconds();
      }
      auto ready_at     = ( cooldown->ready - cooldown->player->cooldown_tolerance() );
      auto current_time = cooldown->sim.current_time();
      if ( ready_at <= current_time )
      {
        return 0.0;
      }

      return ( ready_at - current_time ).total_seconds();
    } );
  }

  if ( name == "execute_time" )
  {
    struct execute_time_expr_t : public action_state_expr_t
    {
      execute_time_expr_t( action_t& a ) : action_state_expr_t( "execute_time", a )
      {
      }

      double evaluate() override
      {
        if ( action.channeled )
        {
          action.snapshot_state( state, result_amount_type::NONE );
          state->target = action.target;
          return action.composite_dot_duration( state ).total_seconds() + action.execute_time().total_seconds();
        }
        else
          return std::max( action.execute_time().total_seconds(), action.gcd().total_seconds() );
      }
    };
    return std::make_unique<execute_time_expr_t>( *this );
  }

  if ( name == "tick_time" )
  {
    struct tick_time_expr_t : public action_expr_t
    {
      tick_time_expr_t( action_t& a ) : action_expr_t( "tick_time", a )
      {
      }
      double evaluate() override
      {
        dot_t* dot = action.find_dot( action.target );
        if ( dot && dot->is_ticking() )
          return action.tick_time( dot->state ).total_seconds();
        else
          return 0.0;
      }
    };
    return std::make_unique<tick_time_expr_t>( *this );
  }

  if ( name == "new_tick_time" )
  {
    struct new_tick_time_expr_t : public action_state_expr_t
    {
      new_tick_time_expr_t( action_t& a ) : action_state_expr_t( "new_tick_time", a )
      {
      }
      double evaluate() override
      {
        action.snapshot_state( state, result_amount_type::DMG_OVER_TIME );
        return action.tick_time( state ).total_seconds();
      }
    };
    return std::make_unique<new_tick_time_expr_t>( *this );
  }

  if ( auto q = dot_t::create_expression( nullptr, this, this, name, true ) )
    return q;

  if ( name == "cooldown_react" )
  {
    return make_fn_expr( name, [this] { return cooldown->up() && cooldown->reset_react <= sim->current_time(); } );
  }

  if ( name == "cast_delay" )
  {
    struct cast_delay_expr_t : public action_expr_t
    {
      cast_delay_expr_t( action_t& a ) : action_expr_t( "cast_delay", a )
      {
      }
      double evaluate() override
      {
        if ( action.sim->debug )
        {
          action.sim->print_debug(
              "{} {} cast_delay(): can_react_at={} cur_time={}", *action.player,
              action,
              ( action.player->cast_delay_occurred + action.player->cast_delay_reaction ),
              action.sim->current_time() );
        }

        if ( action.player->cast_delay_occurred == timespan_t::zero() ||
             action.player->cast_delay_occurred + action.player->cast_delay_reaction < action.sim->current_time() )
          return true;
        else
          return false;
      }
    };
    return std::make_unique<cast_delay_expr_t>( *this );
  }

  if ( name == "tick_multiplier" )
  {
    struct tick_multiplier_expr_t : public action_state_expr_t
    {
      tick_multiplier_expr_t( action_t& a ) : action_state_expr_t( "tick_multiplier", a )
      {
        state->n_targets    = 1;
        state->chain_target = 0;
      }

      double evaluate() override
      {
        state->target = action.target;
        action.snapshot_state( state, result_amount_type::NONE );

        return action.composite_ta_multiplier( state );
      }
    };
    return std::make_unique<tick_multiplier_expr_t>( *this );
  }

  if ( name == "energize_amount" )
  {
    struct energize_amount_expr_t : public action_state_expr_t
    {
      energize_amount_expr_t( action_t& a ) : action_state_expr_t( "energize_amount", a )
      {
        state->n_targets = 1;
        state->chain_target = 0;
      }

      double evaluate() override
      {
        state->target = action.target;
        action.snapshot_state( state, result_amount_type::NONE );

        int num_targets = state->n_targets;

        if ( action.energize_type_() == action_energize::PER_HIT )
        {
          num_targets = action.n_targets();
          if ( num_targets == -1 || num_targets > 0 )
          {
            action.target_cache.is_valid = false;
            auto max_targets = as<int>( action.target_list().size() );
            num_targets = ( num_targets < 0 ) ? max_targets : std::min( max_targets, num_targets );
          }

          state->n_targets = std::max( 1, num_targets );
        }

        return action.composite_energize_amount( state ) * state->n_targets;
      }
    };
    return std::make_unique<energize_amount_expr_t>( *this );
  }

  if ( name == "persistent_multiplier" )
  {
    struct persistent_multiplier_expr_t : public action_state_expr_t
    {
      persistent_multiplier_expr_t( action_t& a ) : action_state_expr_t( "persistent_multiplier", a )
      {
        state->n_targets    = 1;
        state->chain_target = 0;
      }

      double evaluate() override
      {
        state->target = action.target;
        action.snapshot_state( state, result_amount_type::NONE );

        return action.composite_persistent_multiplier( state );
      }
    };
    return std::make_unique<persistent_multiplier_expr_t>( *this );
  }

  if ( name == "charges" || name == "charges_fractional" || name == "max_charges" ||
       name == "recharge_time" || name == "full_recharge_time" )
  {
    return cooldown->create_expression( name );
  }

  if ( name == "damage" )
    return std::make_unique<amount_expr_t>( name, result_amount_type::DMG_DIRECT, *this );
  else if ( name == "hit_damage" )
    return std::make_unique<amount_expr_t>( name, result_amount_type::DMG_DIRECT, *this, RESULT_HIT );
  else if ( name == "crit_damage" )
    return std::make_unique<amount_expr_t>( name, result_amount_type::DMG_DIRECT, *this, RESULT_CRIT );
  else if ( name == "hit_heal" )
    return std::make_unique<amount_expr_t>( name, result_amount_type::HEAL_DIRECT, *this, RESULT_HIT );
  else if ( name == "crit_heal" )
    return std::make_unique<amount_expr_t>( name, result_amount_type::HEAL_DIRECT, *this, RESULT_CRIT );
  else if ( name == "tick_damage" )
    return std::make_unique<amount_expr_t>( name, result_amount_type::DMG_OVER_TIME, *this );
  else if ( name == "hit_tick_damage" )
    return std::make_unique<amount_expr_t>( name, result_amount_type::DMG_OVER_TIME, *this, RESULT_HIT );
  else if ( name == "crit_tick_damage" )
    return std::make_unique<amount_expr_t>( name, result_amount_type::DMG_OVER_TIME, *this, RESULT_CRIT );
  else if ( name == "tick_heal" )
    return std::make_unique<amount_expr_t>( name, result_amount_type::HEAL_OVER_TIME, *this, RESULT_HIT );
  else if ( name == "crit_tick_heal" )
    return std::make_unique<amount_expr_t>( name, result_amount_type::HEAL_OVER_TIME, *this, RESULT_CRIT );

  if ( name == "crit_pct_current" )
  {
    struct crit_pct_current_expr_t : public action_state_expr_t
    {
      crit_pct_current_expr_t( action_t& a ) : action_state_expr_t( "crit_pct_current", a )
      {
        state->n_targets    = 1;
        state->chain_target = 0;
      }

      double evaluate() override
      {
        state->target = action.target;
        action.snapshot_state( state, result_amount_type::NONE );

        return std::min( 100.0, state->composite_crit_chance() * 100.0 );
      }
    };
    return std::make_unique<crit_pct_current_expr_t>( *this );
  }

  if ( name == "multiplier" )
  {
    return make_fn_expr( name, [this] {
      double multiplier = 0.0;
      for ( auto base_school : base_schools )
      {
        double v = player->cache.player_multiplier( base_school );
        if ( v > multiplier )
        {
          multiplier = v;
        }
      }

      return multiplier;
    } );
  }

  if ( name == "primary_target" )
  {
    return make_fn_expr( name, [this]() { return player->target == target; } );
  }

  if ( name == "enabled" )
  {
    return expr_t::create_constant( name, data().found() );
  }

  if ( name == "casting" )
  {
    return make_fn_expr( name, [ this ] ()
    {
      return player->executing && player->executing->execute_event && player->executing->internal_id == internal_id;
    } );
  }

  if ( name == "cast_remains" )
  {
    return make_fn_expr( name, [ this ] ()
    {
      if ( player->executing && player->executing->execute_event && player->executing->internal_id == internal_id )
        return player->executing->execute_event->remains().total_seconds();

      return 0.0;
    } );
  }

  if ( name == "channeling" )
  {
    return make_fn_expr( name, [ this ] ()
    {
      return player->channeling && player->channeling->internal_id == internal_id;
    } );
  }

  if ( name == "channel_remains" )
  {
    return make_fn_expr( name, [ this ] ()
    {
      if ( player->channeling && player->channeling->internal_id == internal_id )
        return player->channeling->get_dot()->remains().total_seconds();

      return 0.0;
    } );
  }

  if ( name == "executing" )
  {
    return make_fn_expr( name, [ this ] ()
    {
      action_t* current_action = player->executing ? player->executing : player->channeling;
      return current_action && current_action->internal_id == internal_id;
    } );
  }

  if ( name == "execute_remains" )
  {
    return make_fn_expr( name, [ this ] ()
    {
      if ( player->executing && player->executing->execute_event && player->executing->internal_id == internal_id )
        return player->executing->execute_event->remains().total_seconds();

      if ( player->channeling && player->channeling->internal_id == internal_id )
        return player->channeling->get_dot()->remains().total_seconds();

      return 0.0;
    } );
  }

  if ( name == "last_used" )
  {
    std::vector<action_t*> last_used_list;
    for ( size_t i = 0; i < player->action_list.size(); ++i )
    {
      action_t* action = player->action_list[ i ];
      if ( action->name_str == name_str )
        last_used_list.push_back( action );
    }

    struct last_used_expr_t : public expr_t
    {
      const std::vector<action_t*> action_list;
      last_used_expr_t( std::vector<action_t*> al ) : expr_t( "last_used" ), action_list( std::move( al ) )
      {
      }
      double evaluate() override
      {
        timespan_t t = timespan_t::min();
        for (auto a : action_list)
        {
          if ( a->last_used > t )
            t = a->last_used;
        }
        return t.total_seconds();
      }
      bool is_constant() override
      {
        return action_list.empty();
      }
    };
    return std::make_unique<last_used_expr_t>( std::move(last_used_list) );
  }

  auto splits = util::string_split<std::string_view>( name, "." );

  if ( splits.size() == 2 )
  {
    if ( splits[ 0 ] == "active_enemies_within" )
    {
      if ( sim->distance_targeting_enabled )
      {
        struct active_enemies_t : public action_expr_t
        {
          double yards_from_player;
          int num_targets;
          active_enemies_t( action_t& a, std::string_view yards ) : action_expr_t( "active_enemies_within", a )
          {
            yards_from_player = util::to_int( yards );
            num_targets       = 0;
          }

          double evaluate() override
          {
            num_targets = 0;
            for ( auto* t : action.player->sim->target_non_sleeping_list )
            {
              if ( action.player->get_player_distance( *t ) <= yards_from_player )
                num_targets++;
            }
            return num_targets;
          }
        };
        return std::make_unique<active_enemies_t>( *this, splits[ 1 ] );
      }
      else
      {  // If distance targeting is not enabled, default to active_enemies behavior.
        return make_ref_expr( name, sim->active_enemies );
      }
    }
    if ( splits[ 0 ] == "prev" )
    {
      struct prev_expr_t : public action_expr_t
      {
        action_t* prev;
        prev_expr_t( action_t& a, std::string_view prev_action )
          : action_expr_t( "prev", a ), prev( a.player->find_action( prev_action ) )
        {
        }
        double evaluate() override
        {
          if ( prev && action.player->last_foreground_action )
            return action.player->last_foreground_action->internal_id == prev->internal_id;
          return false;
        }
        bool is_constant() override
        {
          return !prev;
        }
      };
      return std::make_unique<prev_expr_t>( *this, splits[ 1 ] );
    }
    else if ( splits[ 0 ] == "prev_off_gcd" )
    {
      struct prev_gcd_expr_t : public action_expr_t
      {
        action_t* previously_off_gcd;
        prev_gcd_expr_t( action_t& a, std::string_view offgcdaction )
          : action_expr_t( "prev_off_gcd", a ), previously_off_gcd( a.player->find_action( offgcdaction ) )
        {
        }
        double evaluate() override
        {
          if ( previously_off_gcd != nullptr && !action.player->off_gcdactions.empty() )
          {
            for ( const auto* off_gcdaction : action.player->off_gcdactions )
            {
              if ( off_gcdaction->internal_id == previously_off_gcd->internal_id )
                return true;
            }
          }
          return false;
        }
        bool is_constant() override
        {
          return !previously_off_gcd;
        }
      };
      return std::make_unique<prev_gcd_expr_t>( *this, splits[ 1 ] );
    }
    else if ( splits[ 0 ] == "gcd" )
    {
      if ( splits[ 1 ] == "max" )
      {
        struct gcd_expr_t : public action_expr_t
        {
          timespan_t gcd_time;
          gcd_expr_t( action_t& a ) : action_expr_t( "gcd", a ), gcd_time( 0_ms )
          {
          }
          double evaluate() override
          {
            gcd_time = action.player->base_gcd;
            if ( action.player->cache.attack_haste() < action.player->cache.spell_haste() )
              gcd_time *= action.player->cache.attack_haste();
            else
              gcd_time *= action.player->cache.spell_haste();

            auto min_gcd = action.min_gcd;
            if ( min_gcd == 0_ms )
            {
              min_gcd = 750_ms;
            }

            if ( gcd_time < min_gcd )
              gcd_time = min_gcd;
            return gcd_time.total_seconds();
          }
        };
        return std::make_unique<gcd_expr_t>( *this );
      }
      else if ( splits[ 1 ] == "remains" )
      {
        struct gcd_remains_expr_t : public action_expr_t
        {
          double gcd_remains;
          gcd_remains_expr_t( action_t& a ) : action_expr_t( "gcd", a ), gcd_remains( 0 )
          {
          }
          double evaluate() override
          {
            gcd_remains = ( action.player->gcd_ready - action.sim->current_time() ).total_seconds();
            if ( gcd_remains < 0 )  // It's possible for this to return negative numbers.
              gcd_remains = 0;
            return gcd_remains;
          }
        };
        return std::make_unique<gcd_remains_expr_t>( *this );
      }
      throw std::invalid_argument( fmt::format( "Unsupported gcd expression '{}'.", splits[ 1 ] ) );
    }
  }

  if ( splits.size() <= 2 && splits[ 0 ] == "spell_targets" )
  {
    if ( sim->distance_targeting_enabled )
    {
      struct spell_targets_t : public expr_t
      {
        action_t* spell;
        action_t& original_spell;
        const std::string name_of_spell;
        bool second_attempt;
        spell_targets_t( action_t& a, std::string_view spell_name )
          : expr_t( "spell_targets" ), original_spell( a ), name_of_spell( spell_name ), second_attempt( false )
        {
          spell = a.player->find_action( spell_name );
          if ( !spell )
          {
            for ( const auto* pet : a.player->pet_list )
            {
              spell = pet->find_action( spell_name );
            }
          }
        }

        // Evaluate spell_target spell and restore original state after evaluation
        double evaluate_spell() const
        {
          auto original_target = spell->target;
          spell->target = original_spell.get_expression_target();
          spell->target_cache.is_valid = false;
          auto n_targets = spell->target_list().size();
          spell->target = original_target;
          spell->target_cache.is_valid = false;

          return static_cast<double>( n_targets );
        }

        double evaluate() override
        {
          if ( spell )
          {
            return evaluate_spell();
          }
          else if ( !second_attempt )
          {  // There are cases where spell_targets may be looking for a spell that hasn't had an action created yet.
            // This allows it to check one more time during the sims runtime, just in case the action has been created.
            spell = original_spell.player->find_action( name_of_spell );
            if ( !spell )
            {
              for (auto & pet : original_spell.player->pet_list)
              {
                spell = pet->find_action( name_of_spell );
              }
            }
            if ( !spell )
            {
              original_spell.sim->error( "Warning: {} used invalid spell_targets action \"{}\"",
                                          original_spell.player->name(), name_of_spell );
            }
            else
            {
              return evaluate_spell();
            }
            second_attempt = true;
          }
          return 0;
        }
      };
      return std::make_unique<spell_targets_t>( *this, splits.size() > 1 ? splits[ 1 ] : name_str );
    }
    else
    {
      if ( sim->target_list.size() == 1U && !sim->has_raid_event( "adds" ) && !sim->has_raid_event( "pull" ) )
      {
        return expr_t::create_constant( "spell_targets", 1.0 );
      }
      else
      {
        // If distance targeting is not enabled, default to active_enemies behavior.
        return make_ref_expr( name, sim->active_enemies );
      }
    }
  }

  if ( splits.size() == 3 && splits[ 0 ] == "prev_gcd" )
  {
    int gcd = util::to_int( splits[ 1 ] );

    struct prevgcd_expr_t : public action_expr_t
    {
      int gcd;
      action_t* previously_used;

      prevgcd_expr_t( action_t& a, int gcd, std::string_view prev_action )
        : action_expr_t( "prev_gcd", a ),
          gcd( gcd ),  // prevgcd.1.action will mean 1 gcd ago, prevgcd.2.action will mean 2 gcds ago, etc.
          previously_used( a.player->find_action( prev_action ) )
      {
        if ( !previously_used )
        {
          a.sim->print_debug( "{} could not find action '{}' while setting up prev_gcd expression.", a.player->name(),
                              prev_action );
        }
      }

      bool is_constant() override
      {
        return !previously_used;
      }

      double evaluate() override
      {
        if ( !previously_used )
          return false;
        if ( action.player->prev_gcd_actions.empty() )
          return false;
        if ( as<int>( action.player->prev_gcd_actions.size() ) >= gcd )
          return ( *( action.player->prev_gcd_actions.end() - gcd ) )->internal_id == previously_used->internal_id;
        return false;
      }
    };
    return std::make_unique<prevgcd_expr_t>( *this, gcd, splits[ 2 ] );
  }

  if ( splits.size() == 3 && splits[ 0 ] == "dot" )
  {
    auto action = player->find_action( splits[ 1 ] );
    if ( !action )
    {
      return expr_t::create_constant( splits[ 2 ], 0 );
    }

    auto expr = dot_t::create_expression(nullptr, action, this, splits[ 2 ], true );
    if ( expr )
    {
      return expr;
    }
    else
    {
      throw std::invalid_argument( fmt::format( "Cannot create a valid dot expression from '{}'", splits[ 2 ] ) );
    }
  }

  if ( splits.size() == 3 && splits[ 0 ] == "enemy_dot" )
  {
    // simple by-pass to test
    auto dt_ = dot_t::create_expression( player->get_dot( splits[ 1 ], target ), this, this, splits[ 2 ], false );
    if ( dt_ )
      return dt_;

    // more complicated version, cycles through possible sources
    std::vector<std::unique_ptr<expr_t>> dot_expressions;
    for ( auto t : sim->target_list )
    {
      dot_t* d = player->get_dot( splits[ 1 ], t );
      dot_expressions.push_back( dot_t::create_expression( d, this, this, splits[ 2 ], false ) );
    }

    struct enemy_dots_expr_t : public expr_t
    {
      std::vector<std::unique_ptr<expr_t>> expr_list;

      enemy_dots_expr_t( std::vector<std::unique_ptr<expr_t>> expr_list )
        : expr_t( "enemy_dot" ), expr_list( std::move( expr_list ) )
      {}

      double evaluate() override
      {
        double ret = 0;
        for ( auto&& expr : expr_list )
        {
          double expr_result = expr->eval();
          if ( expr_result != 0 && ( expr_result < ret || ret == 0 ) )
            ret = expr_result;
        }
        return ret;
      }
      bool is_constant() override
      {
        return expr_list.empty();
      }
    };

    return std::make_unique<enemy_dots_expr_t>( std::move(dot_expressions) );
  }

  if ( splits.size() == 3 && splits[ 0 ] == "debuff" )
  {
    return buff_t::create_expression( splits[ 1 ], splits[ 2 ], *this );
  }

  if ( splits.size() >= 2 && splits[ 0 ] == "target" )
  {
    // Find target
    player_t* expr_target = nullptr;
    auto tail      = name.substr( splits[ 0 ].length() + 1 );
    if ( util::is_number( splits[ 1 ] ) )
    {
      sim->error(
          "target.#.* expressions are deprecated and may give unexpected results in simulations with dynamic targets.\n"
          "Please rewrite to a 'target_if' expression." );

      expr_target = find_target_by_number( util::to_int( splits[ 1 ] ) );

      if ( !expr_target )
        throw std::invalid_argument( fmt::format( "Cannot find target by number '{}'.", splits[ 1 ] ) );

      if ( splits.size() == 2 )
      {
        throw std::invalid_argument("Insufficient parameters for expression 'target.<number>.<expression>'");
      }

      tail = name.substr( splits[ 0 ].length() + splits[ 1 ].length() + 2 );
    }
    // Fake target distance
    if ( tail == "distance" )
      return make_ref_expr( "distance", player->base.distance );

    // Return target(.n).tail expression if we have a expression target
    if ( expr_target )
    {
      if ( auto e = expr_target->create_action_expression( *this, tail ) )
      {
        return e;
      }
    }

    // Ensure that we can create an expression, if not, bail out early
    {
      auto expr_ptr = target -> create_expression( tail );
      if ( expr_ptr == nullptr )
      {
        return nullptr;
      }
    }

    // Proxy target based expression, allowing "dynamic switching" of targets
    // for the "target.<expression>" expressions. Generates a suitable
    // expression on demand for each target during run-time.
    //
    // Note, if we ever go dynamic spawning of adds and such, this method needs
    // to change (evaluate() has to resize the pointer array run-time, instead
    // of statically sized one based on constructor).
    struct target_proxy_expr_t : public action_expr_t
    {
      std::vector<std::unique_ptr<expr_t>> proxy_expr;
      std::string suffix_expr_str;

      target_proxy_expr_t( action_t& a, std::string_view expr_str )
        : action_expr_t( "target_proxy_expr", a ), suffix_expr_str( expr_str )
      {
      }

      double evaluate() override
      {
        // In the case of player-targeted non-hostile actions, target.x expressions are not typically relevant
        // Assume in this case the intent is to use the player's target rather than the player for evaluation
        // For things such as self-heals, self.x (e.g. self.health.pct) expressions should be used
        player_t* target = action.get_expression_target();

        if ( proxy_expr.size() <= target->actor_index )
        {
          std::generate_n(std::back_inserter(proxy_expr), target->actor_index + 1 - proxy_expr.size(), []{ return std::unique_ptr<expr_t>(); });
        }

        auto& expr = proxy_expr[ target->actor_index ];

        if ( !expr )
        {
          expr = target->create_action_expression( action, suffix_expr_str );
          if ( !expr )
          {
            throw std::invalid_argument(
                fmt::format( "Cannot create dynamic target expression for target '{}' from '{}'.",
                             target->name(), suffix_expr_str ) );
          }
        }

        return expr->eval();
      }
    };

    return std::make_unique<target_proxy_expr_t>( *this, tail );
  }

  if ( ( splits.size() == 3 && splits[ 0 ] == "action" ) || splits[ 0 ] == "in_flight" ||
       splits[ 0 ] == "in_flight_to_target" || splits[ 0 ] == "in_flight_remains" || splits[ 0 ] == "in_flight_to_target_count" )
  {
    std::vector<action_t*> in_flight_list;
    bool in_flight_singleton = ( splits[ 0 ] == "in_flight" || splits[ 0 ] == "in_flight_to_target" ||
                                 splits[ 0 ] == "in_flight_remains" || splits[ 0 ] == "in_flight_to_target_count" );
    auto action_name  = ( in_flight_singleton ) ? name_str : splits[ 1 ];
    for ( size_t i = 0; i < player->action_list.size(); ++i )
    {
      action_t* action = player->action_list[ i ];
      if ( action->name_str == action_name )
      {
        if ( in_flight_singleton || splits[ 2 ] == "in_flight" ||
          splits[ 2 ] == "in_flight_to_target" || splits[ 2 ] == "in_flight_remains" )
        {
          in_flight_list.push_back( action );
        }
        else
        {
          return action->create_expression( splits[ 2 ] );
        }
      }
    }
    if ( !in_flight_list.empty() )
    {
      if ( splits[ 0 ] == "in_flight" || ( !in_flight_singleton && splits[ 2 ] == "in_flight" ) )
      {
        struct in_flight_multi_expr_t : public expr_t
        {
          const std::vector<action_t*> action_list;
          in_flight_multi_expr_t( std::vector<action_t*> al ) : expr_t( "in_flight" ), action_list( std::move( al ) )
          {
          }
          double evaluate() override
          {
            return range::any_of( action_list, []( const action_t* a ) { return a->has_travel_events(); } );
          }
          bool is_constant() override
          {
            return action_list.empty();
          }
        };
        return std::make_unique<in_flight_multi_expr_t>( std::move(in_flight_list) );
      }
      else if ( splits[ 0 ] == "in_flight_to_target" ||
                ( !in_flight_singleton && splits[ 2 ] == "in_flight_to_target" ) )
      {
        struct in_flight_to_target_multi_expr_t : public expr_t
        {
          const std::vector<action_t*> action_list;
          action_t& action;

          in_flight_to_target_multi_expr_t( std::vector<action_t*> al, action_t& a )
            : expr_t( "in_flight_to_target" ), action_list( std::move( al ) ), action( a )
          {
          }
          double evaluate() override
          {
            for (auto i : action_list)
            {
              if ( i->has_travel_events_for( action.target ) )
                return true;
            }
            return false;
          }
          bool is_constant() override
          {
            return action_list.empty();
          }
        };
        return std::make_unique<in_flight_to_target_multi_expr_t>( std::move(in_flight_list), *this );
      }
      else if ( splits[ 0 ] == "in_flight_to_target_count" ||
                ( !in_flight_singleton && splits[ 2 ] == "in_flight_to_target_count" ) )
      {
        struct in_flight_to_target_count_multi_expr_t : public expr_t
        {
          const std::vector<action_t*> action_list;
          action_t& action;

          in_flight_to_target_count_multi_expr_t( std::vector<action_t*> al, action_t& a )
            : expr_t( "in_flight_to_target_count" ), action_list( std::move( al ) ), action( a )
          {
          }

          double evaluate() override
          {
            auto count = 0;

            for ( auto i : action_list )
            {
              for ( const auto& travel_event : i->travel_events )
              {
                if ( travel_event->state->target == action.target )
                  count++;
              }
            }

            return count;
          }

          bool is_constant() override
          {
            return action_list.empty();
          }
        };
        return std::make_unique<in_flight_to_target_count_multi_expr_t>( std::move( in_flight_list ), *this );
      }
      else if ( splits[ 0 ] == "in_flight_remains" ||
        ( !in_flight_singleton && splits[ 2 ] == "in_flight_remains" ) )
      {
        struct in_flight_remains_multi_expr_t : public expr_t
        {
          const std::vector<action_t*> action_list;
          in_flight_remains_multi_expr_t( std::vector<action_t*> al ) :
            expr_t( "in_flight" ),
            action_list( std::move( al ) )
          { }

          double evaluate() override
          {
            bool event_found = false;
            timespan_t t = timespan_t::max();
            for ( auto a : action_list )
            {
              if ( a->has_travel_events() )
              {
                event_found = true;
                t = std::min( t, a->shortest_travel_event() );
              }
            }

            return event_found ? t.total_seconds() : 0.0;
          }
          bool is_constant() override
          {
            return action_list.empty();
          }
        };
        return std::make_unique<in_flight_remains_multi_expr_t>( std::move(in_flight_list) );
      }
    }
  }

  // necessary for self.target.*, self.dot.*
  if ( splits.size() >= 2 && splits[ 0 ] == "self" )
  {
    auto tail = name.substr( splits[ 0 ].length() + 1 );
    return player->create_action_expression( *this, tail );
  }

  // necessary for sim.target.*
  if ( splits.size() >= 2 && splits[ 0 ] == "sim" )
  {
    auto tail = name.substr( splits[ 0 ].length() + 1 );
    return sim->create_expression( tail );
  }

  return player->create_action_expression( *this, name );
}

double action_t::ppm_proc_chance( double PPM ) const
{
  if ( weapon )
  {
    return weapon->proc_chance_on_swing( PPM );
  }
  else
  {
    timespan_t t = execute_time();
    // timespan_t time = channeled ? base_tick_time : base_execute_time;

    if ( t == timespan_t::zero() )
      t = timespan_t::from_seconds( 1.5 );  // player -> base_gcd;

    return ( PPM * t.total_minutes() );
  }
}

timespan_t action_t::tick_time( const action_state_t* s ) const
{
  auto base = base_tick_time.base + base_tick_time.flat_add + tick_time_flat_modifier( s );
  if ( base <= 0_ms )
    return 0_ms;

  auto mul = base_tick_time.pct_mul * tick_time_pct_multiplier( s );
  if ( mul <= 0 )
    return 0_ms;

  if ( hasted_ticks )
    mul *= s->haste;

  // Tick time is rounded to nearest ms.
  // Assuming this applies to all tick time, including hasted duration dots. As tick time is used in calculation for
  // hasted duration (in order to ensure # of ticks match) using rounding for tick time can have a non-trivial impact
  // on short duration dots with a large number of ticks, such as eye beam.
  return timespan_t::from_millis( std::round( static_cast<double>( base.total_millis() ) * mul ) );
}

timespan_t action_t::tick_time_flat_modifier( const action_state_t* ) const
{
  return 0_ms;
}

double action_t::tick_time_pct_multiplier( const action_state_t* ) const
{
  return 1.0;
}

void action_t::snapshot_internal( action_state_t* state, unsigned flags, result_amount_type rt )
{
  assert( state );

  state->result_type = rt;

  if ( flags & STATE_CRIT )
    state->crit_chance = composite_crit_chance() * composite_crit_chance_multiplier();

  if ( flags & STATE_HASTE )
    state->haste = composite_haste();

  if ( flags & STATE_AP )
    state->attack_power = composite_total_attack_power();

  if ( flags & STATE_SP )
    state->spell_power = composite_total_spell_power();

  if ( flags & STATE_VERSATILITY )
    state->versatility = composite_versatility( state );

  if ( flags & STATE_MUL_SPELL_DA )
    state->da_multiplier = composite_da_multiplier( state );

  if ( flags & STATE_MUL_SPELL_TA )
    state->ta_multiplier = composite_ta_multiplier( state );

  if ( flags & STATE_ROLLING_TA )
    state->rolling_ta_multiplier = composite_rolling_ta_multiplier( state );

  if ( flags & STATE_MUL_PLAYER_DAM )
    state->player_multiplier = composite_player_multiplier( state );

  if ( flags & STATE_MUL_PERSISTENT )
    state->persistent_multiplier = composite_persistent_multiplier( state );

  if ( flags & STATE_MUL_PET )
    state->pet_multiplier = player->cast_pet()->composite_owner_pet_damage_multiplier( state );

  if ( flags & STATE_TGT_MUL_DA )
    state->target_da_multiplier = composite_target_da_multiplier( state->target );

  if ( flags & STATE_TGT_MUL_TA )
    state->target_ta_multiplier = composite_target_ta_multiplier( state->target );

  if ( flags & STATE_TGT_MUL_PET )
    state->target_pet_multiplier = player->cast_pet()->composite_owner_pet_target_damage_multiplier( state->target );

  if ( flags & STATE_TGT_CRIT )
    state->target_crit_chance = composite_target_crit_chance( state->target ) * composite_crit_chance_multiplier();

  if ( flags & STATE_TGT_MITG_DA )
    state->target_mitigation_da_multiplier = composite_target_mitigation( state->target, get_school() );

  if ( flags & STATE_TGT_MITG_TA )
    state->target_mitigation_ta_multiplier = composite_target_mitigation( state->target, get_school() );

  if ( flags & STATE_TGT_ARMOR )
    state->target_armor = composite_target_armor( state->target );
}

// action_t::composite_dot_duration =========================================

timespan_t action_t::composite_dot_duration( const action_state_t* s ) const
{
  auto base = dot_duration.base + dot_duration.flat_add + dot_duration_flat_modifier( s );
  if ( base <= 0_ms )
    return 0_ms;

  auto mul = dot_duration.pct_mul * dot_duration_pct_multiplier( s );
  if ( mul <= 0 )
    return 0_ms;

  if ( hasted_dot_duration )
  {
    // if duration and tick are both hasted, we rebuild the duration based on the tick time * number of ticks to ensure
    // that tick time rounding does not result in erroneous partial ticks.
    // TODO: determine if this should also be the case for hasted duration without hasted ticks
    if ( hasted_ticks )
    {
      // duplicate action_t::tick_time without haste modification.
      auto _tick_time =
        static_cast<double>(
          ( base_tick_time.base + base_tick_time.flat_add + tick_time_flat_modifier( s ) ).total_millis() ) *
        base_tick_time.pct_mul * tick_time_pct_multiplier( s );

      // determine base number of ticks
      auto _duration = static_cast<double>( base.total_millis() ) * mul;
      auto _num_ticks = _duration / _tick_time;

      // should we always check this in an integer? or error/warn if not?
      // assert( static_cast<double>( static_cast<int>( _num_ticks ) ) == _num_ticks );

      // rebuild duration based on hasted tick time * number of ticks
      return timespan_t::from_millis( std::round( _tick_time * s->haste ) * _num_ticks );
    }
    else
    {
      mul *= s->haste;
    }
  }

  // TODO: assumed to be rounded to ms like tick_time(), confirm if possible.
  return timespan_t::from_millis( std::round( static_cast<double>( base.total_millis() ) * mul ) );
}

timespan_t action_t::dot_duration_flat_modifier( const action_state_t* ) const
{
  return 0_ms;
}

double action_t::dot_duration_pct_multiplier( const action_state_t* ) const
{
  return 1.0;
}

event_t* action_t::start_action_execute_event( timespan_t t, action_state_t* state )
{
  return make_event<action_execute_event_t>( *sim, this, t, state );
}

void action_t::do_schedule_travel( action_state_t* state, timespan_t time_ )
{
  if ( time_ <= timespan_t::zero() )
  {
    impact( state );
    action_state_t::release( state );
  }
  else
  {
    sim->print_log( "{} schedules travel ({}) for {}", *player, time_, *this );

    travel_events.push_back( make_event<travel_event_t>( *sim, this, state, time_ ) );
  }
}

void action_t::schedule_travel( action_state_t* s )
{
  if ( !execute_state )
    execute_state = get_state();

  execute_state->copy_state( s );

  // This is used for spells that don't use the typical player ---> main target travel time.
  time_to_travel = distance_targeting_travel_time( s );
  if ( time_to_travel == timespan_t::zero() )
    time_to_travel = travel_time();

  do_schedule_travel( s, time_to_travel );

  if ( result_is_hit( s->result ) )
  {
    hit_any_target = true;
    num_targets_hit++;
  }
}

void action_t::impact( action_state_t* s )
{
  // Note, Critical damage bonus for direct amounts is computed on impact, instead of cast finish.
  s->result_amount = calculate_crit_damage_bonus( s );

  assess_damage( ( type == ACTION_HEAL || type == ACTION_ABSORB ) ? result_amount_type::HEAL_DIRECT : result_amount_type::DMG_DIRECT, s );

  if ( result_is_hit( s->result ) )
  {
    trigger_dot( s );

    if ( impact_action )
    {
      assert( !impact_action->pre_execute_state );
      impact_action->set_target( s->target );
      impact_action->execute();
    }
  }
  else
  {
    sim->print_log( "Target {} avoids {} {} ({})", *s->target, *player, *this, s->result );
  }
}

void action_t::trigger_dot( action_state_t* s )
{
  timespan_t duration = composite_dot_duration( s );
  if ( duration <= timespan_t::zero() )
    return;

  // To simulate precasting HoTs, remove one tick worth of duration if precombat.
  // We also add a fake zero_tick in dot_t::check_tick_zero().
  if ( !harmful && is_precombat && !tick_zero && !tick_on_application )
    duration -= tick_time( s );

  dot_t* dot = get_dot( s->target );

  if ( dot_behavior == DOT_CLIP )
    dot->cancel();

  dot->current_action = this;
  dot->max_stack      = dot_max_stack;

  if ( !dot->state )
    dot->state = get_state();
  dot->state->copy_state( s );

  if ( !dot->is_ticking() )
  {
    if ( get_school() == SCHOOL_PHYSICAL && harmful )
    {
      if ( buff_t* b = s->target->debuffs.bleeding )
      {
        if ( b->current_value > 0 )
        {
          b->current_value += 1.0;
        }
        else
        {
          b->start( 1, 1.0 );
        }
      }
    }
  }

  dot->trigger( duration );
}

/**
 * Determine if a travel event for given target currently exists.
 */
bool action_t::has_travel_events_for( const player_t* t ) const
{
  for ( const auto& travel_event : travel_events )
  {
    if ( travel_event->state->target == t )
      return true;
  }

  return false;
}

/**
 * Determine the remaining duration of the next travel event.
 */
timespan_t action_t::shortest_travel_event() const
{
  if ( travel_events.empty() )
    return timespan_t::zero();

  timespan_t t = timespan_t::max();
  for ( const auto& travel_event : travel_events )
    t = std::min( t, travel_event->remains() );

  return t;
}

void action_t::remove_travel_event( travel_event_t* e )
{
  std::vector<travel_event_t*>::iterator pos = range::find( travel_events, e );
  if ( pos != travel_events.end() )
    erase_unordered( travel_events, pos );
}

void action_t::do_teleport( action_state_t* state )
{
  player->teleport( composite_teleport_distance( state ) );
}

/**
 * Calculates the new dot length after a refresh
 * Necessary because we have both pandemic behaviour ( last 30% of the dot are preserved )
 * and old Cata/MoP behavior ( only time to the next tick is preserved )
 */
timespan_t action_t::calculate_dot_refresh_duration( const dot_t* dot, timespan_t triggered_duration ) const
{
  switch ( dot_behavior )
  {
    case dot_behavior_e::DOT_REFRESH_PANDEMIC:
      return std::max( dot->remains(), std::min( triggered_duration * 0.3, dot->remains() ) + triggered_duration );
    case dot_behavior_e::DOT_ROLLING:
      if ( dot->ticks_left_fractional() < 1.0 )
        return triggered_duration;
      return dot->time_to_next_full_tick() + triggered_duration;
    case dot_behavior_e::DOT_REFRESH_DURATION:
      return dot->time_to_next_full_tick() + triggered_duration;
    case dot_behavior_e::DOT_EXTEND:
      return dot->remains() + triggered_duration;
    case dot_behavior_e::DOT_NONE:
      return dot->remains();
    case dot_behavior_e::DOT_CLIP:
    default:
      return triggered_duration;
  }
}

bool action_t::dot_refreshable( const dot_t* dot, timespan_t triggered_duration ) const
{
  switch ( dot_behavior )
  {
    case dot_behavior_e::DOT_REFRESH_PANDEMIC:
      return dot->remains() <= triggered_duration * 0.3;
    case dot_behavior_e::DOT_REFRESH_DURATION:
      return dot->ticks_left() <= 1;
    case dot_behavior_e::DOT_EXTEND:
    case dot_behavior_e::DOT_ROLLING:
      return true;
    case dot_behavior_e::DOT_NONE:
    case dot_behavior_e::DOT_CLIP:
    default:
      return false;
  }
}

call_action_list_t::call_action_list_t( player_t* player, util::string_view options_str )
  : action_t( ACTION_CALL, "call_action_list", player, spell_data_t::nil() ), alist( nullptr )
{
  std::string alist_name;
  int randomtoggle = 0;
  add_option( opt_string( "name", alist_name ) );
  add_option( opt_int( "random", randomtoggle ) );
  parse_options( options_str );

  ignore_false_positive = true;  // Truly terrible things could happen if a false positive comes back on this.
  use_off_gcd = true;
  trigger_gcd = timespan_t::zero();
  use_while_casting = true;
  usable_while_casting = true;

  if ( alist_name.empty() )
  {
    sim->errorf( "Player %s uses call_action_list without specifying the name of the action list\n", player->name() );
    sim->cancel();
  }

  alist = player->find_action_priority_list( alist_name );

  if ( randomtoggle == 1 )
    alist->random = randomtoggle;

  if ( !alist )
  {
    sim->error( "{} uses call_action_list with unknown action list {}\n", *player, alist_name );
    sim->cancel();
  }
}

swap_action_list_t::swap_action_list_t( player_t* player, util::string_view options_str,
                                        util::string_view name ) :
    action_t( ACTION_OTHER, name, player ),
    alist( nullptr )
{
  std::string alist_name;
  int randomtoggle = 0;
  add_option( opt_string( "name", alist_name ) );
  add_option( opt_int( "random", randomtoggle ) );
  parse_options( options_str );
  ignore_false_positive = true;
  if ( alist_name.empty() )
  {
    sim->error( "Player {} uses {} without specifying the name of the action list\n", player->name(), name );
    sim->cancel();
  }

  alist = player->find_action_priority_list( alist_name );

  if ( !alist )
  {
    sim->error( "Player {} uses {} with unknown action list {}\n", player->name(), name, alist_name );
    sim->cancel();
  }
  else if ( randomtoggle == 1 )
    alist->random = randomtoggle;

  trigger_gcd = timespan_t::zero();
  use_off_gcd = true;
  use_while_casting = true;
  usable_while_casting = true;
}

void swap_action_list_t::execute()
{
  sim->print_log( "{} swaps to action list {}", player->name(), alist->name_str );
  player->activate_action_list( alist, player->current_execute_type );
}

bool swap_action_list_t::ready()
{
  if ( player->active_action_list == alist )
    return false;

  return action_t::ready();
}

run_action_list_t::run_action_list_t( player_t* player, util::string_view options_str ) :
  swap_action_list_t( player, options_str, "run_action_list" )
{
  quiet                 = true;
  ignore_false_positive = true;
}

void run_action_list_t::execute()
{
  if ( sim->log )
    sim->out_log.print( "{} runs action list {}{}",
        *player,
        alist->name_str,
        player->readying ? " (off-gcd)" : "");

  if ( player->restore_action_list == nullptr )
  {
    player->restore_action_list = player->active_action_list;
    player->restore_action_list_type = player->current_execute_type;
  }
  player->activate_action_list( alist, player->current_execute_type );
}

/**
 * If the action is still ticking and all resources could be successfully consumed,
 * return true to indicate continued ticking.
 */
bool action_t::consume_cost_per_tick( const dot_t& dot )
{
  if ( !consume_per_tick_ )
  {
    return true;
  }

  if ( player->get_active_dots( &dot ) == 0 )
  {
    sim->print_debug( "{} {} ticking cost ends because dot is no longer ticking.", *player, *this );
    return false;
  }

  if ( player->resource_regeneration == regen_type::DYNAMIC )
    player->do_dynamic_regen( true );

  // Consume resources
  /*
   * Assumption: If not enough resource is available, still consume as much as possible
   * and cancel action afterwards.
   * philoptik 2015-03-23
   */
  bool cancel_action = false;
  for ( resource_e r = RESOURCE_NONE; r < RESOURCE_MAX; ++r )
  {
    double cost = cost_per_tick( r );
    if ( cost <= 0.0 )
      continue;

    bool enough_resource_available = player->resource_available( r, cost );
    if ( !enough_resource_available )
    {
      sim->print_log( "{} {} not enough resource for ticking cost {} {} (current={}). Going to cancel the action.",
                      *player, *this, cost, r, player->resources.current[ r ] );
    }

    last_resource_cost = player->resource_loss( r, cost, nullptr, this );
    stats->consume_resource( r, last_resource_cost );

    sim->print_log( "{} {} consumes ticking cost {} ({}) {} (current={}).", *player, *this, cost, last_resource_cost, r,
                    player->resources.current[ r ] );

    if ( !enough_resource_available )
    {
      cancel_action = true;
    }
  }

  if ( cancel_action )
  {
    cancel();
    range::for_each( target_list(), [this]( player_t* target ) {
      if ( dot_t* d = find_dot( target ) )
      {
        d->cancel();
      }
    } );
    return false;
  }

  return true;
}

dot_t* action_t::get_dot( player_t* t )
{
  if ( !t )
    t = target;
  if ( !t )
    return nullptr;

  dot_t*& dot = target_specific_dot[ t ];
  if ( !dot )
    dot = t->get_dot( name_str, player );
  return dot;
}

buff_t* action_t::get_debuff( player_t* t )
{
  if ( !t )
    t = target;
  if ( !t )
    return nullptr;

  buff_t*& debuff = target_specific_debuff[ t ];
  if ( !debuff )
    debuff = create_debuff( t );
  return debuff;
}

buff_t* action_t::create_debuff( player_t* t )
{
  std::string name_ = target_debuff->ok() ? target_debuff->name_cstr() : name_str;
  util::tokenize( name_ );
  return make_buff( actor_pair_t( t, player ),  name_, target_debuff );
}

// return s_data_reporting if available, otherwise fallback to s_data
const spell_data_t& action_t::data_reporting() const
{
  if ( s_data_reporting == spell_data_t::nil() )
    return ( *s_data );
  else
    return ( *s_data_reporting );
}

// returns name_str_reporting if available, otherwise fallback to name_str
const char* action_t::name_reporting() const
{
  if ( name_str_reporting.empty() )
    return name_str.c_str();
  else
    return name_str_reporting.c_str();
}

dot_t* action_t::find_dot( player_t* t ) const
{
  if ( !t )
    return nullptr;
  return target_specific_dot[ t ];
}

buff_t* action_t::find_debuff( player_t* t ) const
{
  if ( !t )
    return nullptr;

  return target_specific_debuff[ t ];
}

void action_t::add_child( action_t* child )
{
  child->parent_dot = target->get_dot( name_str, player );
  child_action.push_back( child );
  if ( child->parent_dot && range > 0 && child->radius > 0 && child->is_aoe() )
  {
    // If the parent spell has a range, the tick_action has a radius and is an aoe spell, then the tick action likely
    // also has a range. This will allow distance_target_t to correctly determine spells that radiate from the target,
    // instead of the player.
    child->range = range;
  }
  // Check for this so we don't create a circular reference in cases where the child action is already set up to use
  // the same stats object as the parent action.
  if ( this->stats != child->stats )
  {
    stats->add_child( child->stats );
  }
}

void action_t::add_option( std::unique_ptr<option_t> new_option )
{ options.insert( options.begin(), std::move(new_option) ); }

double action_t::composite_target_damage_vulnerability( player_t* t ) const
{
  double target_vulnerability = 0.0;
  double tmp;

  for ( auto base_school : base_schools )
  {
    tmp = t->composite_player_vulnerability( base_school );
    if ( tmp > target_vulnerability )
      target_vulnerability = tmp;
  }

  return target_vulnerability;
}

double action_t::composite_leech( const action_state_t* ) const
{
  return player->cache.leech();
}

double action_t::composite_run_speed() const
{
  return player->cache.run_speed();
}

double action_t::composite_avoidance() const
{
  return player->cache.avoidance();
}

double action_t::composite_corruption() const
{
  return player->cache.corruption();
}

double action_t::composite_corruption_resistance() const
{
  return player->cache.corruption_resistance();
}

double action_t::composite_total_corruption() const
{
  return player->composite_total_corruption();
}

double action_t::composite_player_multiplier( const action_state_t* ) const
{
  double player_school_multiplier = 0.0;
  double tmp;

  for ( auto base_school : base_schools )
  {
    tmp = player->cache.player_multiplier( base_school );
    if ( tmp > player_school_multiplier )
      player_school_multiplier = tmp;
  }

  return player_school_multiplier;
}

double action_t::composite_da_multiplier( const action_state_t* ) const
{
  return action_multiplier() * action_da_multiplier();
}

double action_t::composite_ta_multiplier( const action_state_t* ) const
{
  return action_multiplier() * action_ta_multiplier();
}

double action_t::composite_rolling_ta_multiplier( const action_state_t* s ) const
{
  // The behavior of Rolling Periodic DoTs can be modeled by keeping track of a multiplier.
  // A single instance of the DoT has a multiplier of 1.0 for all ticks. When the DoT is
  // refreshed early, the damage from any remaining ticks is rolled into multiplier so that
  // damage is not lost.
  double m = 1.0;

  dot_t* dot = find_dot( s->target );
  if ( dot && dot->is_ticking() )
  {
    double ticks_left = dot->ticks_left_fractional();
    timespan_t new_tick = tick_time( s );
    timespan_t new_duration = composite_dot_duration( s );
    double new_base_ticks = new_duration / new_tick;
    // Calculate ticks_left_fractional for the DoT after it is refreshed.
    double new_ticks_left = 1.0 + ( calculate_dot_refresh_duration( dot, new_duration ) - dot->time_to_next_full_tick() ) / new_tick;
    // Roll the multiplier for the old ticks that will be lost into a multiplier for the new DoT.
    m = ( ticks_left * dot->state->rolling_ta_multiplier + new_base_ticks ) / new_ticks_left;
    sim->print_debug( "{} {} rolling_ta_multiplier updated: old_multiplier={} to new_multiplier={} ticks_left={} new_base_ticks={} new_ticks_left={}.",
      *player, *this, dot->state->rolling_ta_multiplier, m, ticks_left, new_base_ticks, new_ticks_left );
  }

  return m;
}

/// Persistent modifiers that are snapshot at the start of the spell cast

double action_t::composite_persistent_multiplier(const action_state_t*) const
{
  return player->composite_persistent_multiplier(get_school());
}

double action_t::composite_target_mitigation(player_t* t, school_e s) const
{
  return t->composite_mitigation_multiplier(s);
}

double action_t::composite_player_critical_multiplier(const action_state_t* s) const
{
  return player->composite_player_critical_damage_multiplier(s);
}

bool action_t::has_movement_directionality() const
{
  // If ability has no movement restrictions, it'll be usable
  if ( movement_directionality == movement_direction_type::NONE || movement_directionality == movement_direction_type::OMNI )
    return true;
  else
  {
    movement_direction_type m = player->movement_direction();

    // If player isnt moving, allow everything
    if ( m == movement_direction_type::NONE )
      return true;
    else
      return m == movement_directionality;
  }
}

void action_t::reschedule_queue_event()
{
  if ( !queue_event )
  {
    return;
  }

  timespan_t new_queue_delay = cooldown->queue_delay();
  timespan_t remaining = queue_event->remains();

  // The actual queue delay did not change, so no need to do anything
  if ( new_queue_delay == remaining )
  {
    return;
  }

  sim->print_debug( "{} {} adjusting queue-delayed execution, old={} new={}",
      *player, *this, remaining, new_queue_delay );

  if ( new_queue_delay > remaining )
  {
    queue_event->reschedule( new_queue_delay );
  }
  else
  {
    execute_type et = debug_cast<queued_action_execute_event_t*>( queue_event )->type;

    event_t::cancel( queue_event );
    queue_event = make_event<queued_action_execute_event_t>( *sim, this, new_queue_delay, et );
  }
}
rng::rng_t& action_t::rng()
{ return sim->rng(); }

rng::rng_t& action_t::rng() const
{ return sim -> rng(); }

/**
 * Acquire a new target, where the context is the actor that sources the retarget event, and the actor-level candidate
 * is given as a parameter (selected by player_t::acquire_target). Default target acquirement simply assigns the
 * actor-selected candidate target to the current target. Event contains the retarget event type, context contains the
 * (optional) actor that triggered the event.
 */
void action_t::acquire_target( retarget_source event, player_t* /* context */, player_t* candidate_target )
{
  // Reset target cache every time target acquisition occurs for AOE spells
  if ( n_targets() != 0 )
  {
    target_cache.is_valid = false;
  }

  // Don't change targets if they are not of the same generic type (both enemies, or both friendlies)
  if ( target && candidate_target && target->is_enemy() != candidate_target->is_enemy() )
  {
    return;
  }

  // If the user has indicated a target number for the action, don't adjust targets
  if ( option.target_number > 0 )
  {
    return;
  }

  // Ongoing channels won't be retargeted during channel. Note that this works in all current cases,
  // since if the retargeting event is due to the target demise, the channel has already been
  // interrupted before (due to the demise).
  if ( player->channeling && player->channeling == this )
  {
    return;
  }

  // Don't swap targets if the action's current target is still alive, except in cases
  // where the actor has risen (been summoned, start of iteration etc).
  if ( event != retarget_source::SELF_ARISE &&
       target && !target->is_sleeping() && !target->debuffs.invulnerable->check() )
  {
    return;
  }

  if ( target != candidate_target )
  {
    sim->print_debug( "{} {} target change, current={} candidate={}", *player, *this,
                      target ? target->name() : "(none)", candidate_target ? candidate_target->name() : "(none)" );

    target = candidate_target;
  }
}

void action_t::activate()
{
  sim->target_non_sleeping_list.register_callback( [this]( player_t* ) { target_cache.is_valid = false; } );
}

// Change the target of the action, may require invalidation of target cache
void action_t::set_target( player_t* new_target )
{
  if ( n_targets() != 0 && target != new_target )
  {
    target_cache.is_valid = false;
  }

  target = new_target;
}

// Returns the target to use in expressions for an action
// Default behavior is for player-targeted abilities to return the player's target
player_t* action_t::get_expression_target()
{
  return ( target == player ) ? player->target : target;
}

void action_t::gain_energize_resource( resource_e resource_type, double amount, gain_t* g )
{
  player->resource_gain( resource_type, amount, g, this );
}

bool action_t::usable_during_current_cast() const
{
  if ( background || !usable_while_casting || !use_while_casting )
  {
    return false;
  }

  timespan_t threshold = timespan_t::max();
  if ( player->readying )
  {
    threshold = player->readying->occurs();
  }
  else if ( player->channeling )
  {
    assert( player->channeling->get_dot()->end_event && "player is channeling with its dot having no end event" );
    threshold = player->channeling->get_dot()->end_event->occurs();
    threshold += sim->channel_lag.mean + 4 * sim->channel_lag.stddev;
  }
  else if ( player->executing )
  {
    threshold = player->executing->execute_event->occurs();
  }

  return cooldown->queueable() < threshold;
}

bool action_t::usable_during_current_gcd() const
{
  if ( background || !use_off_gcd )
  {
    return false;
  }

  return player->readying && cooldown->queueable() < player->readying->occurs();
}

double action_t::last_tick_factor(const dot_t* /* d */, timespan_t time_to_tick, timespan_t duration) const
{
  return std::min( 1.0, duration / time_to_tick );
}

void sc_format_to( const action_t& action, fmt::format_context::iterator out )
{
  if ( action.sim->log_spell_id )
  {
    fmt::format_to( out, "Action {} ({})", action.name(), action.data().id() );
  }
  else
  {
    fmt::format_to( out, "Action {}", action.name() );
  }
}

bool action_t::execute_targeting(action_t* action) const
{
  if (action->sim->distance_targeting_enabled)
  {
    action->sim->print_debug(
        "{} action {} - Range {:.3f}, Radius {:.3f}, player location x={:.3f}, y={:.3f}, target: {} - location: "
        "x={:.3f}, y={:.3f}",
        *action->player, *action, action->range, action->radius, action->player->x_position, action->player->y_position,
        *action->target, action->target->x_position, action->target->y_position );

    if ( action->time_to_execute > 0_ms && action->range > 0.0 )
    {
      // No need to recheck if the execute time was zero.
      if ( action->player->get_player_distance( *action->target ) > action->range + action->target->combat_reach )
      {
        // Target is now out of range, we cannot finish the cast.
        return false;
      }
    }
  }
  return true;
}

// This returns a list of all targets currently in range.
std::vector<player_t*>& action_t::targets_in_range_list( std::vector<player_t*>& tl ) const
{
  size_t i = tl.size();
  while ( i > 0 )
  {
    i--;
    player_t* target_ = tl[ i ];
    if ( range > 0.0 && player->get_player_distance( *target_ ) > range )
    {
      tl.erase( tl.begin() + i );
    }
    else if ( !ground_aoe && target_->debuffs.invulnerable && target_->debuffs.invulnerable->check() )
    {
      // Cannot target invulnerable mobs, unless it's a ground aoe. It just won't do damage.
      tl.erase( tl.begin() + i );
    }
  }
  return tl;
}

/*
 treat targets as if they were on an x,y plane with coordinates other than 0,0.
 The simulation flag distance_targeting_enabled must be turned on for these to
 do anything.
*/
std::vector<player_t*>& action_t::check_distance_targeting( std::vector<player_t*>& tl ) const
{
  if ( sim->distance_targeting_enabled )
  {
    size_t i = tl.size();
    while ( i > 0 )
    {
      i--;
      player_t* t = tl[ i ];
      if ( t != target )
      {
        sim->print_debug(
            "{} action {} - Range {:.3f}, Radius {:.3f}, player location x={:.3f}, y={:.3f}, original target: {} - "
            "location: x={:.3f}, y={:.3f}, impact target: {} - location: x={:.3f}, y={:.3f}",
            *player, *this, range, radius, player->x_position, player->y_position, *target, target->x_position,
            target->y_position, *t, t->x_position, t->y_position );

        if ( ( ground_aoe && t->debuffs.flying && t->debuffs.flying->check() ) )
        {
          tl.erase( tl.begin() + i );
        }
        else if ( radius > 0 && range > 0 )
        {
          // Abilities with range/radius radiate from the target.
          if ( ground_aoe && parent_dot && parent_dot->is_ticking() )
          {
            // We need to check the parents dot for location.
            sim->print_debug( "parent_dot location: x={:.3f}, y={:.3f}", parent_dot->state->original_x,
                              parent_dot->state->original_y );

            if ( t->get_ground_aoe_distance( *parent_dot->state ) > radius + t->combat_reach )
            {
              tl.erase( tl.begin() + i );
            }
          }
          else if ( ground_aoe && execute_state )
          {
            if ( t->get_ground_aoe_distance( *execute_state ) > radius + t->combat_reach )
            {
              tl.erase( tl.begin() + i );  // We should just check the child.
            }
          }
          else if ( t->get_player_distance( *target ) > radius )
          {
            tl.erase( tl.begin() + i );
          }
        }
        // If they do not have a range, they are likely based on the distance from the player.
        else if ( radius > 0 && t->get_player_distance( *player ) > radius + t->combat_reach )
        {
          tl.erase( tl.begin() + i );
        }
        // If they only have a range, then they are a single target ability, or are also based on the distance from the
        // player.
        else if ( range > 0 && t->get_player_distance( *player ) > range + t->combat_reach )
        {
          tl.erase( tl.begin() + i );
        }
      }
    }
    if ( sim->debug )
    {
      sim->print_debug( "{} regenerated distance targetting cache for {} ({})", *player, signature_str, *this );
      for ( size_t j = 0; j < tl.size(); j++ )
      {
        sim->print_debug( "[{}, {} (id={}) x={:.3f} y={:.3f}]", static_cast<unsigned>( j ), *tl[ j ],
                          tl[ j ]->actor_index, tl[ j ]->x_position, tl[ j ]->y_position );
      }
    }
  }
  return tl;
}

player_t* action_t::select_target_if_target()
{
  if ( target_if_mode == TARGET_IF_NONE )
  {
    return nullptr;
  }

  if ( target_list().size() == 1 )
  {
    // If first is used, don't return a valid target unless the target_if
    // evaluates to non-zero
    if ( target_if_mode == TARGET_IF_FIRST )
    {
      return target_if_expr->evaluate() > 0 ? target : nullptr;
    }
    // For the rest (min/max), return the target
    return target;
  }

  std::vector<player_t*> master_list;
  if ( sim->distance_targeting_enabled )
  {
    if ( !target_cache.is_valid )
    {
      available_targets( target_cache.list );
      master_list = targets_in_range_list( target_cache.list );
      target_cache.is_valid = true;
    }
    else
    {
      master_list = target_cache.list;
    }

    sim->print_debug( "{} Number of targets found in range: {}", *player, master_list.size() );

    if ( master_list.size() <= 1 )
      return target;
  }
  else
  {
    master_list = target_list();
  }

  player_t* original_target = target;
  player_t* proposed_target = nullptr;
    
  double max_ = -std::numeric_limits<double>::infinity();
  double min_ = std::numeric_limits<double>::infinity();

  for ( auto p : master_list )
  {
    target = p;

    if ( !target_ready( target ) )
    {
      continue;
    }

    double v = target_if_expr->evaluate();

    if ( target_if_mode == TARGET_IF_FIRST && v != 0 )
    {
      proposed_target = target;
      break;
    }
    else if ( target_if_mode == TARGET_IF_MAX && v > max_ )
    {
      max_ = v;
      proposed_target = target;
    }
    else if ( target_if_mode == TARGET_IF_MIN && v < min_ )
    {
      min_ = v;
      proposed_target = target;
    }
  }

  // Restore original target
  target = original_target;

  // If "first available target" did not find anything useful, don't execute the
  // action
  if ( !proposed_target )
  {
    sim->print_debug( "{} target_if no target found for {}", *player, signature_str );

    return nullptr;
  }

  sim->print_debug( "{} target_if best: {} - original: {} - current target: {}", *player,
                    *proposed_target, *original_target, *target );

  return proposed_target;
}

timespan_t action_t::distance_targeting_travel_time( action_state_t* /*s*/ ) const
{
  return timespan_t::zero();
}

void action_t::html_customsection( report::sc_html_stream& os )
{
  // make a copy in case original list needs to be used later
  auto entries = affecting_list;

  for ( auto a : stats->action_list )
    if ( a != this )
      for ( const auto& entry : a->affecting_list )
        if ( !range::contains( entries, entry ) )
          entries.push_back( entry );

  if ( entries.size() )
  {
    os << "<div>\n"
        << "<h4>Affected By (Passive)</h4>\n"
        << "<table class=\"details nowrap\" style=\"width:min-content\">\n";

    os << "<tr>\n"
        << "<th class=\"small\">Type</th>\n"
        << "<th class=\"small\">Spell</th>\n"
        << "<th class=\"small\">ID</th>\n"
        << "<th class=\"small\">#</th>\n"
        << "<th class=\"small\">+/%</th>\n"
        << "<th class=\"small\">Value</th>\n"
        << "</tr>\n";

    for ( const auto& [ eff, val ] : entries )
    {
      std::string op_str;
      std::string type_str;
      std::string val_str;

      switch ( eff->subtype() )
      {
        case A_ADD_FLAT_LABEL_MODIFIER:
        case A_ADD_FLAT_MODIFIER:
          op_str = "ADD";
          type_str = spell_info::effect_property_str( eff );
          val_str = fmt::format( "{:.3f}", val );
          break;
        case A_ADD_PCT_LABEL_MODIFIER:
        case A_ADD_PCT_MODIFIER:
          op_str = "PCT";
          type_str = spell_info::effect_property_str( eff );
          val_str = fmt::format( "{:.1f}%", val * 100 );
          break;
        case A_MODIFY_SCHOOL:
          op_str = "SET";
          type_str = spell_info::effect_subtype_str( eff );
          val_str = util::school_type_string( eff->school_type() );
          break;
        default:
          op_str = "SET";
          type_str = spell_info::effect_subtype_str( eff );
          val_str = fmt::format( "{:.3f}", val );
          break;
      }

      os.format(
        "<tr><td>{}</td><td>{}</td><td class=\"right\">{}</td><td class=\"right\">{}</td><td>{}</td><td class=\"right\">{}</td></tr>",
        type_str,
        eff->spell()->name_cstr(),
        eff->spell()->id(),
        eff->index() + 1,
        op_str,
        val_str );
    }

    os << "</table>\n"
        << "</div>\n";
  }
}

void action_t::apply_affecting_aura( const spell_data_t* spell, const spell_data_t* modifier )
{
  if ( !spell->ok() )
  {
    return;
  }

  assert( ( spell->flags( SX_PASSIVE ) || spell->duration() < 0_ms ) && "only passive spells should be affecting actions." );

  for ( const spelleffect_data_t& effect : spell->effects() )
  {
    const spelleffect_data_t* mod = nullptr;

    if ( modifier && modifier->ok() )
    {
      for ( const auto& m_eff : modifier->effects() )
      {
        if ( m_eff.type() != E_APPLY_AURA )
          continue;

        switch ( m_eff.subtype() )
        {
          case A_ADD_FLAT_MODIFIER:
          case A_ADD_PCT_MODIFIER:
            if ( spell->affected_by( m_eff ) )
              break;
            else
              continue;
          case A_ADD_FLAT_LABEL_MODIFIER:
          case A_ADD_PCT_LABEL_MODIFIER:
            if ( spell->affected_by_label( m_eff ) )
              break;
            else
              continue;
          default:
            continue;
        }

        switch ( m_eff.property_type() )
        {
          case P_EFFECT_1: if ( effect.index() == 0 ) break; else continue;
          case P_EFFECT_2: if ( effect.index() == 1 ) break; else continue;
          case P_EFFECT_3: if ( effect.index() == 2 ) break; else continue;
          case P_EFFECT_4: if ( effect.index() == 3 ) break; else continue;
          case P_EFFECT_5: if ( effect.index() == 4 ) break; else continue;
          default:         continue;
        }

        mod = &m_eff;
        break;
      }
    }

    apply_affecting_effect( effect, mod );
  }
}

void action_t::apply_affecting_effect( const spelleffect_data_t& effect, const spelleffect_data_t* modifier )
{
  struct modified_effect_value_t
  {
    const spelleffect_data_t& effect;
    double value;

    modified_effect_value_t( const spelleffect_data_t& eff ) : effect( eff ), value( eff.base_value() ) {}

    double base_value() const
    { return value; }

    double percent() const
    { return value * ( 1 / 100.0 ); }

    timespan_t time_value() const
    { return timespan_t::from_millis( value ); }

    double resource( resource_e type ) const
    { return base_value() * effect.resource_multiplier( type ); }

    property_type_t property_type() const
    { return effect.property_type(); }
  };

  if ( !effect.ok() || effect.type() != E_APPLY_AURA )
    return;

  if ( !data().affected_by_all( effect ) )
  {
    return;
  }

  double value_ = 0;

  if ( sim->debug )
  {
    const spell_data_t& spell = *effect.spell();
    std::string desc_str;
    const auto& spell_text = player->dbc->spell_text( spell.id() );
    if ( spell_text.rank() )
      desc_str = fmt::format( " (desc={})", spell_text.rank() );

    sim->print_debug( "{} {} is affected by effect {} ({}{} (id={}) - effect #{})", *player, *this, effect.id(),
                      spell.name_cstr(), desc_str, spell.id(), effect.spell_effect_num() + 1 );
  }

  // Applies "Spell Effect N" auras if they directly affect damage auras
  auto apply_effect_n_multiplier = [ &value_, this ]( const modified_effect_value_t& effect, unsigned n ) {
    if ( is_direct_damage_effect( data().effectN( n ) ) )
    {
      base_dd_multiplier *= 1 + effect.percent();
      sim->print_debug( "{} base_dd_multiplier modified by {}% to {}", *this, effect.base_value(), base_dd_multiplier );
      value_ = effect.percent();
    }
    else if ( is_periodic_damage_effect( data().effectN( n ) ) )
    {
      base_td_multiplier *= 1 + effect.percent();
      sim->print_debug( "{} base_td_multiplier modified by {}% to {}", *this, effect.base_value(), base_td_multiplier );
      value_ = effect.percent();
    }
  };

  // Applies "Flat Modifier" and "Flat Modifier w/ Label" auras
  auto apply_flat_modifier = [ &value_, this ]( const modified_effect_value_t& effect ) {
    switch ( effect.property_type() )
    {
      case P_DURATION:
        if ( base_tick_time > timespan_t::zero() )
        {
          dot_duration += effect.time_value();
          sim->print_debug( "{} duration modified by {}", *this, effect.time_value() );
          value_ = effect.base_value();
        }
        if ( ground_aoe_duration > timespan_t::zero() )
        {
          ground_aoe_duration += effect.time_value();
          sim->print_debug( "{} ground aoe duration modified by {}", *this, effect.time_value() );
          value_ = effect.base_value();
        }
        break;

      case P_CAST_TIME:
        base_execute_time += effect.time_value();
        sim->print_debug( "{} cast time modified by {}", *this, effect.time_value() );
        value_ = effect.base_value();
        break;

      case P_RANGE:
        range += effect.base_value();
        sim->print_debug( "{} range modified by {}", *this, effect.base_value() );
        value_ = effect.base_value();
        break;

      case P_CRIT:
        base_crit += effect.percent();
        sim->print_debug( "{} base crit modified by {}", *this, effect.percent() );
        value_ = effect.percent();
        break;

      case P_COOLDOWN:
        if ( cooldown->action == this )
        {
          if ( data().charge_cooldown() > 0_ms )
          {
            internal_cooldown->duration += effect.time_value();
            if ( internal_cooldown->duration < timespan_t::zero() )
              internal_cooldown->duration = timespan_t::zero();
            sim->print_debug( "{} internal cooldown duration modified by {} to {} (due to being a charge cooldown)",
                              *this, effect.time_value(), internal_cooldown->duration );
          }
          else
          {
            cooldown->duration += effect.time_value();
            if ( cooldown->duration < timespan_t::zero() )
              cooldown->duration = timespan_t::zero();
            sim->print_debug( "{} cooldown duration modified by {} to {}", *this, effect.time_value(), cooldown->duration );
          }
          value_ = effect.base_value();
        }
        break;

      case P_RESOURCE_COST:
        base_costs[ resource_current ] += effect.resource( current_resource() );
        sim->print_debug( "{} base resource cost for resource {} (1) modified by {}", *this, resource_current,
                          effect.resource( current_resource() ) );
        value_ = effect.resource( current_resource() );
        break;

      case P_RESOURCE_COST_1:
      {
        if ( data().powers().size() < 2 )
          break;
        // Resource Cost 1 is actually the second resource as it's Zero Indexed.
        resource_e resource = data().powers()[ 1 ].resource();
        base_costs[ resource ] += effect.resource( resource );
        sim->print_debug( "{} base resource cost for resource {} (2) modified by {}", *this, resource,
                          effect.resource( resource ) );
        value_ = effect.resource( resource );
        break;
      }

      case P_RESOURCE_COST_2:
      {
        if ( data().powers().size() < 3 )
          break;
        // Resource Cost 2 is actually the third resource as it's Zero Indexed.
        resource_e resource = data().powers()[ 2 ].resource();
        base_costs[ resource ] += effect.resource( resource );
        sim->print_debug( "{} base resource cost for resource {} (3) modified by {}", *this, resource,
                          effect.resource( resource ) );
        value_ = effect.resource( resource );
        break;
      }

      case P_TARGET:
        assert( !( aoe == -1 || ( effect.base_value() < 0 && effect.base_value() > aoe ) ) );
        if ( aoe > 0 )
        {
          aoe += as<int>( effect.base_value() );
          sim->print_debug( "{} max target count modified by {} to {}", *this, effect.base_value(), aoe );
        }
        else if( aoe == 0 )
        {
          // This behavior depends on if the any effect has chain_targets of 1 defined in spell data or not
          // A bit roundabout, but we skip storing this in action_t::parse_effect_data() if it is only 1 
          bool has_chain_target = range::any_of( data().effects(), []( const spelleffect_data_t& effect ) {
            return effect.chain_target() == 1;
          } );
          aoe = as<int>( has_chain_target ) + as<int>( effect.base_value() );
          sim->print_debug( "{} max target count modified by {} to {}", *this, effect.base_value() - !has_chain_target, aoe );
        }
        value_ = effect.base_value();
        break;

      case P_TARGET_BONUS:
        chain_multiplier += effect.percent();
        sim->print_debug( "{} chain target multiplier modified by {} to {}", *this, effect.percent(), chain_multiplier );
        value_ = effect.percent();
        break;

      case P_GCD:
        trigger_gcd += effect.time_value();
        sim->print_debug( "{} trigger_gcd modified by {} to {}", *this, effect.time_value(), trigger_gcd );
        value_ = effect.base_value();
        break;

      case P_MAX_STACKS:
        if ( has_periodic_damage_effect( data() ) )
        {
          dot_max_stack += as<int>( effect.base_value() );
          sim->print_debug( "{} dot_max_stack modified by {} to {}", *this, effect.base_value(), dot_max_stack );
          value_ = effect.base_value();
        }
        break;

      case P_TICK_TIME:
        base_tick_time += effect.time_value();
        sim->print_debug( "{} base tick time modified by {} to {}", *this, effect.time_value(), base_tick_time );
        value_ = effect.base_value();
        if ( base_tick_time < 0_ms )
        {
          sim->print_debug( "WARNING: base tick time below 0ms!" );
          base_tick_time = 0_ms;
        }
        break;

      default:
        break;
    }
  };

  // Applies "Percent Modifier" and "Percent Modifier w/ Label" auras
  auto apply_percent_modifier = [ &value_, this ]( const modified_effect_value_t& effect ) {
    switch ( effect.property_type() )
    {
      case P_GENERIC:
        base_dd_multiplier *= 1.0 + effect.percent();
        sim->print_debug( "{} base_dd_multiplier modified by {}%", *this, effect.base_value() );
        value_ = effect.percent();
        break;

      case P_SPELL_POWER:
        sim->print_debug( "{} spell_power modified by {}%", *this, effect.base_value() );
        base_dd_multiplier *= 1.0 + effect.percent();
        base_td_multiplier *= 1.0 + effect.percent();
        value_ = effect.percent();
        break;

      case P_DURATION:
        if ( base_tick_time > timespan_t::zero() )
        {
          dot_duration *= 1.0 + effect.percent();
          sim->print_debug( "{} duration modified by {}%", *this, effect.base_value() );
          value_ = effect.percent();
        }
        if ( ground_aoe_duration > timespan_t::zero() )
        {
          ground_aoe_duration *= 1.0 + effect.percent();
          sim->print_debug( "{} ground aoe duration modified by {}%", *this, effect.base_value() );
          value_ = effect.percent();
        }
        break;

      case P_RADIUS:
        radius *= 1.0 + effect.percent();
        sim->print_debug( "{} radius modified by {}%", *this, effect.base_value() );
        value_ = effect.percent();
        break;

      case P_COOLDOWN:
        if ( data().charge_cooldown() <= 0_ms )
        {
          base_recharge_multiplier *= 1.0 + effect.percent();
          if ( base_recharge_multiplier <= 0 )
            cooldown->duration = timespan_t::zero();
          sim->print_debug( "{} cooldown recharge multiplier modified by {}%", *this, effect.base_value() );
          value_ = effect.percent();
        }
        break;

      case P_RESOURCE_COST:
        base_costs[ resource_current ] *= 1.0 + effect.percent();
        sim->print_debug( "{} base resource cost for resource {} (1) modified by {}%", *this, resource_current,
                          effect.base_value() );
        value_ = effect.percent();
        break;

      case P_RESOURCE_COST_1:
      {
        if ( data().powers().size() < 2 )
          break;
        // Zero Indexed, this is the second cost.
        resource_e resource = data().powers()[ 1 ].resource();
        base_costs[ resource ] *= 1.0 + effect.percent();
        sim->print_debug( "{} base resource cost for resource {} (2) modified by {}%", *this, resource,
                          effect.base_value() );
        value_ = effect.percent();
        break;
      }

      case P_RESOURCE_COST_2:
      {
        if ( data().powers().size() < 3 )
          break;
        // Zero Indexed, this is the third cost.
        resource_e resource = data().powers()[ 2 ].resource();
        base_costs[ resource ] *= 1.0 + effect.percent();
        sim->print_debug( "{} base resource cost for resource {} (3) modified by {}%", *this, resource,
                          effect.base_value() );
        value_ = effect.percent();
        break;
      }

      case P_TARGET_BONUS:
        chain_multiplier *= 1.0 + effect.percent();
        sim->print_debug( "{} chain target multiplier modified by {}% to {}", *this, effect.base_value(), chain_multiplier );
        value_ = effect.percent();
        break;

      case P_TICK_TIME:
        if ( base_tick_time > timespan_t::zero() )
        {
          base_tick_time *= 1.0 + effect.percent();
          sim->print_debug( "{} base tick time modified by {}%", *this, effect.base_value() );
        }
        value_ = effect.percent();
        break;

      case P_TICK_DAMAGE:
        base_td_multiplier *= 1.0 + effect.percent();
        sim->print_debug( "{} base_td_multiplier modified by {}%", *this, effect.base_value() );
        value_ = effect.percent();
        break;

      case P_CRIT_BONUS:
        crit_bonus_multiplier *= 1.0 + effect.percent();
        sim->print_debug( "{} critical bonus multiplier modified by {}%", *this, effect.base_value() );
        value_ = effect.percent();
        break;

      case P_CRIT:
        crit_chance_multiplier *= 1.0 + effect.percent();
        sim->print_debug( "{} critical strike chance multiplier modified by {}%", *this, effect.base_value() );
        value_ = effect.percent();
        break;

      case P_CAST_TIME:
        base_execute_time *= 1 + effect.percent();
        sim->print_debug( "{} cast time modified by {}% to {}", *this, effect.base_value(), base_execute_time );
        value_ = effect.percent();
        break;

      case P_GCD:
        trigger_gcd *= 1.0 + effect.percent();
        if ( trigger_gcd < timespan_t::zero() )
          trigger_gcd = timespan_t::zero();
        sim->print_debug( "{} trigger_gcd modified by {}% to {}", *this, effect.base_value(), trigger_gcd );
        value_ = effect.percent();
        break;

      default:
        break;
    }
  };

  auto m_effect = modified_effect_value_t( effect );
  if ( modifier && modifier->ok() )
  {
    if ( modifier->subtype() == A_ADD_FLAT_MODIFIER || modifier->subtype() == A_ADD_FLAT_LABEL_MODIFIER )
      m_effect.value += modifier->base_value();
    else if ( modifier->subtype() == A_ADD_PCT_MODIFIER || modifier->subtype() == A_ADD_PCT_LABEL_MODIFIER)
      m_effect.value *= 1 + modifier->percent();
  }

  // Standard Affected-by Auras
  if ( data().affected_by( effect ) )
  {
    switch ( effect.subtype() )
    {
      case A_HASTED_GCD:
        gcd_type = gcd_haste_type::ATTACK_HASTE;
        sim->print_debug( "{} gcd type set to attack_haste", *this );
        value_ = 1;
        break;

      case A_HASTED_COOLDOWN:
        cooldown->hasted = true;
        sim->print_debug( "{} cooldown set to hasted", *this );
        value_ = 1;
        break;

      case A_MODIFY_SCHOOL:
        set_school( effect.school_type() );
        value_ = effect.misc_value1();
        break;

      case A_ADD_FLAT_MODIFIER:
        apply_flat_modifier( m_effect );
        break;

      case A_ADD_PCT_MODIFIER:
        apply_percent_modifier( m_effect );
        break;

      default:
        break;
    }
  }
  // Label-based Auras
  else if ( data().affected_by_label( effect ) )
  {
    switch ( effect.subtype() )
    {
      case A_ADD_FLAT_LABEL_MODIFIER:
        apply_flat_modifier( m_effect );
        break;

      case A_ADD_PCT_LABEL_MODIFIER:
        apply_percent_modifier( m_effect );
        switch ( effect.property_type() )
        {
          case P_EFFECT_1:
            apply_effect_n_multiplier( m_effect, 1 );
            break;

          default:
            break;
        }
        break;

      default:
        break;
    }
  }
  // Category-based Auras
  else if ( data().affected_by_category( effect ) )
  {
    switch ( effect.subtype() )
    {
      case A_MODIFY_CATEGORY_COOLDOWN:
        if ( cooldown->action == this )
        {
          if ( data().charge_cooldown() > 0_ms )
          {
            internal_cooldown->duration += m_effect.time_value();
            if ( internal_cooldown->duration < timespan_t::zero() )
              internal_cooldown->duration = timespan_t::zero();
            sim->print_debug( "{} internal cooldown duration modified by {} to {} (due to being a charge cooldown)",
                              *this, m_effect.time_value(), internal_cooldown->duration );
          }
          else
          {
            cooldown->duration += m_effect.time_value();
            if ( cooldown->duration < timespan_t::zero() )
              cooldown->duration = timespan_t::zero();
            sim->print_debug( "{} cooldown duration modified by {} to {}", *this, m_effect.time_value(),
                              cooldown->duration );
          }
          value_ = m_effect.base_value();
        }
        break;

      case A_MOD_MAX_CHARGES:
        if ( cooldown->action == this && data().charge_cooldown() > 0_ms )
        {
          cooldown->charges += as<int>( m_effect.base_value() );
          sim->print_debug( "{} cooldown charges modified by {}", *this, as<int>( m_effect.base_value() ) );
          value_ = m_effect.base_value();
        }
        break;

      case A_HASTED_CATEGORY:
        cooldown->hasted = true;
        sim->print_debug( "{} cooldown set to hasted", *this );
        value_ = 1;
        break;

      case A_MOD_RECHARGE_TIME_CATEGORY:
        if ( cooldown->action == this )
        {
          if ( data().charge_cooldown() <= 0_ms )
          {
            sim->print_debug( "{} cooldown recharge time modifier ({}) ignored due to not being a charge cooldown",
                              *this, m_effect.time_value() );
          }
          else
          {
            cooldown->duration += m_effect.time_value();
            if ( cooldown->duration < timespan_t::zero() )
              cooldown->duration = timespan_t::zero();
            sim->print_debug( "{} cooldown recharge time modified by {}", *this, m_effect.time_value() );
          }
          value_ = m_effect.base_value();
        }
        break;

      case A_MOD_RECHARGE_TIME_PCT_CATEGORY:
        if ( data().charge_cooldown() > 0_ms )
        {
          base_recharge_multiplier *= 1 + m_effect.percent();
          if ( base_recharge_multiplier <= 0 )
          {
            cooldown->duration = timespan_t::zero();
          }
          sim->print_debug( "{} cooldown recharge multiplier modified by {}%", *this, m_effect.base_value() );
          value_ = m_effect.percent();
        }
        break;

      default:
        break;
    }
  }

  if ( value_ )
    affecting_list.emplace_back( &effect, value_ );
}

void action_t::apply_affecting_conduit( const conduit_data_t& conduit, int effect_num )
{
  assert( effect_num == -1 || effect_num > 0 );

  if ( !conduit.ok() )
    return;

  for ( size_t i = 1; i <= conduit->effect_count(); i++ )
  {
    if ( effect_num == -1 || as<size_t>( effect_num ) == i )
      apply_affecting_conduit_effect( conduit, i );
    else
      apply_affecting_effect( conduit->effectN( i ) );
  }
}

void action_t::apply_affecting_conduit_effect( const conduit_data_t& conduit, size_t effect_num )
{
  if ( !conduit.ok() )
    return;

  spelleffect_data_t effect = conduit->effectN( effect_num );
  effect._base_value = conduit.value();
  apply_affecting_effect( effect );
}

void action_t::execute_on_target( player_t* t, double amount )
{
  assert( t );
  set_target( t );

  if ( amount >= 0.0 )
    base_dd_min = base_dd_max = amount;

  execute();
}
