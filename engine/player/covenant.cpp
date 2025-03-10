// ==========================================================================
// Dedmonwakeen's Raid DPS/TPS Simulator.
// Send questions to natehieter@gmail.com
// ==========================================================================
#include "covenant.hpp"

#include "action/dot.hpp"
#include "action/spell.hpp"
#include "buff/buff.hpp"
#include "dbc/covenant_data.hpp"
#include "dbc/dbc.hpp"
#include "dbc/spell_data.hpp"
#include "item/special_effect.hpp"
#include "player/actor_target_data.hpp"
#include "player/player.hpp"
#include "report/decorators.hpp"
#include "sim/expressions.hpp"
#include "sim/option.hpp"
#include "sim/sim.hpp"
#include "util/io.hpp"
#include "util/util.hpp"

conduit_data_t::conduit_data_t() :
  /* m_player( nullptr ), */ m_conduit( &conduit_rank_entry_t::nil() ), m_spell( spell_data_t::not_found() )
{ }

conduit_data_t::conduit_data_t( const player_t* player, const conduit_rank_entry_t& conduit )
  : /* m_player( player ), */ m_conduit( &conduit ), m_spell( dbc::find_spell( player, conduit.spell_id ) )
{ }

bool conduit_data_t::ok() const
{
  return m_conduit->conduit_id != 0;
}

unsigned conduit_data_t::rank() const
{
  // Ranks in the client data begin from 0, but better to deal with them as 1+
  return m_conduit->rank + 1;
}

double conduit_data_t::value() const
{
  return m_conduit->value;
}

double conduit_data_t::percent() const
{
  return m_conduit->value / 100.0;
}

timespan_t conduit_data_t::time_value( time_type tt ) const
{
  if ( tt == time_type::MS )
  {
    return timespan_t::from_millis( m_conduit->value );
  }
  else
  {
    return timespan_t::from_seconds( m_conduit->value );
  }
}

namespace util
{
const char* covenant_type_string( covenant_e id, bool full )
{
  switch ( id )
  {
    case covenant_e::KYRIAN:    return full ? "Kyrian" : "kyrian";
    case covenant_e::VENTHYR:   return full ? "Venthyr" : "venthyr";
    case covenant_e::NIGHT_FAE: return full ? "Night Fae" : "night_fae";
    case covenant_e::NECROLORD: return full ? "Necrolord" : "necrolord";
    case covenant_e::DISABLED:  return "disabled";
    default:                    return "invalid";
  }
}

covenant_e parse_covenant_string( util::string_view covenant_str )
{
  if ( util::str_compare_ci( covenant_str, "kyrian" ) )
  {
    return covenant_e::KYRIAN;
  }
  else if ( util::str_compare_ci( covenant_str, "venthyr" ) )
  {
    return covenant_e::VENTHYR;
  }
  else if ( util::str_compare_ci( covenant_str, "night_fae" ) )
  {
    return covenant_e::NIGHT_FAE;
  }
  else if ( util::str_compare_ci( covenant_str, "necrolord" ) )
  {
    return covenant_e::NECROLORD;
  }
  else if ( util::str_compare_ci( covenant_str, "none" ) ||
            util::str_compare_ci( covenant_str, "disabled" ) ||
            util::str_compare_ci( covenant_str, "0" ) )
  {
    return covenant_e::DISABLED;
  }

  return covenant_e::INVALID;
}
} // Namespace util ends

namespace covenant
{
std::unique_ptr<covenant_state_t> create_player_state( const player_t* player )
{
  return std::make_unique<covenant_state_t>( player );
}

covenant_state_t::covenant_state_t( const player_t* player )
  : m_covenant( covenant_e::INVALID ), m_player( player ), m_renown_level(), cast_callback( nullptr )
{
}

bool covenant_state_t::parse_covenant( sim_t*             sim,
                                       util::string_view /* name */,
                                       util::string_view value )
{
  if ( !sim->shadowlands_opts.enabled )
    return true;

  covenant_e covenant = util::parse_covenant_string( value );
  unsigned covenant_id = util::to_unsigned_ignore_error( value, static_cast<unsigned>( covenant_e::INVALID ) );

  if ( covenant == covenant_e::INVALID &&
       covenant_id > static_cast<unsigned>( covenant_e::COVENANT_ID_MAX ) )
  {
    sim->error( "{} unknown covenant '{}', must be one of "
                "kyrian, venthyr, night_fae, necrolord, or none, or a number between 0-4.",
        m_player->name(), value );
    return false;
  }

  if ( covenant != covenant_e::INVALID )
  {
    m_covenant = covenant;
  }
  else
  {
    m_covenant = static_cast<covenant_e>( covenant_id );
  }

  // Renown spells cannot be properly found without a Covenant. If there are
  // no spells found yet, try to find them here in case renown is already set.
  if ( m_renown.empty() )
    set_renown_level( m_renown_level );

  return true;
}

// Parse soulbind= option into conduit_state_t and soulbind spell ids
// Format:
// soulbind_token = conduit_id:conduit_rank[:empowered] | soulbind_ability_id
// soulbind = [soulbind_tree_id,]soulbind_token/soulbind_token/soulbind_token/...
// Where:
// soulbind_tree_id = numeric or tokenized soulbind tree identifier; unused by
//                    Simulationcraft, and ignored on parse.
// conduit_id = conduit id number or tokenized version of the conduit name
// conduit_rank = the rank number, starting from 1
// soulbind_ability_id = soulbind ability spell id or tokenized veresion of the soulbind
// ability name
bool covenant_state_t::parse_soulbind( sim_t*             sim,
                                       util::string_view /* name */,
                                       util::string_view value )
{
  auto value_str = value;

  // Ignore anything before a comma character in the soulbind option to allow the
  // specification of the soulbind tree (as a numeric parameter, or the name of the
  // follower in tokenized form). Currently simc has no use for this information, but
  // third party tools will find it easier to determine the soulbind tree to display/use
  // from the identifier, instead of reverse-mapping it from the soulbind spell
  // identifiers.
  auto comma_pos = value_str.find(',');
  if ( comma_pos != std::string::npos )
  {
    value_str = value_str.substr( comma_pos + 1 );
  }

  for ( const util::string_view entry : util::string_split<util::string_view>( value_str, "|/" ) )
  {
    // Conduit handling
    if ( entry.find( ':' ) != util::string_view::npos )
    {
      auto _conduit_split = util::string_split<util::string_view>( entry, ":" );
      if ( _conduit_split.size() != 2 && _conduit_split.size() != 3 )
      {
        sim->error( "{} unknown conduit format {}, must be conduit_id:conduit_rank[:empowered]",
          m_player->name(), entry );
        return false;
      }

      const conduit_entry_t* conduit_entry = nullptr;
      unsigned conduit_id = util::to_unsigned_ignore_error( _conduit_split[ 0 ], 0 );
      unsigned conduit_rank = util::to_unsigned( _conduit_split[ 1 ] );
      // convenience tracking for error message purposes
      // TODO: retain state in conduit_entry_t for html report differentiation of empowered vs non
      bool conduit_empowered = false;

      if ( conduit_rank == 0 )
      {
        sim->error( "{} invalid conduit rank '{}', must be 1+",
            m_player->name(), _conduit_split[ 1 ] );
        return false;
      }

      // Find by number (conduit ID)
      if ( conduit_id > 0 )
      {
        conduit_entry = &conduit_entry_t::find( conduit_id, m_player->dbc->ptr );
      }
      // Try to find by tokenized string
      else
      {
        conduit_entry = &conduit_entry_t::find( _conduit_split[ 0 ], m_player->dbc->ptr,
            true );
      }

      if ( conduit_entry->id == 0 )
      {
        sim->error( "{} unknown conduit id {}", m_player->name(), _conduit_split[ 0 ] );
        return false;
      }

      if ( _conduit_split.size() == 3 )
      {
        if ( _conduit_split[ 2 ] == "1" )
        {
          // empowered conduit simply get +2 to rank
          conduit_rank += 2;
          conduit_empowered = true;
        }
        else if ( _conduit_split[ 2 ] != "0" )
        {
          sim->error( "{} unknown conduit empowered flag {} for {} (id={}), must be '0' or '1'", m_player->name(),
                      _conduit_split[ 2 ], conduit_entry->name, conduit_entry->id );
          return false;
        }
      }

      const auto& conduit_rank_entry = conduit_rank_entry_t::find( conduit_entry->id,
          conduit_rank - 1U, m_player->dbc->ptr );
      if ( conduit_rank_entry.conduit_id == 0 )
      {
        sim->error( "{} unknown conduit rank {}{} for {} (id={})", m_player->name(), _conduit_split[ 1 ],
                    conduit_empowered ? " (empowered)" : "", conduit_entry->name, conduit_entry->id );
        return false;
      }

      // In game ranks are zero-indexed
      m_conduits.emplace_back( conduit_entry->id, conduit_rank - 1U );
    }
    // Soulbind handling
    else
    {
      const soulbind_ability_entry_t* soulbind_entry = nullptr;
      auto soulbind_spell_id = util::to_unsigned_ignore_error( entry, 0 );
      if ( soulbind_spell_id > 0 )
      {
        soulbind_entry = &soulbind_ability_entry_t::find( soulbind_spell_id,
            m_player->dbc->ptr );
      }
      else
      {
        soulbind_entry = &soulbind_ability_entry_t::find( entry, m_player->dbc->ptr, true );
      }

      if ( soulbind_entry->spell_id == 0 )
      {
        sim->error( "{} unknown soulbind spell id {}", m_player->name(), entry );
        return false;
      }

      m_soulbinds.push_back( soulbind_entry->spell_id );
    }
  }

  m_soulbind_str.push_back( std::string( value ) );

  return true;
}

bool covenant_state_t::parse_soulbind_clear( sim_t* sim, util::string_view name, util::string_view value )
{
  m_conduits.clear();
  m_soulbinds.clear();
  m_soulbind_str.clear();

  return parse_soulbind( sim, name, value );
}

bool covenant_state_t::parse_renown( sim_t*,
                                     util::string_view /* name */,
                                     util::string_view value )
{
  unsigned renown_level = util::to_unsigned( value );
  set_renown_level( renown_level );

  return true;
}

void covenant_state_t::set_renown_level( unsigned renown_level )
{
  m_renown_level = renown_level;
  m_renown.clear();
  std::unordered_map<std::string, unsigned> renown_levels;
  std::unordered_map<std::string, unsigned> renown_spells;

  for ( auto& entry : renown_reward_entry_t::find_by_covenant_id( id(), m_player->dbc->ptr ) )
  {
    if ( renown() < entry.renown_level )
      continue;

    auto it = renown_levels.find( entry.name );
    if ( it != renown_levels.end() && it->second > entry.renown_level )
      continue;

    renown_levels[ entry.name ] = entry.renown_level;
    renown_spells[ entry.name ] = entry.spell_id;
  }

  for ( const auto& spell : renown_spells )
    m_renown.push_back( spell.second );
}

bool covenant_state_t::is_conduit_socket_empowered( unsigned soulbind_id, unsigned tier, unsigned ui_order )
{
  for ( auto& entry : enhanced_conduit_entry_t::find_by_soulbind_id( soulbind_id, m_player->dbc->ptr ) )
  {
    if ( tier < entry.tier )
      break;

    if ( tier == entry.tier and ui_order == entry.ui_order )
      return renown() >= entry.renown_level;
  }

  return false;
}

const spell_data_t* covenant_state_t::get_covenant_ability( util::string_view name ) const
{
  if ( !enabled() )
  {
    return spell_data_t::not_found();
  }

  const auto& entry = covenant_ability_entry_t::find( name, m_player->dbc->ptr );
  if ( entry.spell_id == 0 )
  {
    return spell_data_t::nil();
  }

  if ( entry.covenant_id != id() )
  {
    return spell_data_t::not_found();
  }

  if ( entry.class_id && entry.class_id != as<unsigned>( util::class_id( m_player->type ) ) )
  {
    return spell_data_t::not_found();
  }

  const auto spell = dbc::find_spell( m_player, entry.spell_id );
  if ( as<int>( spell->level() ) > m_player->true_level )
  {
    return spell_data_t::not_found();
  }

  return spell;
}

const spell_data_t* covenant_state_t::get_soulbind_ability( util::string_view name,
                                                            bool              tokenized ) const
{
  if ( !enabled() )
  {
    return spell_data_t::not_found();
  }

  std::string search_str = tokenized ? util::tokenize_fn( name ) : std::string( name );

  const auto& soulbind = soulbind_ability_entry_t::find( search_str,
      m_player->dbc->ptr, tokenized );
  if ( soulbind.spell_id == 0 )
  {
    return spell_data_t::nil();
  }

  auto it = range::find( m_soulbinds, soulbind.spell_id );
  if ( it == m_soulbinds.end() )
  {
    return spell_data_t::not_found();
  }

  return dbc::find_spell( m_player, soulbind.spell_id );
}

conduit_data_t covenant_state_t::get_conduit_ability( util::string_view name,
                                                      bool              tokenized ) const
{
  if ( !enabled() )
  {
    return {};
  }

  const auto& conduit_entry = conduit_entry_t::find( name, m_player->dbc->ptr, tokenized );
  if ( conduit_entry.id == 0 )
  {
    return {};
  }

  auto it = range::find_if( m_conduits, [&conduit_entry]( const conduit_state_t& entry ) {
    return std::get<0>( entry ) == conduit_entry.id;
  } );

  if ( it == m_conduits.end() )
  {
    return {};
  }

  const auto& conduit_rank = conduit_rank_entry_t::find(
      std::get<0>( *it ), std::get<1>( *it ), m_player->dbc->ptr );
  if ( conduit_rank.conduit_id == 0 )
  {
    return {};
  }

  return { m_player, conduit_rank };
}

std::string covenant_state_t::soulbind_option_str() const
{
  if ( !m_soulbind_str.empty() )
  {
    std::string output;

    for ( auto it = m_soulbind_str.begin(); it != m_soulbind_str.end(); it++ )
    {
      auto str = *it;
      str.erase( 0, str.find_first_not_of( '/' ) );
      str.erase( str.find_last_not_of( '/' ) + 1 );

      if ( !str.empty() )
        output += fmt::format( "{}={}", it == m_soulbind_str.begin() ? "soulbind" : "\nsoulbind+", str );
    }

    return output;
  }

  if ( m_soulbinds.empty() && m_conduits.empty() )
  {
    return {};
  }

  std::vector<std::string> b;

  range::for_each( m_conduits, [&b]( const conduit_state_t& token ) {
    // Note, conduit ranks in client data are zero-indexed
    b.emplace_back( fmt::format( "{}:{}", std::get<0>( token ), std::get<1>( token ) + 1u ) );
  } );

  range::for_each( m_soulbinds, [&b]( unsigned spell_id ) {
    b.emplace_back( fmt::format( "{}", spell_id ) );
  } );

  return fmt::format( "soulbind={},{}", m_soulbind_id, util::string_join( b, "/" ) );
}

std::string covenant_state_t::covenant_option_str() const
{
  if ( m_covenant == covenant_e::INVALID )
  {
    return {};
  }

  return fmt::format( "covenant={}", util::covenant_type_string( m_covenant ) );
}

std::unique_ptr<expr_t> covenant_state_t::create_expression(
    util::span<const util::string_view> expr_str ) const
{
  if ( expr_str.size() < 2 )
  {
    return nullptr;
  }

  if ( util::str_compare_ci( expr_str[ 0 ], "covenant" ) )
  {
    auto covenant = util::parse_covenant_string( expr_str[ 1 ] );
    if ( covenant == covenant_e::INVALID )
    {
      throw std::invalid_argument(
          fmt::format( "Invalid covenant string '{}'.", expr_str[ 1 ] ) );
    }
    bool active = covenant == covenant_e::DISABLED ? !enabled() : covenant == m_covenant;
    return expr_t::create_constant( "covenant", as<double>( active ) );
  }
  else if ( util::str_compare_ci( expr_str[ 0 ], "conduit" ) )
  {
    const auto conduit_ability = get_conduit_ability( expr_str[ 1 ], true );
    if ( !conduit_ability.ok() )
    {
      return expr_t::create_constant( "conduit_nok", 0.0 );
    }

    if ( expr_str.size() == 2 )
    {
      return expr_t::create_constant( "conduit_enabled", conduit_ability.ok() );
    }

    if ( expr_str.size() == 3 )
    {
      if ( util::str_compare_ci( expr_str[ 2 ], "enabled" ) )
      {
        return expr_t::create_constant( "conduit_enabled", as<double>( conduit_ability.ok() ) );
      }
      else if ( util::str_compare_ci( expr_str[ 2 ], "rank" ) )
      {
        return expr_t::create_constant( "conduit_rank", as<double>( conduit_ability.rank() ) );
      }
      else if ( util::str_compare_ci( expr_str[ 2 ], "value" ) )
      {
        return expr_t::create_constant( "conduit_value", conduit_ability.value() );
      }
      else if ( util::str_compare_ci( expr_str[ 2 ], "time_value" ) )
      {
        return expr_t::create_constant( "conduit_time_value", conduit_ability.time_value() );
      }
    }

    throw std::invalid_argument( fmt::format( "Invalid conduit string '{}'", fmt::join( expr_str, "." ) ) );
  }
  else if ( util::str_compare_ci( expr_str[ 0 ], "soulbind" ) )
  {
    auto soulbind_spell = get_soulbind_ability( expr_str[ 1 ], true );
    if ( soulbind_spell == spell_data_t::nil() )
    {
      throw std::invalid_argument(
          fmt::format( "Invalid soulbind ability '{}'", expr_str[ 1 ] ) );
    }

    if ( expr_str.size() == 2 || ( expr_str.size() == 3 && util::str_compare_ci( expr_str[ 2 ], "enabled" ) ) )
    {
      return expr_t::create_constant( "soulbind_enabled", soulbind_spell->ok() );
    }
  }

  return nullptr;
}

void covenant_state_t::copy_state( const std::unique_ptr<covenant_state_t>& other )
{
  m_covenant = other->m_covenant;
  m_conduits = other->m_conduits;
  m_soulbinds = other->m_soulbinds;
  m_renown = other->m_renown;

  m_soulbind_str = other->m_soulbind_str;
  m_covenant_str = other->m_covenant_str;
  m_renown_level = other->m_renown_level;
}

void covenant_state_t::register_options( player_t* player )
{
  player->add_option( opt_func( "soulbind", [this](sim_t* sim, util::string_view name, util::string_view value) { return parse_soulbind_clear( sim, name, value ); } ) );
  player->add_option( opt_func( "soulbind+", [this](sim_t* sim, util::string_view name, util::string_view value) { return parse_soulbind( sim, name, value ); } ) );
  player->add_option( opt_func( "covenant", [this](sim_t* sim, util::string_view name, util::string_view value) { return parse_covenant( sim, name, value ); } ) );
  player->add_option( opt_func( "renown", [this](sim_t* sim, util::string_view name, util::string_view value) { return parse_renown( sim, name, value ); } ) );
}

unsigned covenant_state_t::get_covenant_ability_spell_id( bool generic ) const
{
  if ( !enabled() )
    return 0U;

  for ( const auto& e : covenant_ability_entry_t::data( m_player->dbc->ptr ) )
  {
    if ( e.covenant_id != static_cast<unsigned>( m_covenant ) )
      continue;

    if ( e.class_id != as<unsigned>( util::class_id( m_player->type ) ) && !e.ability_type )
      continue;

    if ( e.ability_type != static_cast<unsigned>( generic ) )
      continue;

    if ( !m_player->find_spell( e.spell_id )->ok() )
      continue;

    return e.spell_id;
  }

  return 0U;
}

report::sc_html_stream& covenant_state_t::generate_report( report::sc_html_stream& root ) const
{
  if ( !enabled() )
    return root;

  const sim_t& sim = *( m_player->sim );

  root.format( "<tr class=\"left\"><th class=\"nowrap\">{} ({})</th><td><ul class=\"float\">\n",
               util::covenant_type_string( type(), true ), renown() );

  auto cv_spell = m_player->find_spell( get_covenant_ability_spell_id() );
  root.format( "<li>{}</li>\n", report_decorators::decorated_spell_name( sim, *cv_spell ) );

  for ( const auto& e : conduit_entry_t::data( m_player->dbc->ptr ) )
  {
    for ( const auto& [ conduit_id, rank ] : m_conduits )
    {
      if ( conduit_id == e.id )
      {
        const auto& conduitRankData = conduit_rank_entry_t::find( conduit_id, rank, m_player->is_ptr() );
        auto conduitData = conduit_data_t( m_player, conduitRankData );
        root.format( "<li class=\"nowrap\">{} ({})</li>\n",
                     report_decorators::decorated_conduit_name( sim, conduitData ), rank + 1 );
      }
    }
  }

  root << "</ul></td></tr>\n";

  if ( !m_soulbinds.empty() )
  {
    root << "<tr class=\"left\"><th></th><td><ul class=\"float\">\n";

    for ( const auto& sb : m_soulbinds )
    {
      auto sb_spell = m_player->find_spell( sb );
      root.format( "<li class=\"nowrap\">{}</li>\n", report_decorators::decorated_spell_name( sim, *sb_spell ) );
    }

    root << "</ul></td></tr>\n";
  }

  if ( !m_renown.empty() )
  {
    root << "<tr class=\"left\"><th></th><td><ul class=\"float\">\n";

    for ( const auto& r : m_renown )
    {
      auto r_spell = m_player->find_spell( r );
      root.format( "<li class=\"nowrap\">{}</li>\n", report_decorators::decorated_spell_name( sim, *r_spell ) );
    }

    root << "</ul></td></tr>\n";
  }

  return root;
}

// Conduit checking function for default profile generation in report_helper::check_gear()
void covenant_state_t::check_conduits( util::string_view tier_name, unsigned max_conduit_rank ) const
{
  // Copied logic from covenant_state_t::generate_report(), feel free to improve it
  for ( const auto& e : conduit_entry_t::data( m_player->dbc->ptr ) )
  {
    for ( const auto& [conduit_id, rank] : m_conduits )
    {
      if ( conduit_id == e.id )
      {
        unsigned int conduit_rank = rank + 1;
        if ( conduit_rank != max_conduit_rank )
        {
          m_player -> sim -> error( "Player {} has conduit {} equipped at rank {}, conduit rank for {} is {}.\n",
                                    m_player -> name(), e.name, conduit_rank, tier_name, max_conduit_rank );
        }
      }
    }
  }
  // TODO?: check conduit count too? Not sure if it's possible
  // It doesn't seem like we extract conduit type or soulbind trees from spelldata
}

covenant_cb_base_t::covenant_cb_base_t( bool on_class, bool on_base )
  : trigger_on_class( on_class ), trigger_on_base( on_base )
{}

covenant_ability_cast_cb_t::covenant_ability_cast_cb_t( player_t* p, const special_effect_t& e )
  : dbc_proc_callback_t( p, e ),
    class_abilities(),
    base_ability( p->covenant->get_covenant_ability_spell_id( true ) ),
    cb_list()
{
  class_abilities.push_back( p->covenant->get_covenant_ability_spell_id() );

  // Manual overrides for covenant abilities that don't utilize the spells found in __covenant_ability_data dbc table
  if ( p->type == DRUID && p->covenant->type() == covenant_e::KYRIAN )
    class_abilities.push_back( 326446 );
  // Night Fae paladins have 4 different abilities in a cycle, but only Blessing of Summer is in __covenant_ability_data
  if ( p -> type == PALADIN && p -> covenant -> type() == covenant_e::NIGHT_FAE )
  {
    class_abilities.push_back( 328622 ); // Blessing of Autumn
    class_abilities.push_back( 328281 ); // Blessing of Winter
    class_abilities.push_back( 328282 ); // Blessing of Spring
  }
  if ( p -> type == DEATH_KNIGHT && p -> covenant -> type() == covenant_e::NIGHT_FAE )
  {
    class_abilities.push_back( 152280 );  // Defile
  }
  if ( p -> type == WARRIOR && p -> covenant -> type() == covenant_e::VENTHYR )
  {
    class_abilities.push_back( 317349 );  // Condemn Arms
    class_abilities.push_back( 317485 );  // Condemn Fury
  }
  // Fodder to the Flame proc spell (9.0.5 rework)
  if ( p->type == DEMON_HUNTER && p->covenant->type() == covenant_e::NECROLORD )
  {
    class_abilities.push_back( 350570 );
  }
}

void covenant_ability_cast_cb_t::initialize()
{
  listener->sim->print_debug( "Initializing covenant ability cast handler..." );
  listener->callbacks.register_callback( effect.proc_flags(), effect.proc_flags2(), this );
}

void covenant_ability_cast_cb_t::trigger( action_t* a, action_state_t* s )
{
  for ( auto class_ability : class_abilities )
  {
    if ( a -> data().id() == class_ability )
    {
      for ( const auto& cb : cb_list )
        if ( cb -> trigger_on_class )
          cb -> trigger( a, s );
      return;
    }
  }

  if ( a -> data().id() == base_ability )
    for ( const auto& cb : cb_list )
      if ( cb -> trigger_on_base )
        cb -> trigger( a, s );
}

covenant_ability_cast_cb_t* get_covenant_callback( player_t* p )
{
  if ( !p->covenant->enabled() )
    return nullptr;

  if ( !p->covenant->cast_callback )
  {
    auto eff          = new special_effect_t( p );
    eff->name_str     = "covenant_cast_callback";
    eff->proc_flags_  = PF_ALL_DAMAGE;
    eff->proc_flags2_ = PF2_ALL_CAST;
    p->special_effects.push_back( eff );
    p->covenant->cast_callback = new covenant_ability_cast_cb_t( p, *eff );
  }

  return debug_cast<covenant_ability_cast_cb_t*>( p->covenant->cast_callback );
}

struct fleshcraft_t : public spell_t
{
  bool magnificent_skin_active;
  bool pustule_eruption_active;
  bool volatile_solvent_active;

  fleshcraft_t( player_t* p, util::string_view opt )
    : spell_t( "fleshcraft", p, p->find_covenant_spell( "Fleshcraft" ) )
  {
    harmful = may_crit = may_miss = false;
    channeled = interrupt_auto_attack = true;

    parse_options( opt );
  }

  void init_finished() override
  {
    spell_t::init_finished();

    magnificent_skin_active = player->find_soulbind_spell( "Emeni's Magnificent Skin" )->ok();
    volatile_solvent_active = player->find_soulbind_spell( "Volatile Solvent" )->ok();

    pustule_eruption_active = player->find_soulbind_spell( "Pustule Eruption" )->ok();
    if ( pustule_eruption_active )
    {
      action_t* pustule_eruption_damage = player->find_action( "pustule_eruption" );
      if( pustule_eruption_damage )
        add_child( pustule_eruption_damage );
    }
  }

  double composite_haste() const override { return 1.0; }

  void execute() override
  {
    spell_t::execute();

    if ( magnificent_skin_active )
    {
      player->buffs.emenis_magnificent_skin->trigger();
    }

    // This triggers the full duration buff at the start of the cast, regardless of channel
    if ( volatile_solvent_active )
    {
      if ( player->buffs.volatile_solvent_humanoid )
        player->buffs.volatile_solvent_humanoid->trigger();

      if ( player->buffs.volatile_solvent_damage )
        player->buffs.volatile_solvent_damage->trigger();

      if( player->buffs.volatile_solvent_stats )
        player->buffs.volatile_solvent_stats->trigger();
    }

    // Ensure we get the full 9 stack if we are using this precombat without the channel
    if ( is_precombat && pustule_eruption_active && player->buffs.trembling_pustules )
    {
      player->buffs.trembling_pustules->trigger( sim->shadowlands_opts.precombat_pustules );
    }
  }

  void last_tick( dot_t* d ) override
  {
    spell_t::last_tick( d );

    if ( pustule_eruption_active && player->buffs.trembling_pustules )
    {
      // Hardcoded at 3 stacks per 1s of channeling in tooltip, granted at the end of the channel
      // This doesn't appear to always be partial, and is only in increments of 3
      // However, sometimes it is in increments of +3 stacks every 1s (even) tick, need to check more with logs
      int num_stacks = 3 * as<int>( floor( ( base_tick_time * d->current_tick ) / 1_s ) );
      if ( num_stacks > 0 )
        player->buffs.trembling_pustules->trigger( num_stacks );
    }
  }

  timespan_t composite_dot_duration( const action_state_t* s ) const override
  {
    // Channeling and pre-combat don't play nicely together, so need to work around this to get the buffs
    return is_precombat ? timespan_t::zero() : spell_t::composite_dot_duration( s );
  }
};

action_t* create_action( player_t* player, util::string_view name, util::string_view options )
{
  if ( util::str_compare_ci( name, "fleshcraft" ) ) return new fleshcraft_t( player, options );

  return nullptr;
}

bool parse_blizzard_covenant_information( player_t*               player,
                                          const rapidjson::Value& covenant_data )
{
  if ( !player->sim->shadowlands_opts.enabled || !covenant_data.HasMember( "chosen_covenant" ) ||
       !covenant_data[ "chosen_covenant" ].HasMember( "name" ) )
  {
    return true;
  }

  std::string covenant_str = covenant_data[ "chosen_covenant" ][ "name" ].GetString();
  util::tokenize( covenant_str );
  auto covenant = util::parse_covenant_string( covenant_str );
  if ( covenant == covenant_e::INVALID )
  {
    return false;
  }

  player->covenant->set_type( covenant );

  if ( covenant_data.HasMember( "renown_level" ) )
  {
    unsigned renown_level = covenant_data[ "renown_level" ].GetUint();
    player->covenant->set_renown_level( renown_level );
  }

  // The rest of the code cannot be run because Blizzard API does not indicate the active
  // path.
  //return true;

  if ( !covenant_data.HasMember( "soulbinds" ) || !covenant_data[ "soulbinds" ].IsArray() )
  {
    return true;
  }

  for ( auto i = 0U; i < covenant_data[ "soulbinds" ].Size(); ++i )
  {
    const auto& soulbind = covenant_data[ "soulbinds" ][ i ];
    if ( !soulbind.HasMember( "is_active" ) || !soulbind[ "is_active" ].GetBool() )
    {
      continue;
    }

    std::string soulbind_name = soulbind[ "soulbind" ][ "name" ].GetString();
    util::tokenize( soulbind_name );
    unsigned soulbind_id = soulbind[ "soulbind" ][ "id" ].GetUint();

    player->covenant->set_soulbind_id( fmt::format( "{}:{}", soulbind_name, soulbind_id ) );

    for ( auto trait_idx = 0U; trait_idx < soulbind[ "traits" ].Size(); ++trait_idx )
    {
      const auto& entry = soulbind[ "traits" ][ trait_idx ];
      // Soulbind spell
      if ( entry.HasMember( "trait" ) )
      {
        const auto& data_entry = soulbind_ability_entry_t::find_by_soulbind_id(
            entry[ "trait" ][ "id" ].GetUint(), player->dbc->ptr );
        if ( !data_entry.spell_id )
        {
          continue;
        }

        player->covenant->add_soulbind( data_entry.spell_id );
      }
      // Conduit
      else
      {
        const auto& conduit = entry[ "conduit_socket" ];
        if ( !conduit.HasMember( "socket" ) )
        {
          continue;
        }

        unsigned conduit_rank = conduit[ "socket" ][ "rank" ].GetUint() - 1;
        unsigned tier = entry[ "tier" ].GetUint();
        unsigned order = entry[ "display_order" ].GetUint();
        // TODO: retain state in conduit_entry_t for html report differentiation of empowered vs non
        if ( player->covenant->is_conduit_socket_empowered( soulbind_id, tier, order ) )
          conduit_rank += 2;

        player->covenant->add_conduit( conduit[ "socket" ][ "conduit" ][ "id" ].GetUint(), conduit_rank );
      }
    }

    break;
  }

  return true;
}

}  // namespace covenant

namespace report_decorators
{
std::string decorated_conduit_name( const sim_t& sim, const conduit_data_t& conduit )
{
  auto rank_str = fmt::format( "rank={}", conduit.rank() - 1 );
  return decorated_spell_name( sim, *( conduit.operator->() ), rank_str );
}
} // namespace report_decorators
