// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "interfaces/sc_js.hpp"
#include "player/player_talent_points.hpp"
#include "rapidjson/document.h"
#include "rapidjson/filewritestream.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "report/json/report_configuration.hpp"
#include "report/report_timer.hpp"
#include "report/reports.hpp"
#include "sim/iteration_data_entry.hpp"
#include "sim/plot.hpp"
#include "sim/profileset.hpp"
#include "sim/reforge_plot.hpp"
#include "sim/scale_factor_control.hpp"
#include "util/git_info.hpp"
#include "util/plot_data.hpp"

#include <ctime>

#include "simulationcraft.hpp"

using namespace rapidjson;
using namespace js;

namespace
{

/**
 * Template helper to add only non "default value" containers (sample data essentially) to the JSON
 * root.
 */
template <typename T>
auto add_non_default( JsonOutput root, util::string_view name, const T& container )
    -> std::void_t<decltype( container.mean() )>
{
  if ( container.mean() != 0 )
  {
    root[ name ] = container;
  }
}

template <typename T>
void add_non_default( JsonOutput root, util::string_view name, const T& v, const T& default_value )
{
  if ( v != default_value )
  {
    root[ name ] = v;
  }
}

/* Template helper to add only non-zero "containers" (sample data essentially) to the JSON root. */
template <typename T>
auto add_non_zero( JsonOutput root, util::string_view name, const T& container )
    -> std::void_t<decltype( container.mean() )>
{
  add_non_default( root, name, container );
}

void add_non_zero( JsonOutput root, util::string_view name, timespan_t v )
{
  add_non_default( root, name, v, timespan_t::zero() );
}

void add_non_zero( JsonOutput root, util::string_view name, double v )
{
  add_non_default( root, name, v, 0.0 );
}

void add_non_zero( JsonOutput root, util::string_view name, int v )
{
  add_non_default( root, name, v, 0 );
}

void add_non_zero( JsonOutput root, util::string_view name, unsigned v )
{
  add_non_default( root, name, v, 0U );
}

void add_non_zero( JsonOutput root, util::string_view name, bool v )
{
  add_non_default( root, name, v, false );
}

void add_non_zero( JsonOutput root, util::string_view name, util::string_view v )
{
  if ( !v.empty() )
  {
    root[ name ] = v;
  }
}

bool has_resources( const gain_t* gain )
{
  return range::any_of( gain->count, []( double v ) {
    return v != 0;
  } );
}

bool has_resources( const gain_t& gain )
{
  return has_resources( &gain );
}

void gain_to_json( JsonOutput root, const gain_t* g )
{
  root[ "name" ] = g->name();

  for ( resource_e r = RESOURCE_NONE; r < RESOURCE_MAX; ++r )
  {
    if ( g->count[ r ] == 0 )
    {
      continue;
    }
    auto node = root[ util::resource_type_string( r ) ];

    node[ "actual" ] = g->actual[ r ];
    node[ "overflow" ] = g->overflow[ r ];
    node[ "count" ] = g->count[ r ];
  }
}

void gain_to_json( JsonOutput root, const gain_t& g )
{
  gain_to_json( root, &g );
}

void gains_to_json( JsonOutput root, const player_t& p )
{
  root.make_array();

  range::for_each( p.gain_list, [ & ]( const gain_t* g ) {
    if ( !has_resources( g ) )
    {
      return;
    }

    gain_to_json( root.add(), g );
  } );
}

void to_json( JsonOutput root, const buff_t* b )
{
  root[ "name" ] = b->name();
  root[ "spell_name" ] = b->data_reporting().name_cstr();
  root[ "spell_school" ] = util::school_type_string( b->data_reporting().get_school_type() );
  add_non_zero( root, "spell", b->data_reporting().id() );
  if ( b->item )
  {
    root[ "item" ][ "id" ] = b->item->parsed.data.id;
    root[ "item" ][ "ilevel" ] = b->item->item_level();
  }

  if ( b->source && b->source != b->player )
  {
    root[ "source" ] = b->source->name();
  }

  if ( b->cooldown && b->cooldown->duration > timespan_t::zero() )
  {
    root[ "cooldown" ] = *b->cooldown;
  }

  root[ "start_count" ] = b->avg_start.mean();
  add_non_zero( root, "refresh_count", b->avg_refresh.mean() );
  add_non_zero( root, "interval", b->start_intervals.mean() );
  add_non_zero( root, "trigger", b->trigger_intervals.mean() );
  add_non_zero( root, "duration", b->duration_lengths.mean() );
  add_non_zero( root, "uptime", b->uptime_pct.mean() );
  add_non_zero( root, "benefit", b->benefit_pct.mean() );
  add_non_zero( root, "overflow_stacks", b->avg_overflow_count.mean() );
  add_non_zero( root, "overflow_total", b->avg_overflow_total.mean() );
  add_non_zero( root, "expire_count", b->avg_expire.mean() );

  add_non_default( root, "default_value", b->default_value, buff_t::DEFAULT_VALUE() );

  if ( b->sim->buff_uptime_timeline != 0 && b->uptime_array.mean() != 0 )
  {
    root[ "stack_uptime" ] = b->uptime_array;
  }
}

void buffs_to_json( JsonOutput root, const player_t& p )
{
  root.make_array();
  range::for_each( p.report_information.dynamic_buffs, [ & ]( const buff_t* b ) {
    if ( b->avg_start.sum() == 0 )
    {
      return;
    }
    to_json( root.add(), b );
  } );
}
void constant_buffs_to_json( JsonOutput root, const player_t& p )
{
  root.make_array();
  // constant buffs
  range::for_each( p.report_information.constant_buffs, [ & ]( const buff_t* b ) {
    if ( b->avg_start.sum() == 0 )
    {
      return;
    }
    to_json( root.add(), b );
  } );
}

void to_json( JsonOutput root, const stats_t::stats_results_t& sr )
{
  root[ "actual_amount" ] = sr.actual_amount;
  root[ "avg_actual_amount" ] = sr.avg_actual_amount;
  root[ "total_amount" ] = sr.total_amount;
  root[ "fight_actual_amount" ] = sr.fight_actual_amount;
  root[ "fight_total_amount" ] = sr.fight_total_amount;
  root[ "overkill_pct" ] = sr.overkill_pct;
  root[ "count" ] = sr.count;
  root[ "pct" ] = sr.pct;
}

void procs_to_json( JsonOutput root, const player_t& p )
{
  root.make_array();
  range::for_each( p.proc_list, [ & ]( const proc_t* proc ) {
    if ( proc->count.mean() == 0 )
    {
      return;
    }

    auto node = root.add();
    node[ "name" ] = proc->name();
    node[ "interval" ] = proc->interval_sum.mean();
    node[ "count" ] = proc->count.mean();
  } );
}

bool has_valid_stats( const std::vector<stats_t*>& stats_list, int level = 0 )
{
  return range::any_of( stats_list, [ level ]( const stats_t* stats ) {
    if ( stats->quiet )
    {
      return false;
    }

    if ( level == 0 )
    {
      if ( stats->num_executes.mean() == 0 && stats->compound_amount == 0 )
      {
        return false;
      }

      if ( stats->parent )
      {
        return false;
      }
    }
    else
    {
      if ( stats->compound_amount == 0 )
      {
        return false;
      }
    }

    return true;
  } );
}

void stats_to_json( JsonOutput root, const std::vector<stats_t*>& stats_list, int level = 0 )
{
  root.make_array();

  range::for_each( stats_list, [ & ]( const stats_t* s ) {
    if ( s->quiet )
    {
      return;
    }
    if ( level == 0 )
    {
      // top-level is just abilities that were executed or did some damage
      if ( s->num_executes.mean() == 0 && s->compound_amount == 0 )
      {
        return;
      }
      // probably a pet ability that matchs a player ability, e.g. Kill Command
      if ( s->parent )
      {
        return;
      }
    }
    else
    {
      // children show anything with an amount to catch dots and effects of spells
      if ( s->compound_amount == 0 )
      {
        return;
      }
    }

    auto node = root.add();

    // find action for this stat
    action_t* a = nullptr;
    for ( const auto& action : s->action_list )
    {
      if ( ( a = action )->id > 1 )
        break;
    }

    if ( a != nullptr )
    {
      node[ "id" ] = a->id;
      node[ "spell_name" ] = a->data_reporting().name_cstr();
      if ( a->item )
      {
        // grab item ID if available, can link trinket pets back to the item
        node[ "item_id" ] = a->item->parsed.data.id;
      }
    }
    node[ "name" ] = s->name();
    if ( s->school != SCHOOL_NONE )
    {
      node[ "school" ] = util::school_type_string( s->school );
    }
    node[ "type" ] = util::stats_type_string( s->type );

    if ( has_resources( s->resource_gain ) )
    {
      gain_to_json( node[ "resource_gain" ], s->resource_gain );
    }

    node[ "num_executes" ] = s->num_executes;
    node[ "compound_amount" ] = s->compound_amount;

    add_non_zero( node, "total_execute_time", s->total_execute_time );
    add_non_zero( node, "portion_aps", s->portion_aps );
    add_non_zero( node, "portion_apse", s->portion_apse );
    add_non_zero( node, "portion_amount", s->portion_amount );
    add_non_zero( node, "actual_amount", s->actual_amount );
    add_non_zero( node, "total_amount", s->total_amount );

    if ( s->num_executes.mean() > 1.0 )
    {
      node[ "total_intervals" ] = s->total_intervals;
    }

    add_non_zero( node, "num_ticks", s->num_ticks );
    add_non_zero( node, "num_tick_results", s->num_tick_results );
    add_non_zero( node, "total_tick_time", s->total_tick_time );
    add_non_zero( node, "num_refreshes", s->num_refreshes );

    add_non_zero( node, "num_direct_results", s->num_direct_results );

    for ( full_result_e r = FULLTYPE_NONE; r < FULLTYPE_MAX; ++r )
    {
      if ( s->direct_results[ r ].count.sum() != 0 )
      {
        to_json( node[ "direct_results" ][ util::full_result_type_string( r ) ], s->direct_results[ r ] );
      }
    }

    for ( result_e r = RESULT_NONE; r < RESULT_MAX; ++r )
    {
      if ( s->tick_results[ r ].count.sum() != 0 )
      {
        to_json( node[ "tick_results" ][ util::result_type_string( r ) ], s->tick_results[ r ] );
      }
    }

    // add children stats
    if ( has_valid_stats( s->children, level + 1 ) )
    {
      stats_to_json( node[ "children" ], s->children, level + 1 );
    }
  } );
}

void gear_to_json( JsonOutput root, const player_t& p )
{
  for ( slot_e slot = SLOT_MIN; slot < SLOT_MAX; slot++ )
  {
    const item_t& item = p.items[ slot ];
    if ( !item.active() || !item.has_stats() )
    {
      continue;
    }
    auto slotnode = root[ item.slot_name() ];

    slotnode[ "name" ] = item.name_str;
    slotnode[ "encoded_item" ] = item.encoded_item();
    slotnode[ "ilevel" ] = item.item_level();

    for ( size_t i = 0; i < std::size( item.parsed.data.stat_type_e ); i++ )
    {
      auto val = item.stat_value( i );
      if ( val <= 0 )
      {
        continue;
      }
      slotnode[ util::stat_type_string( item.stat( i ) ) ] = val;
    }
  }
}

void to_json( JsonOutput root, const player_t& p, const player_collected_data_t::buffed_stats_t& bs,
              const std::vector<resource_e>& relevant_resources )
{
  for ( attribute_e a = ATTRIBUTE_NONE; a < ATTRIBUTE_MAX; ++a )
  {
    add_non_zero( root[ "attribute" ], util::attribute_type_string( a ), bs.attribute[ a ] );
  }

  range::for_each( relevant_resources, [ &root, &bs ]( resource_e r ) {
    root[ "resources" ][ util::resource_type_string( r ) ] = bs.resource[ r ];
  } );

  add_non_zero( root[ "stats" ], "spell_power", bs.spell_power );
  add_non_zero( root[ "stats" ], "attack_power", bs.attack_power );
  add_non_zero( root[ "stats" ], "spell_crit", bs.spell_crit_chance );
  add_non_zero( root[ "stats" ], "attack_crit", bs.attack_crit_chance );
  add_non_zero( root[ "stats" ], "spell_haste", bs.spell_haste );
  add_non_zero( root[ "stats" ], "attack_haste", bs.attack_haste );
  add_non_zero( root[ "stats" ], "spell_cast_speed", bs.spell_cast_speed );
  add_non_zero( root[ "stats" ], "auto_attack_speed", bs.auto_attack_speed );

  add_non_zero( root[ "stats" ], "mastery_value", bs.mastery_value );
  add_non_zero( root[ "stats" ], "damage_versatility", bs.damage_versatility );
  add_non_zero( root[ "stats" ], "heal_versatility", bs.heal_versatility );
  add_non_zero( root[ "stats" ], "mitigation_versatility", bs.mitigation_versatility );

  // some of these secondaries are dupes from above. Duplication is intended to preserve backwards compatibility while
  // making a few names more consistent with the game. they're intended to be a quicker/simpler reference to match paper
  // doll stats in-game. crit and haste pick the max between melee/spell which seems to be the game logic

  add_non_zero( root[ "stats" ], "crit_rating",
                p.composite_melee_crit_rating() > p.composite_spell_crit_rating() ? p.composite_melee_crit_rating()
                                                                                  : p.composite_spell_crit_rating() );
  add_non_zero( root[ "stats" ], "crit_pct",
                bs.attack_crit_chance > bs.spell_crit_chance ? bs.attack_crit_chance : bs.spell_crit_chance );

  double attack_haste_pct = bs.attack_haste != 0 ? 1 / bs.attack_haste - 1 : 0;
  double spell_haste_pct = bs.spell_haste != 0 ? 1 / bs.spell_haste - 1 : 0;
  add_non_zero( root[ "stats" ], "haste_rating",
                p.composite_melee_haste_rating() > p.composite_spell_haste_rating()
                    ? p.composite_melee_haste_rating()
                    : p.composite_spell_haste_rating() );
  add_non_zero( root[ "stats" ], "haste_pct", attack_haste_pct > spell_haste_pct ? attack_haste_pct : spell_haste_pct );

  add_non_zero( root[ "stats" ], "mastery_rating", p.composite_mastery_rating() );
  add_non_zero( root[ "stats" ], "mastery_pct", bs.mastery_value );

  add_non_zero( root[ "stats" ], "versatility_rating", p.composite_damage_versatility_rating() );
  add_non_zero( root[ "stats" ], "versatility_pct", bs.damage_versatility );

  add_non_zero( root[ "stats" ], "avoidance_rating", p.composite_avoidance_rating() );
  add_non_zero( root[ "stats" ], "avoidance_pct", bs.avoidance );
  add_non_zero( root[ "stats" ], "leech_rating", p.composite_leech_rating() );
  add_non_zero( root[ "stats" ], "leech_pct", bs.leech );
  add_non_zero( root[ "stats" ], "speed_rating", p.composite_speed_rating() );
  add_non_zero( root[ "stats" ], "speed_pct", bs.run_speed );

  add_non_zero( root[ "stats" ], "manareg_per_second", bs.manareg_per_second );

  add_non_zero( root[ "stats" ], "armor", bs.armor );
  add_non_zero( root[ "stats" ], "dodge", bs.dodge );
  add_non_zero( root[ "stats" ], "parry", bs.parry );
  add_non_zero( root[ "stats" ], "block", bs.block );

  add_non_zero( root[ "stats" ], "corruption", bs.corruption );
  add_non_zero( root[ "stats" ], "corruption_resistance", bs.corruption_resistance );
}

void to_json( JsonOutput root, const ::report::json::report_configuration_t& report_configuration,
              const std::vector<player_collected_data_t::action_sequence_data_t>& asd,
              const std::vector<resource_e>& relevant_resources )
{
  root.make_array();

  range::for_each( asd, [ &root, &relevant_resources,
                          &report_configuration ]( const player_collected_data_t::action_sequence_data_t& entry ) {
    auto json = root.add();

    json[ "time" ] = entry.time;
    if ( entry.action )
    {
      json[ "id" ] = entry.action->id;
      json[ "name" ] = entry.action->name();
      json[ "target" ] = entry.action->harmful ? entry.target_name : "none";
      json[ "spell_name" ] = entry.action->data_reporting().name_cstr();
      json[ "queue_failed" ] = entry.queue_failed;
      if ( entry.action->item )
      {
        json[ "item_name" ] = entry.action->item->name_str;
      }
    }
    else
    {
      json[ "wait" ] = entry.wait_time;
    }

    if ( !entry.buff_list.empty() )
    {
      auto buffs = json[ "buffs" ];
      buffs.make_array();
      range::for_each( entry.buff_list, [ &buffs, &report_configuration ]( const auto& data ) {
        auto entry = buffs.add();

        entry[ "id" ] = data.object->data_reporting().id();
        entry[ "name" ] = data.object->name();
        entry[ "stacks" ] = data.value;
        if ( report_configuration.full_states )
        {
          entry[ "remains" ] = data.time_value;
        }
      } );
    }

    // Writing cooldown and debuffs data if asking for json full states
    if ( report_configuration.full_states && !entry.cooldown_list.empty() )
    {
      auto cooldowns = json[ "cooldowns" ];
      cooldowns.make_array();
      range::for_each( entry.cooldown_list, [ &cooldowns ]( const auto& data ) {
        auto entry = cooldowns.add();

        entry[ "name" ] = data.object->name();
        entry[ "stacks" ] = data.value;
        entry[ "remains" ] = data.time_value;
      } );
    }

    if ( report_configuration.full_states && !entry.target_list.empty() )
    {
      auto targets = json[ "targets" ];
      targets.make_array();
      range::for_each( entry.target_list, [ &targets ]( const auto& target_data ) {
        auto target_entry = targets.add();
        target_entry[ "name" ] = target_data.first->name();
        auto debuffs = target_entry[ "debuffs" ];
        debuffs.make_array();
        range::for_each( target_data.second, [ &debuffs ]( const auto& data ) {
          auto entry = debuffs.add();
          entry[ "name" ] = data.object->name();
          entry[ "stack" ] = data.value;
          entry[ "remains" ] = data.time_value;
        } );
      } );
    }

    auto resources = json[ "resources" ];
    auto resources_max = json[ "resources_max" ];
    range::for_each( relevant_resources, [ &resources, &resources_max, &entry ]( resource_e r ) {
      resources[ util::resource_type_string( r ) ] = util::round( entry.resource_snapshot[ r ], 2 );
      // TODO: Why do we have this instead of using some static one?
      resources_max[ util::resource_type_string( r ) ] = util::round( entry.resource_max_snapshot[ r ], 2 );
    } );
  } );
}

void collected_data_to_json( JsonOutput root, const ::report::json::report_configuration_t& report_configuration,
                             const player_t& p )
{
  const auto& sim = *p.sim;
  const auto& cd = p.collected_data;

  add_non_zero( root, "fight_length", cd.fight_length );
  add_non_zero( root, "waiting_time", cd.waiting_time );
  add_non_zero( root, "executed_foreground_actions", cd.executed_foreground_actions );
  add_non_zero( root, "dmg", cd.dmg );
  add_non_zero( root, "compound_dmg", cd.compound_dmg );
  add_non_zero( root, "timeline_dmg", cd.timeline_dmg );
  add_non_zero( root, "total_iterations", cd.total_iterations );
  if ( !p.is_enemy() && sim.enemy_targets > 1 )
  {
    add_non_zero( root, "prioritydps", cd.prioritydps );
  }
  add_non_zero( root, "dps", cd.dps );
  add_non_zero( root, "dpse", cd.dpse );

  if ( p.role == ROLE_TANK || p.role == ROLE_HEAL )
  {
    if ( p.role == ROLE_TANK )
    {
      root[ "dtps" ] = cd.dmg_taken;
      root[ "timeline_dmg_taken" ] = cd.timeline_dmg_taken;
      root[ "deaths" ] = cd.deaths;
    }

    root[ "heal" ] = cd.heal;
    root[ "compound_heal" ] = cd.compound_heal;
    root[ "hps" ] = cd.hps;
    root[ "hpse" ] = cd.hpse;
    root[ "absorb" ] = cd.absorb;
    root[ "compound_absorb" ] = cd.compound_absorb;
    root[ "aps" ] = cd.aps;

    if ( p.role == ROLE_TANK )
    {
      root[ "htps" ] = cd.htps;
      root[ "heal_taken" ] = cd.heal_taken;
      root[ "timeline_healing_taken" ] = cd.timeline_healing_taken;
      root[ "atps" ] = cd.atps;
      root[ "absorb_taken" ] = cd.absorb_taken;
    }
  }

  // No point printing out target metric if target_error is not used
  if ( cd.target_metric.count() > 0 )
  {
    root[ "target_metric" ] = cd.target_metric;
  }

  // Key off of resource loss to figure out what resources are even relevant
  std::vector<resource_e> relevant_resources;
  for ( size_t i = 0, end = cd.resource_lost.size(); i < end; ++i )
  {
    auto r = static_cast<resource_e>( i );

    if ( cd.resource_lost[ r ].mean() > 0 )
    {
      relevant_resources.push_back( r );
    }
  }

  // always include buffed_stats in JSON
  to_json( root[ "buffed_stats" ], p, cd.buffed_stats_snapshot, relevant_resources );

  if ( sim.report_details != 0 )
  {
    // Rest of the resource summaries are printed only based on relevant resources
    range::for_each( relevant_resources, [ &root, &cd ]( resource_e r ) {
      root[ "resource_lost" ][ util::resource_type_string( r ) ] = cd.resource_lost[ r ];

      root[ "resource_overflowed" ][ util::resource_type_string( r ) ] = cd.resource_overflowed[ r ];

      if ( r < cd.combat_end_resource.size() )
      {
        // Combat ending resources
        add_non_zero( root[ "combat_end_resource" ], util::resource_type_string( r ), cd.combat_end_resource[ r ] );
      }

      // Resource timeline for a given resource with loss
      auto it =
          range::find_if( cd.resource_timelines, [ r ]( const player_collected_data_t::resource_timeline_t& rtl ) {
            return rtl.type == r;
          } );

      if ( it != cd.resource_timelines.end() )
      {
        root[ "resource_timelines" ][ util::resource_type_string( r ) ] = it->timeline;
        if ( r == RESOURCE_HEALTH )
          root[ "resource_timelines" ][ "health_pct" ] = cd.health_pct;
      }
    } );

    // Stat timelines, if they exist
    range::for_each( cd.stat_timelines, [ &root ]( const player_collected_data_t::stat_timeline_t& stl ) {
      add_non_zero( root[ "stat_timelines" ], util::stat_type_string( stl.type ), stl.timeline );
    } );

    if ( cd.health_changes.collect )
    {
      root[ "health_changes" ] = cd.health_changes.merged_timeline;
    }

    if ( !cd.action_sequence_precombat.empty() )
    {
      to_json( root[ "action_sequence_precombat" ], report_configuration, cd.action_sequence_precombat,
               relevant_resources );
    }

    if ( !cd.action_sequence.empty() )
    {
      to_json( root[ "action_sequence" ], report_configuration, cd.action_sequence, relevant_resources );
    }
  }
}

void to_json( JsonOutput root, const dbc_t& dbc )
{
  bool versions[] = { false, true };
  for ( const auto& ptr : versions )
  {
    root[ dbc::wow_ptr_status( ptr ) ][ "build_level" ] = dbc::client_data_build( ptr );
    root[ dbc::wow_ptr_status( ptr ) ][ "wow_version" ] = dbc::client_data_version_str( ptr );
    root[ dbc::wow_ptr_status( ptr ) ][ "hotfix_date" ] = dbc::hotfix_date_str( ptr );
    root[ dbc::wow_ptr_status( ptr ) ][ "hotfix_build" ] = dbc::hotfix_build_version( ptr );
    root[ dbc::wow_ptr_status( ptr ) ][ "hotfix_hash" ] = dbc::hotfix_hash_str( ptr );
  }

  root[ "version_used" ] = StringRef( dbc::wow_ptr_status( dbc.ptr ) );
}

void scale_factors_to_json( JsonOutput root, const player_t& p )
{
  if ( p.sim->report_precision < 0 )
    p.sim->report_precision = 2;

  auto sm = p.sim->scaling->scaling_metric;
  const auto& sf =
      ( p.sim->scaling->normalize_scale_factors ) ? p.scaling->scaling_normalized[ sm ] : p.scaling->scaling[ sm ];

  for ( stat_e i = STAT_NONE; i < STAT_MAX; i++ )
  {
    if ( p.scaling->scales_with[ i ] )
    {
      root[ util::stat_type_abbrev( i ) ] = sf.get_stat( i );
    }
  }
}

void scale_factors_all_to_json( JsonOutput root, const player_t& p )
{
  if ( p.sim->report_precision < 0 )
    p.sim->report_precision = 2;

  for ( scale_metric_e sm = SCALE_METRIC_DPS; sm < SCALE_METRIC_MAX; sm++ )
  {
    auto node = root[ util::scale_metric_type_abbrev( sm ) ];

    const auto& sf =
        ( p.sim->scaling->normalize_scale_factors ) ? p.scaling->scaling_normalized[ sm ] : p.scaling->scaling[ sm ];

    for ( stat_e i = STAT_NONE; i < STAT_MAX; i++ )
    {
      if ( p.scaling->scales_with[ i ] )
      {
        node[ util::stat_type_abbrev( i ) ] = sf.get_stat( i );
      }
    }
  }
}

void scale_deltas_to_json( JsonOutput root, const player_t& p )
{
  for ( stat_e i = STAT_NONE; i < STAT_MAX; i++ )
  {
    if ( p.scaling->scales_with[ i ] )
    {
      root[ util::stat_type_abbrev( i ) ] = p.sim->scaling->stats->get_stat( i );
    }
  }
}

void to_json( JsonOutput& arr, const ::report::json::report_configuration_t& report_configuration, const player_t& p )
{
  auto root = arr.add();  // Add a fresh object to the players array and use it as root

  root[ "name" ] = p.name();
  root[ "race" ] = util::race_type_string( p.race );
  root[ "level" ] = p.true_level;
  root[ "role" ] = util::role_type_string( p.role );
  root[ "specialization" ] = util::specialization_string( p.specialization() );
  root[ "profile_source" ] = util::profile_source_string( p.profile_source_ );
  root[ "talents" ] = p.talents_str;
  root[ "party" ] = p.party;
  root[ "ready_type" ] = p.ready_type;
  root[ "bugs" ] = p.bugs;
  root[ "valid_fight_style" ] = p.validate_fight_style( p.sim->fight_style );
  root[ "scale_player" ] = p.scale_player;
  root[ "potion_used" ] = p.potion_used;
  root[ "timeofday" ] = p.timeofday == player_t::NIGHT_TIME ? "NIGHT_TIME" : "DAY_TIME";
  root[ "zandalari_loa" ] = p.zandalari_loa == player_t::AKUNDA      ? "akunda"
                            : p.zandalari_loa == player_t::BWONSAMDI ? "bwonsamdi"
                            : p.zandalari_loa == player_t::GONK      ? "gonk"
                            : p.zandalari_loa == player_t::KIMBUL    ? "kimbul"
                            : p.zandalari_loa == player_t::KRAGWA    ? "kragwa"
                                                                     : "paku";
  root[ "vulpera_tricks" ] = p.vulpera_tricks == player_t::HOLY      ? "holy"
                             : p.vulpera_tricks == player_t::FLAMES  ? "flames"
                             : p.vulpera_tricks == player_t::SHADOWS ? "shadows"
                             : p.vulpera_tricks == player_t::HEALING ? "healing"
                                                                     : "corrosive";
  root[ "earthen_mineral" ] = p.earthen_mineral == player_t::AMBER      ? "amber"
                              : p.earthen_mineral == player_t::EMERALD  ? "emerald"
                              : p.earthen_mineral == player_t::ONYX     ? "onyx"
                              : p.earthen_mineral == player_t::SAPPHIRE ? "sapphire"
                                                                        : "ruby";

  if ( p.is_enemy() )
  {
    root[ "death_pct" ] = p.death_pct;
    root[ "height" ] = p.height;
    root[ "combat_reach" ] = p.combat_reach;
  }

  if ( p.sim->report_pets_separately )
  {
    // TODO: Pet reporting
  }

  root[ "invert_scaling" ] = p.invert_scaling;
  root[ "reaction_offset" ] = p.reaction_offset;
  root[ "reaction_max" ] = p.reaction_max;
  root[ "reaction_mean" ] = p.reaction.mean;
  root[ "reaction_stddev" ] = p.reaction.stddev;
  root[ "reaction_nu" ] = p.reaction_nu;
  root[ "world_lag" ] = p.world_lag.mean;
  root[ "world_lag_stddev" ] = p.world_lag.stddev;
  root[ "brain_lag" ] = p.brain_lag.mean;
  root[ "brain_lag_stddev" ] = p.brain_lag.stddev;
  
  to_json( root[ "dbc" ], *p.dbc );

  for ( auto i = PROFESSION_NONE; i < PROFESSION_MAX; ++i )
  {
    if ( p.profession[ i ] > 0 )
    {
      root[ "professions" ][ util::profession_type_string( i ) ] = p.profession[ i ];
    }
  }

  for ( resource_e r = RESOURCE_NONE; r < RESOURCE_MAX; ++r )
  {
    std::string name = "base_";
    name += util::resource_type_string( r );
    name += "_regen_per_second";
    add_non_zero( root, name, p.resources.base_regen_per_second[ r ] );
  }

  root[ "potion" ] = p.potion_str.empty() ? p.default_potion() : p.potion_str;
  root[ "flask" ] = p.flask_str.empty() ? p.default_flask() : p.flask_str;
  root[ "food" ] = p.food_str.empty() ? p.default_food() : p.food_str;
  root[ "augmentation" ] = p.rune_str.empty() ? p.default_rune() : p.rune_str;
  root[ "temporary_enchant" ] =
      p.temporary_enchant_str.empty() ? p.default_temporary_enchant() : p.temporary_enchant_str;

  /* TODO: Not implemented reporting begins here

  to_json( root[ "base_stats" ], p.base );
  to_json( root[ "initial_stats" ], p.initial );
  to_json( root[ "current_stats" ], p.current );

  if ( p.main_hand_weapon -> slot != SLOT_NONE )
  {
    to_json( root[ "main_hand_weapon" ], p.main_hand_weapon );
  }

  if ( p.off_hand_weapon -> slot != SLOT_NONE )
  {
    to_json( root[ "off_hand_weapon" ], p.off_hand_weapon );
  }

  to_json( root[ "resources" ], p.resources );
  to_json( root[ "consumables" ], p.consumables );
  */

  if ( p.sim->scaling->has_scale_factors() )
  {
    scale_factors_to_json( root[ "scale_factors" ], p );
    scale_factors_all_to_json( root[ "scale_factors_all" ], p );
    scale_deltas_to_json( root[ "scale_deltas" ], p );
  }

  collected_data_to_json( root[ "collected_data" ], report_configuration, p );

  if ( p.sim->report_details != 0 )
  {
    buffs_to_json( root[ "buffs" ], p );
    constant_buffs_to_json( root[ "buffs_constant" ], p );

    if ( !p.proc_list.empty() )
    {
      procs_to_json( root[ "procs" ], p );
    }

    if ( !p.gain_list.empty() )
    {
      gains_to_json( root[ "gains" ], p );
    }

    stats_to_json( root[ "stats" ], p.stats_list );

    // add pet stats as a separate property
    JsonOutput stats_pets = root[ "stats_pets" ];
    for ( const auto& pet : p.pet_list )
    {
      if ( has_valid_stats( pet->stats_list ) )
      {
        stats_to_json( stats_pets[ pet->name_str ], pet->stats_list );
      }
    }
  }

  gear_to_json( root[ "gear" ], p );

  JsonOutput custom = root[ "custom" ];
  p.output_json_report( custom );
}

void to_json( JsonOutput& arr, const raid_event_t& event )
{
  auto root = arr.add();
  root[ "name" ] = event.name;
  root[ "type" ] = event.type;
  add_non_zero( root, "first", event.first );
  add_non_zero( root, "last", event.last );
  add_non_zero( root, "cooldown", event.cooldown.mean );
  add_non_zero( root, "cooldown_stddev", event.cooldown.stddev );
  add_non_zero( root, "cooldown_min", event.cooldown.min );
  add_non_zero( root, "cooldown_max", event.cooldown.max );
  add_non_zero( root, "duration", event.duration.mean );
  add_non_zero( root, "duration_stddev", event.duration.stddev );
  add_non_zero( root, "duration_min", event.duration.min );
  add_non_zero( root, "duration_max", event.duration.max );
  add_non_zero( root, "distance_min", event.distance_min );
  add_non_zero( root, "distance_max", event.distance_max );
  add_non_zero( root, "players_only", event.players_only );
  add_non_default( root, "player_chance", event.player_chance, 1.0 );
  add_non_default( root, "affected_role", event.affected_role, ROLE_NONE );
  add_non_zero( root, "saved_duration", event.saved_duration );
}

void iteration_data_to_json( JsonOutput root, const std::vector<iteration_data_entry_t>& entries )
{
  root.make_array();

  range::for_each( entries, [ &root ]( const iteration_data_entry_t& entry ) {
    auto json_entry = root.add();

    json_entry[ "metric" ] = entry.metric;
    json_entry[ "seed" ] = entry.seed;
    json_entry[ "target_health" ] = entry.target_health;
  } );
}

#ifndef SC_NO_THREADING

void profileset_fetch_output_data( const profileset::profile_output_data_t& output_data, js::JsonOutput& ovr )
{
  if ( output_data.race() != RACE_NONE )
  {
    ovr[ "race" ] = util::race_type_string( output_data.race() );
  }
  if ( !output_data.talents().empty() )
  {
    const auto& talents = output_data.talents();
    auto ovr_talents = ovr[ "talents" ].make_array();
    for ( auto talent : talents )
    {
      auto ovr_talent = ovr_talents.add();
      ovr_talent[ "tier" ] = talent->row();
      ovr_talent[ "id" ] = talent->id();
      ovr_talent[ "spell_id" ] = talent->spell_id();
      ovr_talent[ "name" ] = talent->name_cstr();
    }
  }
  if ( !output_data.gear().empty() )
  {
    const auto& gear = output_data.gear();
    auto ovr_gear = ovr[ "gear" ];
    for ( const auto& item : gear )
    {
      auto ovr_slot = ovr_gear[ item.slot_name() ];
      ovr_slot[ "item_id" ] = item.item_id();
      ovr_slot[ "item_level" ] = item.item_level();
    }
  }
  if ( output_data.agility() )
  {
    ovr[ "stats" ][ "stamina" ] = output_data.stamina();
    ovr[ "stats" ][ "agility" ] = output_data.agility();
    ovr[ "stats" ][ "intellect" ] = output_data.strength();
    ovr[ "stats" ][ "strength" ] = output_data.intellect();

    ovr[ "stats" ][ "crit_rating" ] = output_data.crit_rating();
    ovr[ "stats" ][ "crit_pct" ] = output_data.crit_pct();
    ovr[ "stats" ][ "haste_rating" ] = output_data.haste_rating();
    ovr[ "stats" ][ "haste_pct" ] = output_data.haste_pct();
    ovr[ "stats" ][ "mastery_rating" ] = output_data.mastery_rating();
    ovr[ "stats" ][ "mastery_pct" ] = output_data.mastery_pct();
    ovr[ "stats" ][ "versatility_rating" ] = output_data.versatility_rating();
    ovr[ "stats" ][ "versatility_pct" ] = output_data.versatility_pct();

    ovr[ "stats" ][ "avoidance_rating" ] = output_data.avoidance_rating();
    ovr[ "stats" ][ "avoidance_pct" ] = output_data.avoidance_pct();
    ovr[ "stats" ][ "leech_rating" ] = output_data.leech_rating();
    ovr[ "stats" ][ "leech_pct" ] = output_data.leech_pct();
    ovr[ "stats" ][ "speed_rating" ] = output_data.speed_rating();
    ovr[ "stats" ][ "speed_pct" ] = output_data.speed_pct();

    ovr[ "stats" ][ "corruption" ] = output_data.corruption();
    ovr[ "stats" ][ "corruption_resistance" ] = output_data.corruption_resistance();
  }
}

void profileset_json2( const profileset::profilesets_t& profileset, const sim_t& sim, js::JsonOutput& root )
{
  root[ "metric" ] = util::scale_metric_type_string( sim.profileset_metric.front() );

  auto results = root[ "results" ].make_array();

  range::for_each( profileset.profilesets(),
                   [ &results, &sim ]( const profileset::profilesets_t::profileset_entry_t& profileset ) {
                     const auto& result = profileset->result();

                     if ( result.mean() == 0 )
                     {
                       return;
                     }

                     auto&& obj = results.add();

                     obj[ "name" ] = profileset->name();
                     obj[ "mean" ] = result.mean();
                     obj[ "min" ] = result.min();
                     obj[ "max" ] = result.max();
                     obj[ "stddev" ] = result.stddev();
                     obj[ "mean_stddev" ] = result.mean_stddev();
                     obj[ "mean_error" ] = result.mean_stddev() * sim.confidence_estimator;

                     if ( result.median() != 0 )
                     {
                       obj[ "median" ] = result.median();
                       obj[ "first_quartile" ] = result.first_quartile();
                       obj[ "third_quartile" ] = result.third_quartile();
                     }

                     obj[ "iterations" ] = as<uint64_t>( result.iterations() );

                     if ( profileset->results() > 1 )
                     {
                       auto results2 = obj[ "additional_metrics" ].make_array();
                       for ( size_t midx = 1; midx < sim.profileset_metric.size(); ++midx )
                       {
                         auto obj2 = results2.add();
                         const auto& result = profileset->result( sim.profileset_metric[ midx ] );

                         obj2[ "metric" ] = util::scale_metric_type_string( sim.profileset_metric[ midx ] );
                         obj2[ "mean" ] = result.mean();
                         obj2[ "min" ] = result.min();
                         obj2[ "max" ] = result.max();
                         obj2[ "stddev" ] = result.stddev();
                         obj2[ "mean_stddev" ] = result.mean_stddev();
                         obj2[ "mean_error" ] = result.mean_stddev() * sim.confidence_estimator;

                         if ( result.median() != 0 )
                         {
                           obj2[ "median" ] = result.median();
                           obj2[ "first_quartile" ] = result.first_quartile();
                           obj2[ "third_quartile" ] = result.third_quartile();
                         }
                       }
                     }

                     // Optional override ouput data
                     if ( !sim.profileset_output_data.empty() )
                     {
                       const auto& output_data = profileset->output_data();
                       // TODO: Create the overrides object only if there is at least one override registered
                       auto ovr = obj[ "overrides" ];
                       profileset_fetch_output_data( output_data, ovr );
                     }
                   } );
}

void profileset_json3( const profileset::profilesets_t& profilesets, const sim_t& sim, js::JsonOutput& root )
{
  auto results = root[ "results" ].make_array();

  range::for_each( profilesets.profilesets(),
                   [ &results, &sim ]( const profileset::profilesets_t::profileset_entry_t& profileset ) {
                     auto&& obj = results.add();
                     obj[ "name" ] = profileset->name();
                     auto results_obj = obj[ "metrics" ].make_array();

                     for ( size_t midx = 0; midx < sim.profileset_metric.size(); ++midx )
                     {
                       const auto& result = profileset->result( sim.profileset_metric[ midx ] );

                       auto&& obj = results_obj.add();

                       obj[ "metric" ] = util::scale_metric_type_string( sim.profileset_metric[ midx ] );
                       obj[ "mean" ] = result.mean();
                       obj[ "min" ] = result.min();
                       obj[ "max" ] = result.max();
                       obj[ "stddev" ] = result.stddev();
                       obj[ "mean_stddev" ] = result.mean_stddev();
                       obj[ "mean_error" ] = result.mean_stddev() * sim.confidence_estimator;

                       if ( result.median() != 0 )
                       {
                         obj[ "median" ] = result.median();
                         obj[ "first_quartile" ] = result.first_quartile();
                         obj[ "third_quartile" ] = result.third_quartile();
                       }

                       obj[ "iterations" ] = as<uint64_t>( result.iterations() );
                     }

                     // Optional override ouput data
                     if ( !sim.profileset_output_data.empty() )
                     {
                       const auto& output_data = profileset->output_data();
                       // TODO: Create the overrides object only if there is at least one override registered
                       auto ovr = obj[ "overrides" ];
                       profileset_fetch_output_data( output_data, ovr );
                     }
                   } );
}
#endif

void profileset_json( const ::report::json::report_configuration_t& report_configuration,
                      const profileset::profilesets_t& profileset, const sim_t& sim, js::JsonOutput& root )
{
#ifndef SC_NO_THREADING
  if ( report_configuration.version_intersects( ">=3.0.0" ) )
  {
    profileset_json3( profileset, sim, root );
  }
  else
  {
    profileset_json2( profileset, sim, root );
  }
#endif
}

void dps_plot_json( const ::report::json::report_configuration_t& /* report_configuration */, const plot_t& dps_plot,
                    const sim_t& sim, js::JsonOutput& root )
{
  for ( auto player : sim.player_list )
  {
    if ( player->quiet )
      continue;

    auto&& obj = root.add();
    obj[ "name" ] = player->name();
    auto data_obj = obj[ "data" ].make_array();

    for ( stat_e j = STAT_NONE; j < STAT_MAX; j++ )
    {
      if ( !dps_plot.is_plot_stat( j ) )
        continue;

      auto&& dobj = data_obj.add();
      auto stat_obj = dobj[ util::stat_type_abbrev( j ) ].make_array();

      for ( const auto& data : player->dps_plot_data[ j ] )
      {
        auto&& sobj = stat_obj.add();
        sobj[ "rating" ] = data.plot_step;
        sobj[ "dps" ] = data.value;
        sobj[ "dps-error" ] = data.error;
      }
    }
  }
}

void reforge_plot_json( const ::report::json::report_configuration_t& /* report_configuration */,
                        const reforge_plot_t& reforge_plot, const sim_t& sim, js::JsonOutput& root )
{
  const auto& stat_list = reforge_plot.reforge_plot_stat_indices;

  for ( auto player : sim.player_list )
  {
    if ( player->quiet )
      continue;

    auto&& obj = root.add();
    obj[ "name" ] = player->name();
    auto data_obj = obj[ "data" ].make_array();

    for ( size_t i = 0; i < player->reforge_plot_data.size(); i++ )
    {
      auto&& dobj = data_obj.add();
      size_t j = 0;

      while ( j < stat_list.size() )
      {
        dobj[ util::stat_type_abbrev( stat_list[ j ] ) ] = player->reforge_plot_data[ i ][ j ].value;
        j++;
      }

      dobj[ "dps" ] = player->reforge_plot_data[ i ][ j ].value;
      dobj[ "dps-error" ] = player->reforge_plot_data[ i ][ j ].error;
    }
  }
}

void to_json( const ::report::json::report_configuration_t& report_configuration, JsonOutput root, const sim_t& sim )
{
  // Sim-scope options
  auto options_root = root[ "options" ];

  options_root[ "debug" ] = sim.debug;
  options_root[ "max_time" ] = sim.max_time.total_seconds();
  options_root[ "expected_iteration_time" ] = sim.expected_iteration_time.total_seconds();
  options_root[ "vary_combat_length" ] = sim.vary_combat_length;
  options_root[ "iterations" ] = sim.iterations;
  options_root[ "target_error" ] = sim.target_error;
  options_root[ "threads" ] = sim.threads;
  options_root[ "seed" ] = sim.seed;
  options_root[ "single_actor_batch" ] = sim.single_actor_batch;
  options_root[ "queue_lag" ] = sim.queue_lag.mean;
  options_root[ "queue_lag_stddev" ] = sim.queue_lag.stddev;
  options_root[ "gcd_lag" ] = sim.gcd_lag.mean;
  options_root[ "gcd_lag_stddev" ] = sim.gcd_lag.stddev;
  options_root[ "channel_lag" ] = sim.channel_lag.mean;
  options_root[ "channel_lag_stddev" ] = sim.channel_lag.stddev;
  options_root[ "queue_gcd_reduction" ] = sim.queue_gcd_reduction;
  options_root[ "strict_gcd_queue" ] = sim.strict_gcd_queue;
  options_root[ "confidence" ] = sim.confidence;
  options_root[ "confidence_estimator" ] = sim.confidence_estimator;
  options_root[ "world_lag" ] = sim.world_lag.mean;
  options_root[ "world_lag_stddev" ] = sim.world_lag.stddev;
  options_root[ "travel_variance" ] = sim.travel_variance;
  options_root[ "default_skill" ] = sim.default_skill;
  options_root[ "reaction_time" ] = sim.reaction_time;
  options_root[ "regen_periodicity" ] = sim.regen_periodicity;
  options_root[ "ignite_sampling_delta" ] = sim.ignite_sampling_delta;
  options_root[ "fixed_time" ] = sim.fixed_time;
  options_root[ "optimize_expressions" ] = sim.optimize_expressions;
  options_root[ "optimal_raid" ] = sim.optimal_raid;
  options_root[ "log" ] = sim.log;
  options_root[ "debug_each" ] = sim.debug_each;
  options_root[ "stat_cache" ] = sim.stat_cache;
  options_root[ "max_aoe_enemies" ] = sim.max_aoe_enemies;
  options_root[ "enemy_death_pct" ] = sim.enemy_death_pct;
  options_root[ "challenge_mode" ] = sim.challenge_mode;
  options_root[ "timewalk" ] = sim.timewalk;
  options_root[ "pvp_mode" ] = sim.pvp_mode;
  options_root[ "rng" ] = sim.rng();
  options_root[ "deterministic" ] = sim.deterministic;
  options_root[ "average_range" ] = sim.average_range;
  options_root[ "average_gauss" ] = sim.average_gauss;
  options_root[ "fight_style" ] = util::fight_style_string( sim.fight_style );
  options_root[ "desired_targets" ] = sim.desired_targets;
  options_root[ "default_aura_delay" ] = sim.default_aura_delay.mean;
  options_root[ "default_aura_delay_stddev" ] = sim.default_aura_delay.stddev;
  options_root[ "profileset_metric" ] = util::scale_metric_type_abbrev( sim.profileset_metric.front() );
  options_root[ "profileset_multiactor_base_name" ] = sim.profileset_multiactor_base_name;

  to_json( options_root[ "dbc" ], *sim.dbc );

  if ( sim.scaling->calculate_scale_factors )
  {
    auto scaling_root = options_root[ "scaling" ];
    scaling_root[ "calculate_scale_factors" ] = sim.scaling->calculate_scale_factors;
    scaling_root[ "normalize_scale_factors" ] = sim.scaling->normalize_scale_factors;
    add_non_zero( scaling_root, "scale_only", sim.scaling->scale_only_str );
    add_non_zero( scaling_root, "scale_over", sim.scaling->scale_over );
    add_non_zero( scaling_root, "scale_over_player", sim.scaling->scale_over_player );
    add_non_default( scaling_root, "scale_delta_multiplier", sim.scaling->scale_delta_multiplier, 1.0 );
    add_non_zero( scaling_root, "positive_scale_delta", sim.scaling->positive_scale_delta );
    add_non_zero( scaling_root, "scale_lag", sim.scaling->scale_lag );
    add_non_zero( scaling_root, "center_scale_delta", sim.scaling->center_scale_delta );
  }

  // Overrides
  auto overrides = root[ "overrides" ];
  add_non_zero( overrides, "arcane_intellect", sim.overrides.arcane_intellect );
  add_non_zero( overrides, "battle_shout", sim.overrides.battle_shout );
  add_non_zero( overrides, "power_word_fortitude", sim.overrides.power_word_fortitude );
  add_non_zero( overrides, "chaos_brand", sim.overrides.chaos_brand );
  add_non_zero( overrides, "mystic_touch", sim.overrides.mystic_touch );
  add_non_zero( overrides, "mortal_wounds", sim.overrides.mortal_wounds );
  add_non_zero( overrides, "bleeding", sim.overrides.bleeding );
  add_non_zero( overrides, "bloodlust", sim.overrides.bloodlust );
  if ( sim.overrides.bloodlust )
  {
    add_non_zero( overrides, "bloodlust_percent", sim.bloodlust_percent );
    add_non_zero( overrides, "bloodlust_time", sim.bloodlust_time );
  }

  if ( !sim.overrides.target_health.empty() )
  {
    overrides[ "target_health" ] = sim.overrides.target_health;
  }

  // Players
  JsonOutput players_arr = root[ "players" ].make_array();

  range::for_each( sim.player_no_pet_list.data(), [ & ]( const player_t* p ) {
    to_json( players_arr, report_configuration, *p );
  } );

  if ( sim.profilesets->n_profilesets() > 0 )
  {
    auto profileset_root = root[ "profilesets" ];
    profileset_json( report_configuration, *sim.profilesets, sim, profileset_root );
  }

  if ( !sim.plot->dps_plot_stat_str.empty() )
  {
    auto dps_plot_root = root[ "dps_plot" ].make_array();
    dps_plot_json( report_configuration, *sim.plot, sim, dps_plot_root );
  }

  if ( !sim.reforge_plot->reforge_plot_stat_str.empty() )
  {
    auto reforge_plot_root = root[ "reforge_plot" ].make_array();
    reforge_plot_json( report_configuration, *sim.reforge_plot, sim, reforge_plot_root );
  }

  auto stats_root = root[ "statistics" ];
  stats_root[ "elapsed_cpu_seconds" ] = chrono::to_fp_seconds( sim.elapsed_cpu );
  stats_root[ "elapsed_time_seconds" ] = chrono::to_fp_seconds( sim.elapsed_time );
  stats_root[ "init_time_seconds" ] = chrono::to_fp_seconds( sim.init_time );
  stats_root[ "merge_time_seconds" ] = chrono::to_fp_seconds( sim.merge_time );
  stats_root[ "analyze_time_seconds" ] = chrono::to_fp_seconds( sim.analyze_time );
  stats_root[ "simulation_length" ] = sim.simulation_length;
  stats_root[ "total_events_processed" ] = sim.event_mgr.total_events_processed;
  add_non_zero( stats_root, "raid_dps", sim.raid_dps );
  add_non_zero( stats_root, "raid_hps", sim.raid_hps );
  add_non_zero( stats_root, "raid_aps", sim.raid_aps );
  add_non_zero( stats_root, "total_dmg", sim.total_dmg );
  add_non_zero( stats_root, "total_heal", sim.total_heal );
  add_non_zero( stats_root, "total_absorb", sim.total_absorb );

  if ( sim.report_details != 0 )
  {
    // Targets
    JsonOutput targets_arr = root[ "targets" ].make_array();

    range::for_each( sim.target_list.data(), [ & ]( const player_t* p ) {
      to_json( targets_arr, report_configuration, *p );
    } );

    // Raid events
    if ( !sim.raid_events.empty() )
    {
      auto arr = root[ "raid_events" ].make_array();

      range::for_each( sim.raid_events, [ & ]( const std::unique_ptr<raid_event_t>& event ) {
        to_json( arr, *event );
      } );
    }

    if ( !sim.buff_list.empty() )
    {
      JsonOutput buffs_arr = root[ "sim_auras" ].make_array();
      range::for_each( sim.buff_list, [ & ]( const buff_t* b ) {
        if ( b->avg_start.mean() == 0 )
        {
          return;
        }
        to_json( buffs_arr.add(), b );
      } );
    }

    if ( !sim.low_iteration_data.empty() )
    {
      iteration_data_to_json( root[ "iteration_data" ][ "low" ], sim.low_iteration_data );
    }

    if ( !sim.high_iteration_data.empty() )
    {
      iteration_data_to_json( root[ "iteration_data" ][ "high" ], sim.high_iteration_data );
    }
  }
}

void normal_print( FileWriteStream& stream, Document& doc,
                   const ::report::json::report_configuration_t& report_configuration )
{
  Writer<FileWriteStream> writer( stream );
  if ( report_configuration.decimal_places > 0 )
  {
    writer.SetMaxDecimalPlaces( report_configuration.decimal_places );
  }
  auto accepted = doc.Accept( writer );
  if ( !accepted )
  {
    throw std::runtime_error( "JSON Writer did not accept document." );
  }
}

void pretty_print( FileWriteStream& stream, Document& doc,
                   const ::report::json::report_configuration_t& report_configuration )
{
  PrettyWriter<FileWriteStream> writer( stream );
  if ( report_configuration.decimal_places > 0 )
  {
    writer.SetMaxDecimalPlaces( report_configuration.decimal_places );
  }
  auto accepted = doc.Accept( writer );
  if ( !accepted )
  {
    throw std::runtime_error( "JSON Writer did not accept document." );
  }
}

void print_json_pretty( FILE* o, const sim_t& sim, const ::report::json::report_configuration_t& report_configuration )
{
  Document doc;
  Value& v = doc;
  v.SetObject();

  JsonOutput root( doc, v );

  if ( report_configuration.version_intersects( ">=3.0.0" ) )
  {
    root[ "$id" ] =
        fmt::format( "https://www.simulationcraft.org/reports/{}.schema.json", report_configuration.version() );
  }
  root[ "version" ] = SC_VERSION;
  root[ "report_version" ] = report_configuration.version();
  root[ "ptr_enabled" ] = SC_USE_PTR;
  root[ "beta_enabled" ] = SC_BETA;
  root[ "build_date" ] = __DATE__;
  root[ "build_time" ] = __TIME__;
  root[ "timestamp" ] = as<uint64_t>( std::time( nullptr ) );
  if constexpr ( SC_NO_NETWORKING_ON )
  {
    root[ "no_networking" ] = true;
  }

  if ( git_info::available() )
  {
    root[ "git_revision" ] = git_info::revision();
    root[ "git_branch" ] = git_info::branch();
  }

  to_json( report_configuration, root[ "sim" ], sim );

  if ( !sim.error_list.empty() )
  {
    root[ "notifications" ] = sim.error_list;
  }

  std::array<char, 16384> buffer;
  FileWriteStream b( o, buffer.data(), buffer.size() );
  if ( report_configuration.pretty_print )
  {
    pretty_print( b, doc, report_configuration );
  }
  else
  {
    normal_print( b, doc, report_configuration );
  }
}

void print_json_report( sim_t& sim, const ::report::json::report_configuration_t& report_configuration )
{
  if ( !report_configuration.destination().empty() )
  {
    // Setup file stream and open file
    io::cfile s( report_configuration.destination(), "w" );
    if ( !s )
    {
      sim.errorf( "Failed to open JSON output file '%s'.", report_configuration.destination().c_str() );
      return;
    }

    // Print JSON report
    try
    {
      if ( report_configuration.full_states )
      {
        fmt::print( "\nReport will be generated with full state for each action.\n" );
      }
      report_timer_t t( fmt::format( "JSON report version {}", report_configuration.version() ), stdout );
      if ( !sim.profileset_enabled )
      {
        t.start();
      }
      print_json_pretty( s, sim, report_configuration );
    }
    catch ( const std::exception& e )
    {
      sim.error( "Error generating JSON report: {}", e.what() );
    }
  }
}
}  // unnamed namespace

namespace report
{
void print_json( sim_t& sim )
{
  for ( const auto& report_configuration : sim.json_reports )
  {
    print_json_report( sim, report_configuration );
  }
}

}  // namespace report
