#include "simulationcraft.hpp"
#include "class_modules/apl/apl_rogue.hpp"

namespace rogue_apl {

std::string potion( const player_t* p )
{
  return ( ( p->true_level >= 71 ) ? "tempered_potion_3" :
           ( p->true_level >= 61 ) ? "elemental_potion_of_ultimate_power_3" :
           ( p->true_level >= 51 ) ? "potion_of_spectral_agility" :
           ( p->true_level >= 40 ) ? "potion_of_unbridled_fury" :
           ( p->true_level >= 35 ) ? "draenic_agility" :
           "disabled" );
}

std::string flask( const player_t* p )
{
  if ( p->specialization() == ROGUE_OUTLAW && p->true_level >= 71 )
    return "flask_of_tempered_versatility_3";

  return ( ( p->true_level >= 71 ) ? "flask_of_alchemical_chaos_3" :
           ( p->true_level >= 61 ) ? "iced_phial_of_corrupting_rage_3" :
           ( p->true_level >= 51 ) ? "spectral_flask_of_power" :
           ( p->true_level >= 40 ) ? "greater_flask_of_the_currents" :
           ( p->true_level >= 35 ) ? "greater_draenic_agility_flask" :
           "disabled" );
}

std::string food( const player_t* p )
{
  return ( ( p->true_level >= 71 ) ? "feast_of_the_divine_day" :
           ( p->true_level >= 61 ) ? "fated_fortune_cookie" :
           ( p->true_level >= 51 ) ? "feast_of_gluttonous_hedonism" :
           ( p->true_level >= 45 ) ? "famine_evaluator_and_snack_table" :
           ( p->true_level >= 40 ) ? "lavish_suramar_feast" :
           "disabled" );
}

std::string rune( const player_t* p )
{
  return ( ( p->true_level >= 80 ) ? "crystallized" :
           ( p->true_level >= 70 ) ? "draconic" :
           ( p->true_level >= 60 ) ? "veiled" :
           ( p->true_level >= 50 ) ? "battle_scarred" :
           ( p->true_level >= 45 ) ? "defiled" :
           ( p->true_level >= 40 ) ? "hyper" :
           "disabled" );
}

std::string temporary_enchant( const player_t* p )
{
  return ( ( p->true_level >= 71 ) ? "main_hand:ironclaw_whetstone_3/off_hand:ironclaw_whetstone_3" :
           ( p->true_level >= 61 ) ? "main_hand:buzzing_rune_3/off_hand:buzzing_rune_3" :
           ( p->true_level >= 51 ) ? "main_hand:shaded_sharpening_stone/off_hand:shaded_sharpening_stone" :
           "disabled" );
}

//assassination_apl_start
void assassination( player_t* p )
{
  action_priority_list_t* default_ = p->get_action_priority_list( "default" );
  action_priority_list_t* precombat = p->get_action_priority_list( "precombat" );
  action_priority_list_t* aoe_dot = p->get_action_priority_list( "aoe_dot" );
  action_priority_list_t* cds = p->get_action_priority_list( "cds" );
  action_priority_list_t* core_dot = p->get_action_priority_list( "core_dot" );
  action_priority_list_t* direct = p->get_action_priority_list( "direct" );
  action_priority_list_t* items = p->get_action_priority_list( "items" );
  action_priority_list_t* misc_cds = p->get_action_priority_list( "misc_cds" );
  action_priority_list_t* shiv = p->get_action_priority_list( "shiv" );
  action_priority_list_t* stealthed = p->get_action_priority_list( "stealthed" );
  action_priority_list_t* vanish = p->get_action_priority_list( "vanish" );

  precombat->add_action( "apply_poison" );
  precombat->add_action( "snapshot_stats" );
  precombat->add_action( "variable,name=trinket_sync_slot,value=1,if=trinket.1.has_stat.any_dps&(!trinket.2.has_stat.any_dps|trinket.1.cooldown.duration>=trinket.2.cooldown.duration)&!trinket.2.is.treacherous_transmitter|trinket.1.is.treacherous_transmitter", "Check which trinket slots have Stat Values" );
  precombat->add_action( "variable,name=trinket_sync_slot,value=2,if=trinket.2.has_stat.any_dps&(!trinket.1.has_stat.any_dps|trinket.2.cooldown.duration>trinket.1.cooldown.duration)&!trinket.1.is.treacherous_transmitter|trinket.2.is.treacherous_transmitter" );
  precombat->add_action( "variable,name=effective_spend_cp,value=cp_max_spend-2<?5*talent.hand_of_fate", "Determine combo point finish condition" );
  precombat->add_action( "stealth", "Pre-cast Slice and Dice if possible" );
  precombat->add_action( "slice_and_dice,precombat_seconds=1" );

  default_->add_action( "stealth", "Restealth if possible (no vulnerable enemies in combat)" );
  default_->add_action( "kick", "Interrupt on cooldown to allow simming interactions with that" );
  default_->add_action( "variable,name=single_target,value=spell_targets.fan_of_knives<2", "Conditional to check if there is only one enemy" );
  default_->add_action( "variable,name=regen_saturated,value=energy.regen_combined>30", "Combined Energy Regen needed to saturate" );
  default_->add_action( "variable,name=in_cooldowns,value=dot.deathmark.ticking|dot.kingsbane.ticking|debuff.shiv.up", "Pooling Setup, check for cooldowns" );
  default_->add_action( "variable,name=upper_limit_energy,value=energy.pct>=(50-10*talent.vicious_venoms.rank)", "Check upper bounds of energy to begin spending" );
  default_->add_action( "variable,name=cd_soon,value=cooldown.kingsbane.remains<3&!cooldown.kingsbane.ready", "Checking for cooldowns soon" );
  default_->add_action( "variable,name=not_pooling,value=variable.in_cooldowns|!variable.cd_soon&buff.darkest_night.up|variable.upper_limit_energy|fight_remains<=20", "Pooling Condition all together" );
  default_->add_action( "variable,name=scent_effective_max_stacks,value=(spell_targets.fan_of_knives*talent.scent_of_blood.rank*2)>?20", "Check what the maximum Scent of Blood stacks is currently" );
  default_->add_action( "variable,name=scent_saturation,value=buff.scent_of_blood.stack>=variable.scent_effective_max_stacks", "We are Scent Saturated when our stack count is hitting the maximum" );
  default_->add_action( "call_action_list,name=stealthed,if=stealthed.rogue|stealthed.improved_garrote|master_assassin_remains>0", "Call Stealthed Actions" );
  default_->add_action( "call_action_list,name=cds", "Call Cooldowns" );
  default_->add_action( "call_action_list,name=core_dot", "Call Core DoT effects" );
  default_->add_action( "call_action_list,name=aoe_dot,if=!variable.single_target", "Call AoE DoTs when in AoE" );
  default_->add_action( "call_action_list,name=direct", "Call Direct Damage Abilities" );
  default_->add_action( "arcane_torrent,if=energy.deficit>=15+energy.regen_combined" );
  default_->add_action( "arcane_pulse" );
  default_->add_action( "lights_judgment" );
  default_->add_action( "bag_of_tricks" );

  aoe_dot->add_action( "variable,name=dot_finisher_condition,value=combo_points>=variable.effective_spend_cp&(pmultiplier<=1)", "AoE Damage over time abilities Helper Variable to check basic finisher conditions" );
  aoe_dot->add_action( "crimson_tempest,target_if=min:remains,if=spell_targets>=2&variable.dot_finisher_condition&refreshable&target.time_to_die-remains>6", "Crimson Tempest on 2+ Targets" );
  aoe_dot->add_action( "garrote,cycle_targets=1,if=combo_points.deficit>=1&(pmultiplier<=1)&refreshable&!variable.regen_saturated&target.time_to_die-remains>12", "Garrote upkeep in AoE to reach energy saturation" );
  aoe_dot->add_action( "rupture,cycle_targets=1,if=variable.dot_finisher_condition&refreshable&(!dot.kingsbane.ticking|buff.cold_blood.up)&(!variable.regen_saturated&(talent.scent_of_blood.rank=2|talent.scent_of_blood.rank<=1&(buff.indiscriminate_carnage.up|target.time_to_die-remains>15)))&target.time_to_die-remains>(7+(talent.dashing_scoundrel*5)+(variable.regen_saturated*6))&!buff.darkest_night.up", "Rupture upkeep in AoE to reach energy or scent of blood saturation" );
  aoe_dot->add_action( "rupture,cycle_targets=1,if=variable.dot_finisher_condition&refreshable&(!dot.kingsbane.ticking|buff.cold_blood.up)&variable.regen_saturated&!variable.scent_saturation&target.time_to_die-remains>19&!buff.darkest_night.up" );
  aoe_dot->add_action( "garrote,if=refreshable&combo_points.deficit>=1&(pmultiplier<=1|remains<=tick_time&spell_targets.fan_of_knives>=3)&(remains<=tick_time*2&spell_targets.fan_of_knives>=3)&(target.time_to_die-remains)>4&master_assassin_remains=0", "Garrote as a special generator for the last CP before a finisher for edge case handling" );

  cds->add_action( "variable,name=deathmark_ma_condition,value=!talent.master_assassin.enabled|dot.garrote.ticking", "Cooldowns Wait on Deathmark for Garrote with MA and check for Kingsbane" );
  cds->add_action( "variable,name=deathmark_kingsbane_condition,value=!talent.kingsbane|cooldown.kingsbane.remains<=2" );
  cds->add_action( "variable,name=deathmark_condition,value=!stealthed.rogue&buff.slice_and_dice.remains>5&dot.rupture.ticking&(buff.envenom.up|spell_targets.fan_of_knives>1)&!debuff.deathmark.up&variable.deathmark_ma_condition&variable.deathmark_kingsbane_condition", "Deathmark to be used if not stealthed, Rupture is up, and all other talent conditions are satisfied" );
  cds->add_action( "call_action_list,name=items", "Usages for various special-case Trinkets and other Cantrips if applicable" );
  cds->add_action( "invoke_external_buff,name=power_infusion,if=dot.deathmark.ticking", "Invoke Externals to Deathmark" );
  cds->add_action( "deathmark,if=(variable.deathmark_condition&target.time_to_die>=10)|fight_remains<=20", "Cast Deathmark if the target will survive long enough" );
  cds->add_action( "call_action_list,name=shiv", "Check for Applicable Shiv usage" );
  cds->add_action( "kingsbane,if=(debuff.shiv.up|cooldown.shiv.remains<6)&(buff.envenom.up|spell_targets.fan_of_knives>1)&(cooldown.deathmark.remains>=50|dot.deathmark.ticking)|fight_remains<=15" );
  cds->add_action( "thistle_tea,if=!buff.thistle_tea.up&debuff.shiv.remains>=6|!buff.thistle_tea.up&dot.kingsbane.ticking&dot.kingsbane.remains<=6|!buff.thistle_tea.up&fight_remains<=cooldown.thistle_tea.charges*6", "Use with shiv or in niche cases at the end of Kingsbane if not already up" );
  cds->add_action( "call_action_list,name=misc_cds", "Potion/Racials/Other misc cooldowns" );
  cds->add_action( "call_action_list,name=vanish,if=!stealthed.all&master_assassin_remains=0" );
  cds->add_action( "cold_blood,use_off_gcd=1,if=(buff.fatebound_coin_tails.stack>0&buff.fatebound_coin_heads.stack>0)|debuff.shiv.up&(cooldown.deathmark.remains>50|!talent.inevitabile_end&effective_combo_points>=variable.effective_spend_cp)", "Cold Blood for Edge Case or Envenoms during shiv" );

  core_dot->add_action( "garrote,if=combo_points.deficit>=1&(pmultiplier<=1)&refreshable&target.time_to_die-remains>12", "Core damage over time abilities used everywhere   Maintain Garrote" );
  core_dot->add_action( "rupture,if=combo_points>=variable.effective_spend_cp&(pmultiplier<=1)&refreshable&target.time_to_die-remains>(4+(talent.dashing_scoundrel*5)+(variable.regen_saturated*6))&(!buff.darkest_night.up|talent.caustic_spatter&!debuff.caustic_spatter.up)", "Maintain Rupture unless darkest night is up" );
  core_dot->add_action( "crimson_tempest,if=combo_points>=variable.effective_spend_cp&refreshable&(!buff.darkest_night.up)&!talent.amplifying_poison", "Maintain Crimson Tempest" );

  direct->add_action( "envenom,if=!buff.darkest_night.up&combo_points>=variable.effective_spend_cp&(variable.not_pooling|debuff.amplifying_poison.stack>=20|!variable.single_target)&!buff.vanish.up", "Direct Damage Abilities Envenom at applicable cp if not pooling, capped on amplifying poison stacks, on an animacharged CP, or in aoe." );
  direct->add_action( "envenom,if=buff.darkest_night.up&effective_combo_points>=cp_max_spend", "Special Envenom handling for Darkest Night" );
  direct->add_action( "variable,name=use_filler,value=combo_points<=variable.effective_spend_cp&!variable.cd_soon|variable.not_pooling|!variable.single_target", "Check if we should be using a filler" );
  direct->add_action( "variable,name=use_caustic_filler,value=talent.caustic_spatter&dot.rupture.ticking&(!debuff.caustic_spatter.up|debuff.caustic_spatter.remains<=2)&combo_points.deficit>=1&!variable.single_target", "Maintain Caustic Spatter" );
  direct->add_action( "mutilate,if=variable.use_caustic_filler" );
  direct->add_action( "ambush,if=variable.use_caustic_filler" );
  direct->add_action( "fan_of_knives,if=buff.darkest_night.up&combo_points=6", "Fan of Knives at 6cp for Darkest Night" );
  direct->add_action( "fan_of_knives,if=variable.use_filler&!priority_rotation&(spell_targets.fan_of_knives>=3-(talent.momentum_of_despair&talent.thrown_precision)|buff.clear_the_witnesses.up&!talent.vicious_venoms)", "Fan of Knives at 3+ targets, accounting for various edge cases" );
  direct->add_action( "fan_of_knives,target_if=!dot.deadly_poison_dot.ticking&(!priority_rotation|dot.garrote.ticking|dot.rupture.ticking),if=variable.use_filler&spell_targets.fan_of_knives>=3-(talent.momentum_of_despair&talent.thrown_precision)", "Fan of Knives to apply poisons if inactive on any target (or any bleeding targets with priority rotation) at 3T" );
  direct->add_action( "ambush,if=variable.use_filler&(buff.blindside.up|stealthed.rogue)&(!dot.kingsbane.ticking|debuff.deathmark.down|buff.blindside.up)", "Ambush on Blindside/Subterfuge. Do not use Ambush from stealth during Kingsbane & Deathmark." );
  direct->add_action( "mutilate,target_if=!dot.deadly_poison_dot.ticking&!debuff.amplifying_poison.up,if=variable.use_filler&spell_targets.fan_of_knives=2", "Tab-Mutilate to apply Deadly Poison at 2 targets" );
  direct->add_action( "mutilate,if=variable.use_filler", "Fallback Mutilate" );

  items->add_action( "variable,name=base_trinket_condition,value=dot.rupture.ticking&cooldown.deathmark.remains<2|dot.deathmark.ticking|fight_remains<=22", "Special Case Trinkets" );
  items->add_action( "use_item,name=ashes_of_the_embersoul,use_off_gcd=1,if=(dot.kingsbane.ticking&dot.kingsbane.remains<=11)|fight_remains<=22" );
  items->add_action( "use_item,name=algethar_puzzle_box,use_off_gcd=1,if=variable.base_trinket_condition" );
  items->add_action( "use_item,name=treacherous_transmitter,use_off_gcd=1,if=variable.base_trinket_condition" );
  items->add_action( "use_item,name=mad_queens_mandate,if=cooldown.deathmark.remains>=30&!dot.deathmark.ticking|fight_remains<=3" );
  items->add_action( "do_treacherous_transmitter_task,use_off_gcd=1,if=dot.deathmark.ticking&variable.single_target|buff.realigning_nexus_convergence_divergence.up&buff.realigning_nexus_convergence_divergence.remains<=2|buff.cryptic_instructions.up&buff.cryptic_instructions.remains<=2|buff.errant_manaforge_emission.up&buff.errant_manaforge_emission.remains<=2|fight_remains<=15" );
  items->add_action( "use_item,name=imperfect_ascendancy_serum,use_off_gcd=1,if=variable.base_trinket_condition" );
  items->add_action( "use_items,slots=trinket1,if=(variable.trinket_sync_slot=1&(debuff.deathmark.up|fight_remains<=20)|(variable.trinket_sync_slot=2&(!trinket.2.cooldown.ready&dot.kingsbane.ticking|!debuff.deathmark.up&cooldown.deathmark.remains>20&dot.kingsbane.ticking))|!variable.trinket_sync_slot)", "Fallback case for using stat trinkets" );
  items->add_action( "use_items,slots=trinket2,if=(variable.trinket_sync_slot=2&(debuff.deathmark.up|fight_remains<=20)|(variable.trinket_sync_slot=1&(!trinket.1.cooldown.ready&dot.kingsbane.ticking|!debuff.deathmark.up&cooldown.deathmark.remains>20&dot.kingsbane.ticking))|!variable.trinket_sync_slot)" );

  misc_cds->add_action( "potion,if=buff.bloodlust.react|fight_remains<30|debuff.deathmark.up", "Miscellaneous Cooldowns Potion" );
  misc_cds->add_action( "blood_fury,if=debuff.deathmark.up", "Various special racials to be synced with cooldowns" );
  misc_cds->add_action( "berserking,if=debuff.deathmark.up" );
  misc_cds->add_action( "fireblood,if=debuff.deathmark.up" );
  misc_cds->add_action( "ancestral_call,if=(!talent.kingsbane&debuff.deathmark.up&debuff.shiv.up)|(talent.kingsbane&debuff.deathmark.up&dot.kingsbane.ticking&dot.kingsbane.remains<8)" );

  shiv->add_action( "variable,name=shiv_condition,value=!debuff.shiv.up&dot.garrote.ticking&dot.rupture.ticking&spell_targets.fan_of_knives<=5", "Shiv conditions" );
  shiv->add_action( "variable,name=shiv_kingsbane_condition,value=talent.kingsbane&buff.envenom.up&variable.shiv_condition" );
  shiv->add_action( "shiv,if=talent.arterial_precision&variable.shiv_condition&spell_targets.fan_of_knives>=4&dot.crimson_tempest.ticking", "Shiv for aoe with Arterial Precision" );
  shiv->add_action( "shiv,if=!talent.lightweight_shiv.enabled&variable.shiv_kingsbane_condition&(dot.kingsbane.ticking&dot.kingsbane.remains<8|!dot.kingsbane.ticking&cooldown.kingsbane.remains>=20)&(!talent.crimson_tempest.enabled|variable.single_target|dot.crimson_tempest.ticking)", "Shiv cases for Kingsbane" );
  shiv->add_action( "shiv,if=buff.darkest_night.up&combo_points>=variable.effective_spend_cp&buff.lingering_darkness.up", "Shiv for big Darkest Night Envenom during Lingering Darkness" );
  shiv->add_action( "shiv,if=talent.lightweight_shiv.enabled&variable.shiv_kingsbane_condition&(dot.kingsbane.ticking&dot.kingsbane.remains<8|cooldown.kingsbane.remains<=1&cooldown.shiv.charges_fractional>=1.7)" );
  shiv->add_action( "shiv,if=talent.arterial_precision&!debuff.shiv.up&dot.garrote.ticking&dot.rupture.ticking&debuff.deathmark.up", "Fallback shiv for arterial during deathmark" );
  shiv->add_action( "shiv,if=!talent.kingsbane&!talent.arterial_precision&variable.shiv_condition&(!talent.crimson_tempest.enabled|variable.single_target|dot.crimson_tempest.ticking)", "Fallback if no special cases apply" );
  shiv->add_action( "shiv,if=fight_remains<=cooldown.shiv.charges*8", "Dump Shiv on fight end" );

  stealthed->add_action( "pool_resource,for_next=1", "Stealthed Actions" );
  stealthed->add_action( "ambush,if=!debuff.deathstalkers_mark.up&talent.deathstalkers_mark&combo_points<variable.effective_spend_cp&(dot.rupture.ticking|variable.single_target|!talent.subterfuge)", "Apply Deathstalkers Mark if it has fallen off or waiting for Rupture in AoE" );
  stealthed->add_action( "shiv,if=talent.kingsbane&dot.kingsbane.ticking&dot.kingsbane.remains<8&(!debuff.shiv.up&debuff.shiv.remains<1)&buff.envenom.up", "Make sure to have Shiv up during Kingsbane as a final check" );
  stealthed->add_action( "envenom,if=effective_combo_points>=variable.effective_spend_cp&dot.kingsbane.ticking&buff.envenom.remains<=3&(debuff.deathstalkers_mark.up|buff.cold_blood.up|buff.darkest_night.up&combo_points=7)", "Envenom to maintain the buff during Subterfuge" );
  stealthed->add_action( "envenom,if=effective_combo_points>=variable.effective_spend_cp&buff.master_assassin_aura.up&variable.single_target&(debuff.deathstalkers_mark.up|buff.cold_blood.up|buff.darkest_night.up&combo_points=7)", "Envenom during Master Assassin in single target" );
  stealthed->add_action( "rupture,target_if=effective_combo_points>=variable.effective_spend_cp&buff.indiscriminate_carnage.up&refreshable&(!variable.regen_saturated|!variable.scent_saturation|!dot.rupture.ticking)&target.time_to_die>15", "Rupture during Indiscriminate Carnage" );
  stealthed->add_action( "garrote,target_if=min:remains,if=stealthed.improved_garrote&(remains<12|pmultiplier<=1|(buff.indiscriminate_carnage.up&active_dot.garrote<spell_targets.fan_of_knives))&!variable.single_target&target.time_to_die-remains>2&combo_points.deficit>2-buff.darkest_night.up*2", "Improved Garrote: Apply or Refresh with buffed Garrotes, accounting for Indiscriminate Carnage" );
  stealthed->add_action( "garrote,if=stealthed.improved_garrote&(pmultiplier<=1|refreshable)&combo_points.deficit>=1+2*talent.shrouded_suffocation" );

  vanish->add_action( "pool_resource,for_next=1,extra_amount=45", "Stealth Cooldowns Vanish Sync for Improved Garrote with Deathmark" );
  vanish->add_action( "vanish,if=!buff.fatebound_lucky_coin.up&effective_combo_points>=variable.effective_spend_cp&(buff.fatebound_coin_tails.stack>=5|buff.fatebound_coin_heads.stack>=5)", "Vanish to fish for Fateful Ending" );
  vanish->add_action( "vanish,if=!talent.master_assassin&!talent.indiscriminate_carnage&talent.improved_garrote&cooldown.garrote.up&(dot.garrote.pmultiplier<=1|dot.garrote.refreshable)&(debuff.deathmark.up|cooldown.deathmark.remains<4)&combo_points.deficit>=(spell_targets.fan_of_knives>?4)", "Vanish to spread Garrote during Deathmark without Indiscriminate Carnage" );
  vanish->add_action( "pool_resource,for_next=1,extra_amount=45" );
  vanish->add_action( "vanish,if=talent.indiscriminate_carnage&talent.improved_garrote&cooldown.garrote.up&(dot.garrote.pmultiplier<=1|dot.garrote.refreshable)&spell_targets.fan_of_knives>2&(target.time_to_die-remains>15|raid_event.adds.in>20)", "Vanish for cleaving Garrotes with Indiscriminate Carnage" );
  vanish->add_action( "vanish,if=talent.master_assassin&debuff.deathmark.up&dot.kingsbane.remains<=6+3*talent.subterfuge.rank", "Vanish fallback for Master Assassin" );
  vanish->add_action( "vanish,if=talent.improved_garrote&cooldown.garrote.up&(dot.garrote.pmultiplier<=1|dot.garrote.refreshable)&(debuff.deathmark.up|cooldown.deathmark.remains<4)&raid_event.adds.in>30", "Vanish fallback for Improved Garrote during Deathmark if no add waves are expected" );
}
//assassination_apl_end

//outlaw_apl_start
void outlaw( player_t* p )
{
  action_priority_list_t* default_ = p->get_action_priority_list( "default" );
  action_priority_list_t* precombat = p->get_action_priority_list( "precombat" );
  action_priority_list_t* build = p->get_action_priority_list( "build" );
  action_priority_list_t* cds = p->get_action_priority_list( "cds" );
  action_priority_list_t* finish = p->get_action_priority_list( "finish" );
  action_priority_list_t* stealth = p->get_action_priority_list( "stealth" );
  action_priority_list_t* stealth_cds = p->get_action_priority_list( "stealth_cds" );
  action_priority_list_t* stealth_cds_off_meta = p->get_action_priority_list( "stealth_cds_off_meta" );

  precombat->add_action( "apply_poison,nonlethal=none,lethal=instant" );
  precombat->add_action( "snapshot_stats", "Snapshot raid buffed stats before combat begins and pre-potting is done." );
  precombat->add_action( "use_item,name=imperfect_ascendancy_serum" );
  precombat->add_action( "stealth,precombat_seconds=2" );
  precombat->add_action( "adrenaline_rush,precombat_seconds=1,if=talent.improved_adrenaline_rush&talent.keep_it_rolling&talent.loaded_dice", "Builds with Keep it Rolling prepull Adrenaline Rush before Roll the Bones to consume Loaded Dice immediately instead of on the next pandemic roll" );
  precombat->add_action( "roll_the_bones,precombat_seconds=1" );
  precombat->add_action( "adrenaline_rush,precombat_seconds=0,if=talent.improved_adrenaline_rush" );

  default_->add_action( "stealth", "Restealth if possible (no vulnerable enemies in combat)" );
  default_->add_action( "kick", "Interrupt on cooldown to allow simming interactions with that" );
  default_->add_action( "variable,name=ambush_condition,value=(talent.hidden_opportunity|combo_points.deficit>=2+talent.improved_ambush+buff.broadside.up)&energy>=50" );
  default_->add_action( "variable,name=finish_condition,value=combo_points>=cp_max_spend-1-(stealthed.all&talent.crackshot|(talent.hand_of_fate|talent.flawless_form)&talent.hidden_opportunity&(buff.audacity.up|buff.opportunity.up))", "Use finishers if at -1 from max combo points, or -2 in Stealth with Crackshot. With the hero trees, Hidden Opportunity builds also finish at -2 if Audacity or Opportunity is active" );
  default_->add_action( "call_action_list,name=cds" );
  default_->add_action( "call_action_list,name=stealth,if=stealthed.all", "High priority stealth list, will fall through if no conditions are met" );
  default_->add_action( "run_action_list,name=finish,if=variable.finish_condition" );
  default_->add_action( "call_action_list,name=build" );
  default_->add_action( "arcane_torrent,if=energy.base_deficit>=15+energy.regen" );
  default_->add_action( "arcane_pulse" );
  default_->add_action( "lights_judgment" );
  default_->add_action( "bag_of_tricks" );

  build->add_action( "ambush,if=talent.hidden_opportunity&buff.audacity.up", "Builders  High priority Ambush for Hidden Opportunity builds" );
  build->add_action( "pistol_shot,if=talent.fan_the_hammer&talent.audacity&talent.hidden_opportunity&buff.opportunity.up&!buff.audacity.up", "With Audacity + Hidden Opportunity + Fan the Hammer, consume Opportunity to proc Audacity any time Ambush is not available" );
  build->add_action( "pistol_shot,if=talent.fan_the_hammer&buff.opportunity.up&(buff.opportunity.stack>=buff.opportunity.max_stack|buff.opportunity.remains<2)", "With Fan the Hammer, consume Opportunity as a higher priority if at max stacks or if it will expire" );
  build->add_action( "pistol_shot,if=talent.fan_the_hammer&buff.opportunity.up&(combo_points.deficit>=(1+(talent.quick_draw+buff.broadside.up)*(talent.fan_the_hammer.rank+1))|combo_points<=talent.ruthlessness)", "With Fan the Hammer, consume Opportunity if it will not overcap CPs, or with 1 CP at minimum" );
  build->add_action( "pistol_shot,if=!talent.fan_the_hammer&buff.opportunity.up&(energy.base_deficit>energy.regen*1.5|combo_points.deficit<=1+buff.broadside.up|talent.quick_draw.enabled|talent.audacity.enabled&!buff.audacity.up)", "If not using Fan the Hammer, then consume Opportunity based on energy, when it will exactly cap CPs, or when using Quick Draw" );
  build->add_action( "pool_resource,for_next=1", "Fallback pooling just so Sinister Strike is never casted if Ambush is available for Hidden Opportunity builds" );
  build->add_action( "ambush,if=talent.hidden_opportunity" );
  build->add_action( "sinister_strike" );

  cds->add_action( "adrenaline_rush,if=!buff.adrenaline_rush.up&(!variable.finish_condition|!talent.improved_adrenaline_rush)|talent.improved_adrenaline_rush&combo_points<=2&!buff.loaded_dice.up", "Cooldowns   Use Adrenaline rush if the buff is missing unless you can finish or with 2 or less cp if loaded dice is missing" );
  cds->add_action( "sprint,if=(trinket.1.is.scroll_of_momentum|trinket.2.is.scroll_of_momentum)&buff.full_momentum.up", "Sprint to further benefit from Scroll of Momentum trinket" );
  cds->add_action( "blade_flurry,if=spell_targets>=2&buff.blade_flurry.remains<gcd", "Maintain Blade Flurry on 2+ targets" );
  cds->add_action( "blade_flurry,if=talent.deft_maneuvers&!variable.finish_condition&(spell_targets>=3&combo_points.deficit=spell_targets+buff.broadside.up|spell_targets>=5)", "With Deft Maneuvers, use Blade Flurry on cooldown at 5+ targets, or at 3-4 targets if missing combo points equal to the amount given" );
  cds->add_action( "keep_it_rolling,if=rtb_buffs>=4&(rtb_buffs.min_remains<1|(buff.broadside.up+buff.ruthless_precision.up+buff.true_bearing.up>=2))", "Use Keep it Rolling with any 4 buffs, unless you only have one of Broadside, Ruthless Precision and True Bearing, then wait until just before the lowest duration buff expires in an attempt to obtain another good buff from Count the Odds." );
  cds->add_action( "keep_it_rolling,if=rtb_buffs>=3&(buff.broadside.up+buff.ruthless_precision.up+buff.true_bearing.up>=2)&(rtb_buffs.min_remains<1|(buff.broadside.up+buff.ruthless_precision.up+buff.true_bearing.up=3))", "Use Keep it Rolling with 3 buffs, if they contain at least 2 of Broadside, Ruthless Precision and True Bearing. If one of the 3 is missing, then wait until just before the lowest buff expires in an attempt to obtain it from Count the Odds." );
  cds->add_action( "roll_the_bones,if=rtb_buffs.will_lose<=buff.loaded_dice.up", "Roll the bones if you have no buffs, or will lose no buffs by rolling. With Loaded Dice up, roll if you have 1 buff or will lose at most 1 buff." );
  cds->add_action( "roll_the_bones,if=talent.keep_it_rolling&buff.loaded_dice.up&rtb_buffs<=2", "KIR builds can also roll with Loaded Dice up and at most 2 buffs in total" );
  cds->add_action( "roll_the_bones,if=talent.hidden_opportunity&buff.loaded_dice.up&rtb_buffs<=2&!buff.broadside.up&!buff.ruthless_precision.up&!buff.true_bearing.up", "HO builds can fish for good buffs by rerolling with 2 buffs and Loaded Dice up if those 2 buffs do not contain either Broadside, Ruthless Precision or True Bearing" );
  cds->add_action( "ghostly_strike,if=combo_points<cp_max_spend" );
  cds->add_action( "use_item,name=imperfect_ascendancy_serum,if=!stealthed.all|fight_remains<=22", "Trinkets that should not be used during stealth and have higher priority than entering stealth" );
  cds->add_action( "use_item,name=mad_queens_mandate,if=!stealthed.all|fight_remains<=5" );
  cds->add_action( "killing_spree,if=variable.finish_condition&!stealthed.all", "Killing Spree has higher priority than stealth cooldowns" );
  cds->add_action( "call_action_list,name=stealth_cds,if=!stealthed.all&talent.crackshot&talent.underhanded_upper_hand&talent.subterfuge&buff.escalating_blade.stack<4&buff.adrenaline_rush.up&variable.finish_condition", "Primary stealth cooldowns for builds using all of uhuh, crackshot, and subterfuge. These builds only use vanish while not already in stealth, when finish condition is active and adrenaline rush is up. Trickster builds also need to use Coup de Grace if available before vanishing." );
  cds->add_action( "call_action_list,name=stealth_cds_off_meta,if=!stealthed.all&(!talent.underhanded_upper_hand|!talent.crackshot|!talent.subterfuge)", "Secondary stealth cds list for off meta builds missing at least one of Underhanded Upper Hand, Crackshot or Subterfuge" );
  cds->add_action( "blade_rush,if=energy.base_time_to_max>4&!stealthed.all", "Use Blade Rush at minimal energy outside of stealth" );
  cds->add_action( "potion,if=buff.bloodlust.react|fight_remains<30|buff.adrenaline_rush.up" );
  cds->add_action( "blood_fury" );
  cds->add_action( "berserking" );
  cds->add_action( "fireblood" );
  cds->add_action( "ancestral_call" );
  cds->add_action( "use_items,slots=trinket1,if=buff.between_the_eyes.up|trinket.1.has_stat.any_dps|fight_remains<=20", "Default conditions for usable items." );
  cds->add_action( "use_items,slots=trinket2,if=buff.between_the_eyes.up|trinket.2.has_stat.any_dps|fight_remains<=20" );

  finish->add_action( "between_the_eyes,if=!talent.crackshot&(buff.between_the_eyes.remains<4|talent.improved_between_the_eyes|talent.greenskins_wickers)&!buff.greenskins_wickers.up", "Finishers Use Between the Eyes to keep the crit buff up, but on cooldown if Improved/Greenskins, and avoid overriding Greenskins" );
  finish->add_action( "between_the_eyes,if=talent.crackshot&(buff.ruthless_precision.up|buff.between_the_eyes.remains<4|!talent.keep_it_rolling|!talent.mean_streak)", "Crackshot builds use Between the Eyes outside of Stealth to refresh the Between the Eyes crit buff or on cd with the Ruthless Precision buff" );
  finish->add_action( "cold_blood" );
  finish->add_action( "coup_de_grace" );
  finish->add_action( "dispatch" );

  stealth->add_action( "cold_blood,if=variable.finish_condition", "Stealth" );
  stealth->add_action( "pool_resource,for_next=1", "Ensure Crackshot BtE is not skipped because of low energy" );
  stealth->add_action( "between_the_eyes,if=variable.finish_condition&talent.crackshot&(!buff.shadowmeld.up|stealthed.rogue)", "High priority Between the Eyes for Crackshot, except not directly out of Shadowmeld" );
  stealth->add_action( "dispatch,if=variable.finish_condition" );
  stealth->add_action( "pistol_shot,if=talent.crackshot&talent.fan_the_hammer.rank>=2&buff.opportunity.stack>=6&(buff.broadside.up&combo_points<=1|buff.greenskins_wickers.up)", "2 Fan the Hammer Crackshot builds can consume Opportunity in stealth with max stacks, Broadside, and low CPs, or with Greenskins active" );
  stealth->add_action( "ambush,if=talent.hidden_opportunity" );

  stealth_cds->add_action( "vanish,if=!talent.killing_spree&!cooldown.between_the_eyes.ready&buff.ruthless_precision.remains>4&(cooldown.keep_it_rolling.remains>150&rtb_buffs.normal>0|!talent.supercharger)", "Main stealth cds list for builds using all of Underhanded Upper Hand, Crackshot and Subterfuge. These builds only use vanish while not already in stealth, when finish condition is active and adrenaline rush is up. Trickster builds also need to use Coup de Grace if available before vanishing. These rules are checked where the list is called since they are common fro all the following vanish conditions  If not using killing spree, vanish if Between the Eyes is on cooldown and Ruthless Precision is up. When playing kir we also hold vanish if we haven't done our first rtb cast after kir use" );
  stealth_cds->add_action( "vanish,if=buff.adrenaline_rush.remains<3&cooldown.adrenaline_rush.remains>10", "Vanish if Adrenaline Rush is about to run out unless remaining cooldown on adrenaline rush is less than 10 sec or available" );
  stealth_cds->add_action( "vanish,if=!talent.killing_spree&buff.supercharge_1.up", "Supercharger builds that do not use killing spree should vanish with the supercharger buff up" );
  stealth_cds->add_action( "vanish,if=cooldown.killing_spree.remains>15", "Killing spree builds can vanish any time killing spree is on cd, preferably with at least 15s left on the cd" );
  stealth_cds->add_action( "vanish,if=cooldown.vanish.full_recharge_time<15", "Vanish if about to cap on vanish charges" );
  stealth_cds->add_action( "vanish,if=fight_remains<8", "Vanish if fight is about to end" );
  stealth_cds->add_action( "shadowmeld,if=variable.finish_condition&!cooldown.vanish.ready" );

  stealth_cds_off_meta->add_action( "vanish,if=talent.underhanded_upper_hand&talent.subterfuge&!talent.crackshot&buff.adrenaline_rush.up&(variable.ambush_condition|!talent.hidden_opportunity)&(!cooldown.between_the_eyes.ready&buff.ruthless_precision.up|buff.ruthless_precision.down|buff.adrenaline_rush.remains<3)", "Off meta builds vanish rules, limited apl support for builds lacking one of the mandatory talents crackshot, underhanded upper hand and subterfuge" );
  stealth_cds_off_meta->add_action( "vanish,if=!talent.underhanded_upper_hand&talent.crackshot&variable.finish_condition" );
  stealth_cds_off_meta->add_action( "vanish,if=!talent.underhanded_upper_hand&!talent.crackshot&talent.hidden_opportunity&!buff.audacity.up&buff.opportunity.stack<buff.opportunity.max_stack&variable.ambush_condition" );
  stealth_cds_off_meta->add_action( "vanish,if=!talent.underhanded_upper_hand&!talent.crackshot&!talent.hidden_opportunity&talent.fateful_ending&(!buff.fatebound_lucky_coin.up&(buff.fatebound_coin_tails.stack>=5|buff.fatebound_coin_heads.stack>=5)|buff.fatebound_lucky_coin.up&!cooldown.between_the_eyes.ready)" );
  stealth_cds_off_meta->add_action( "vanish,if=!talent.underhanded_upper_hand&!talent.crackshot&!talent.hidden_opportunity&!talent.fateful_ending&talent.take_em_by_surprise&!buff.take_em_by_surprise.up" );
  stealth_cds_off_meta->add_action( "shadowmeld,if=variable.finish_condition&!cooldown.vanish.ready" );
}
//outlaw_apl_end

//subtlety_apl_start
void subtlety( player_t* p )
{
  action_priority_list_t* default_ = p->get_action_priority_list( "default" );
  action_priority_list_t* precombat = p->get_action_priority_list( "precombat" );
  action_priority_list_t* cds = p->get_action_priority_list( "cds" );
  action_priority_list_t* race = p->get_action_priority_list( "race" );
  action_priority_list_t* item = p->get_action_priority_list( "item" );
  action_priority_list_t* stealth_cds = p->get_action_priority_list( "stealth_cds" );
  action_priority_list_t* finish = p->get_action_priority_list( "finish" );
  action_priority_list_t* build = p->get_action_priority_list( "build" );
  action_priority_list_t* fill = p->get_action_priority_list( "fill" );

  precombat->add_action( "apply_poison" );
  precombat->add_action( "snapshot_stats" );
  precombat->add_action( "variable,name=priority_rotation,value=priority_rotation" );
  precombat->add_action( "variable,name=trinket_sync_slot,value=1,if=trinket.1.has_stat.any_dps&(!trinket.2.has_stat.any_dps|trinket.1.is.treacherous_transmitter|trinket.1.cooldown.duration>=trinket.2.cooldown.duration)" );
  precombat->add_action( "variable,name=trinket_sync_slot,value=2,if=trinket.2.has_stat.any_dps&(!trinket.1.has_stat.any_dps|trinket.2.cooldown.duration>trinket.1.cooldown.duration)" );
  precombat->add_action( "stealth" );

  default_->add_action( "stealth" );
  default_->add_action( "variable,name=stealth,value=buff.shadow_dance.up|buff.stealth.up|buff.vanish.up", "Variables" );
  default_->add_action( "variable,name=targets,value=spell_targets.shuriken_storm" );
  default_->add_action( "variable,name=skip_rupture,value=buff.shadow_dance.up|!buff.slice_and_dice.up|buff.darkest_night.up|variable.targets>=8&!talent.replicating_shadows&talent.unseen_blade" );
  default_->add_action( "variable,name=maintenance,value=(dot.rupture.ticking|variable.skip_rupture)&buff.slice_and_dice.up" );
  default_->add_action( "variable,name=secret,value=buff.shadow_dance.up|(cooldown.flagellation.remains<40&cooldown.flagellation.remains>20&talent.death_perception)" );
  default_->add_action( "variable,name=racial_sync,value=(buff.shadow_blades.up&buff.shadow_dance.up)|!talent.shadow_blades&buff.symbols_of_death.up|fight_remains<20" );
  default_->add_action( "variable,name=shd_cp,value=combo_points<=1|buff.darkest_night.up&combo_points>=7|effective_combo_points>=6&talent.unseen_blade" );
  default_->add_action( "call_action_list,name=cds", "Cooldowns" );
  default_->add_action( "call_action_list,name=race", "Racials" );
  default_->add_action( "call_action_list,name=item", "Items (Trinkets)" );
  default_->add_action( "call_action_list,name=stealth_cds,if=!variable.stealth", "Cooldowns for Stealth" );
  default_->add_action( "call_action_list,name=finish,if=!buff.darkest_night.up&effective_combo_points>=6|buff.darkest_night.up&combo_points==cp_max_spend", "Finishing Rules" );
  default_->add_action( "call_action_list,name=build", "Combo Point Builder" );
  default_->add_action( "call_action_list,name=fill,if=!variable.stealth", "Filler, Spells used if you can use nothing else." );

  cds->add_action( "cold_blood,if=cooldown.secret_technique.up&buff.shadow_dance.up&combo_points>=6&variable.secret&buff.flagellation_persist.up", "Cooldowns" );
  cds->add_action( "potion,if=buff.bloodlust.react|fight_remains<30|buff.flagellation_buff.up" );
  cds->add_action( "symbols_of_death,if=(buff.symbols_of_death.remains<=3&variable.maintenance&(!talent.flagellation|cooldown.flagellation.remains>=30-15*!talent.death_perception&cooldown.secret_technique.remains<8|!talent.death_perception)|fight_remains<=15)" );
  cds->add_action( "shadow_blades,if=variable.maintenance&variable.shd_cp&buff.shadow_dance.up&!buff.premeditation.up" );
  cds->add_action( "thistle_tea,if=buff.shadow_dance.remains>2&!buff.thistle_tea.up" );
  cds->add_action( "flagellation,if=combo_points>=5&cooldown.shadow_blades.remains<=3|fight_remains<=25" );

  race->add_action( "blood_fury,if=variable.racial_sync", "Race Cooldowns" );
  race->add_action( "berserking,if=variable.racial_sync" );
  race->add_action( "fireblood,if=variable.racial_sync&buff.shadow_dance.up" );
  race->add_action( "ancestral_call,if=variable.racial_sync" );
  race->add_action( "invoke_external_buff,name=power_infusion,if=buff.shadow_dance.up" );

  item->add_action( "use_item,name=treacherous_transmitter,if=cooldown.flagellation.remains<=2|fight_remains<=15", "Trinket and Items" );
  item->add_action( "do_treacherous_transmitter_task,if=buff.shadow_dance.up|fight_remains<=15" );
  item->add_action( "use_item,name=imperfect_ascendancy_serum,use_off_gcd=1,if=dot.rupture.ticking&buff.flagellation_buff.up" );
  item->add_action( "use_item,name=mad_queens_mandate,if=(!talent.lingering_darkness|buff.lingering_darkness.up|equipped.treacherous_transmitter)&(!equipped.treacherous_transmitter|trinket.treacherous_transmitter.cooldown.remains>20)|fight_remains<=15" );
  item->add_action( "use_items,slots=trinket1,if=(variable.trinket_sync_slot=1&(buff.shadow_blades.up|fight_remains<=20)|(variable.trinket_sync_slot=2&(!trinket.2.cooldown.ready&!buff.shadow_blades.up&cooldown.shadow_blades.remains>20))|!variable.trinket_sync_slot)" );
  item->add_action( "use_items,slots=trinket2,if=(variable.trinket_sync_slot=2&(buff.shadow_blades.up|fight_remains<=20)|(variable.trinket_sync_slot=1&(!trinket.1.cooldown.ready&!buff.shadow_blades.up&cooldown.shadow_blades.remains>20))|!variable.trinket_sync_slot)" );

  stealth_cds->add_action( "shadow_dance,if=variable.shd_cp&variable.maintenance&cooldown.secret_technique.remains<=24&(buff.symbols_of_death.remains>=6|buff.shadow_blades.remains>=6)|fight_remains<=10", "Shadow Dance, Vanish, Shadowmeld" );
  stealth_cds->add_action( "vanish,if=energy>=40&!buff.subterfuge.up&effective_combo_points<=3" );
  stealth_cds->add_action( "shadowmeld,if=energy>=40&combo_points.deficit>=3" );

  finish->add_action( "secret_technique,if=variable.secret" );
  finish->add_action( "rupture,if=!variable.skip_rupture&(!dot.rupture.ticking|refreshable)&target.time_to_die-remains>6", "Maintenance Finisher" );
  finish->add_action( "rupture,cycle_targets=1,if=!variable.skip_rupture&!variable.priority_rotation&&target.time_to_die>=(2*combo_points)&refreshable&variable.targets>=2" );
  finish->add_action( "rupture,if=talent.unseen_blade&cooldown.flagellation.remains<10" );
  finish->add_action( "coup_de_grace,if=debuff.fazed.up", "Direct Damage Finisher" );
  finish->add_action( "black_powder,if=!variable.priority_rotation&variable.maintenance&((variable.targets>=2&talent.deathstalkers_mark&(!buff.darkest_night.up|buff.shadow_dance.up&variable.targets>=5))|talent.unseen_blade&variable.targets>=7)" );
  finish->add_action( "eviscerate" );

  build->add_action( "shadowstrike,cycle_targets=1,if=debuff.find_weakness.remains<=2&variable.targets=2&talent.unseen_blade|!used_for_danse&!talent.premeditation", "Combo Point Builder" );
  build->add_action( "shuriken_tornado,if=buff.lingering_darkness.up|talent.deathstalkers_mark&cooldown.shadow_blades.remains>=32&variable.targets>=3|talent.unseen_blade&buff.symbols_of_death.up&variable.targets>=4" );
  build->add_action( "shuriken_storm,if=buff.clear_the_witnesses.up&variable.targets>=2" );
  build->add_action( "shadowstrike,cycle_targets=1,if=talent.deathstalkers_mark&!debuff.deathstalkers_mark.up&variable.targets>=3&(buff.shadow_blades.up|buff.premeditation.up|talent.the_rotten)" );
  build->add_action( "shuriken_storm,if=talent.deathstalkers_mark&!buff.premeditation.up&variable.targets>=(2+3*buff.shadow_dance.up)|buff.clear_the_witnesses.up&!buff.symbols_of_death.up|buff.flawless_form.up&variable.targets>=3&!variable.stealth|talent.unseen_blade&buff.the_rotten.stack=1&variable.targets>=7&buff.shadow_dance.up" );
  build->add_action( "shadowstrike" );
  build->add_action( "goremaws_bite,if=combo_points.deficit>=3" );
  build->add_action( "gloomblade" );
  build->add_action( "backstab" );

  fill->add_action( "arcane_torrent,if=energy.deficit>=15+energy.regen", "This list usually contains Cooldowns with neglectable impact that causes global cooldowns" );
  fill->add_action( "arcane_pulse" );
  fill->add_action( "lights_judgment" );
  fill->add_action( "bag_of_tricks" );
}
//subtlety_apl_end

} // namespace rogue_apl
