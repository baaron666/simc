// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================

#include "absorb.hpp"
#include "action_state.hpp"
#include "buff/buff.hpp"
#include "player/stats.hpp"
#include "player/player.hpp"
#include "sim/sim.hpp"

absorb_t::absorb_t( util::string_view name, player_t* p, const spell_data_t* s )
  : spell_base_t( ACTION_ABSORB, name, p, s ), target_specific( false )
{
  if (sim->heal_target && target == sim->target)
  {
    target = sim->heal_target;
  }
  else if (target->is_enemy())
  {
    target = p;
  }

  may_crit = false;

  stats->type = STATS_ABSORB;
}

absorb_buff_t* absorb_t::create_buff( const action_state_t* s )
{
  buff_t* b = buff_t::find( s->target, name_str, player );
  if ( b )
    return debug_cast<absorb_buff_t*>( b );

  auto buff = make_buff<absorb_buff_t>( actor_pair_t( s->target, player ), name_str, &data() );
  buff->set_absorb_source( stats );

  return buff;
}

void absorb_t::activate()
{
  sim->player_non_sleeping_list.register_callback( [ this ]( player_t* ) { target_cache.is_valid = false; } );
}

void absorb_t::impact(action_state_t* s)
{
  s->result_amount = calculate_crit_damage_bonus(s);
  assess_damage(type == ACTION_HEAL ? result_amount_type::HEAL_DIRECT : result_amount_type::DMG_DIRECT, s);
}

void absorb_t::assess_damage(result_amount_type  /*heal_type*/, action_state_t* s)
{
  if (target_specific[s->target] == nullptr)
  {
    target_specific[s->target] = create_buff(s);
  }

  if (result_is_hit(s->result))
  {
    target_specific[s->target]->trigger(1, s->result_amount);

    sim->print_log("{} {} applies absorb on {} for {} ({}) ({})",
      *player, *this, *s->target, s->result_amount, s->result_total,
      s->result);
  }

  stats->add_result(0.0, s->result_total, result_amount_type::ABSORB, s->result, s->block_result, s->target);
}

result_amount_type absorb_t::amount_type(const action_state_t*, bool) const
{
  return result_amount_type::ABSORB;
}

int absorb_t::num_targets() const
{
  return as<int>(range::count_if(sim->actor_list,
    [](player_t* t) {
      if (t->is_sleeping()) return false;
      if (t->is_enemy()) return false;
      return true;
    }));
}

double absorb_t::composite_da_multiplier(const action_state_t* s) const
{
  double m = action_multiplier() * action_da_multiplier() *
    player->composite_player_absorb_multiplier(s);

  return m;
}

double absorb_t::composite_ta_multiplier(const action_state_t* s) const
{
  double m = action_multiplier() * action_ta_multiplier() *
    player->composite_player_absorb_multiplier(s);

  return m;
}

double absorb_t::composite_versatility(const action_state_t* state) const
{
  return spell_base_t::composite_versatility(state) + player->cache.heal_versatility();
}

double absorb_t::composite_target_multiplier( player_t* target ) const
{
  double m = spell_base_t::composite_target_multiplier( target );
  
  m *= target->composite_player_absorb_received_multiplier();

  return m;
}

size_t absorb_t::available_targets( std::vector<player_t*>& target_list ) const
{
  target_list.clear();
  if ( !target->is_sleeping() )
    target_list.push_back( target );

  for ( const auto& t : sim->player_non_sleeping_list )
  {
    if ( t != target )
      target_list.push_back( t );
  }

  return target_list.size();
}
