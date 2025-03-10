action_priority_list_t* precombat = get_action_priority_list( "precombat" );
action_priority_list_t* def = get_action_priority_list( "default" );
action_priority_list_t* bear = get_action_priority_list( "bear" );

precombat->add_action( "variable,name=If_build,value=1,value_else=0,if=talent.thorns_of_iron.enabled&talent.ursocs_endurance.enabled","Executed before combat begins. Accepts non-harmful actions only." );
precombat->add_action( "heart_of_the_Wild,if=talent.heart_of_the_wild.enabled&!talent.rip.enabled" );
precombat->add_action( "bear_form" );

def->add_action( "auto_attack,if=!buff.prowl.up","Executed every time the actor is available." );
def->add_action( "use_item,name=bestinslots,if=buff.incarnation_guardian_of_ursoc.up|buff.berserk_bear.up" );
def->add_action( "use_item,name=tome_of_lights_devotion,if=buff.inner_resilience.up" );
def->add_action( "use_items" );
def->add_action( "potion,if=(buff.berserk_bear.up|buff.incarnation_guardian_of_ursoc.up)" );
def->add_action( "run_action_list,name=bear" );

bear->add_action( "maul,if=buff.ravage.up&active_enemies>1" );
bear->add_action( "heart_of_the_Wild,if=(talent.heart_of_the_wild.enabled&!talent.rip.enabled)|talent.heart_of_the_wild.enabled&buff.feline_potential_counter.stack=6&active_enemies<3" );
bear->add_action( "moonfire,cycle_targets=1,if=buff.bear_form.up&(((!ticking&target.time_to_die>12)|(refreshable&target.time_to_die>12))&active_enemies<7&talent.fury_of_nature.enabled)|(((!ticking&target.time_to_die>12)|(refreshable&target.time_to_die>12))&active_enemies<4&!talent.fury_of_nature.enabled)" );
bear->add_action( "thrash_bear,target_if=refreshable|(dot.thrash_bear.stack<5&talent.flashing_claws.rank=2|dot.thrash_bear.stack<4&talent.flashing_claws.rank=1|dot.thrash_bear.stack<3&!talent.flashing_claws.enabled)" );
bear->add_action( "bristling_fur,if=!cooldown.pause_action.remains&cooldown.rage_of_the_sleeper.remains>8" );
bear->add_action( "barkskin,if=buff.bear_form.up" );
bear->add_action( "lunar_beam" );
bear->add_action( "convoke_the_spirits,if=(talent.wildpower_surge.enabled&buff.cat_form.up&buff.feline_potential.up)|!talent.wildpower_surge.enabled" );
bear->add_action( "berserk_bear" );
bear->add_action( "incarnation" );
bear->add_action( "rage_of_the_sleeper,if=(((buff.incarnation_guardian_of_ursoc.down&cooldown.incarnation_guardian_of_ursoc.remains>60)|buff.berserk_bear.down)&rage>40&(!talent.convoke_the_spirits.enabled)|(buff.incarnation_guardian_of_ursoc.up|buff.berserk_bear.up)&rage>40&(!talent.convoke_the_spirits.enabled)|(talent.convoke_the_spirits.enabled)&rage>40)" );
bear->add_action( "berserking,if=(buff.berserk_bear.up|buff.incarnation_guardian_of_ursoc.up)" );
bear->add_action( "maul,if=buff.ravage.up&active_enemies<2" );
bear->add_action( "raze,if=(buff.tooth_and_claw.stack>1|buff.tooth_and_claw.up&buff.tooth_and_claw.remains<1+gcd)&variable.If_build=1" );
bear->add_action( "raze,if=variable.If_build=0&(buff.tooth_and_claw.stack>1|buff.tooth_and_claw.up&buff.tooth_and_claw.remains<1+gcd|buff.vicious_cycle_maul.stack=3)" );
bear->add_action( "thrash_bear,if=active_enemies>=5&talent.lunar_calling.enabled" );
bear->add_action( "ironfur,target_if=!debuff.tooth_and_claw.up,if=!buff.ironfur.up&rage>50&!cooldown.pause_action.remains&variable.If_build=0&!buff.rage_of_the_sleeper.up|rage>90&variable.If_build=0|!debuff.tooth_and_claw.up&!buff.ironfur.up&rage>50&!cooldown.pause_action.remains&variable.If_build=0&!buff.rage_of_the_sleeper.up" );
bear->add_action( "ironfur,if=!buff.ravage.up&((rage>40&variable.If_build=1&cooldown.rage_of_the_sleeper.remains>3&talent.rage_of_the_sleeper.enabled|(buff.incarnation.up|buff.berserk_bear.up)&rage>20&variable.If_build=1&cooldown.rage_of_the_sleeper.remains>3&talent.rage_of_the_sleeper.enabled|rage>90&variable.If_build=1&!talent.fount_of_strength.enabled|rage>110&variable.If_build=1&talent.fount_of_strength.enabled|(buff.incarnation.up|buff.berserk_bear.up)&rage>20&variable.If_build=1&buff.rage_of_the_sleeper.up&talent.rage_of_the_sleeper.enabled))" );
bear->add_action( "ironfur,if=!buff.ravage.up&((rage>40&variable.If_build=1&!talent.rage_of_the_sleeper.enabled|(buff.incarnation.up|buff.berserk_bear.up)&rage>20&variable.If_build=1&!talent.rage_of_the_sleeper.enabled|(buff.incarnation.up|buff.berserk_bear.up)&rage>20&variable.If_build=1&!talent.rage_of_the_sleeper.enabled))" );
bear->add_action( "ferocious_bite,if=(buff.cat_form.up&buff.feline_potential.up&active_enemies<3&(buff.incarnation.up|buff.berserk_bear.up)&!dot.rip.refreshable)" );
bear->add_action( "rip,if=(buff.cat_form.up&buff.feline_potential.up&active_enemies<3&(!buff.incarnation.up|!buff.berserk_bear.up))|(buff.cat_form.up&buff.feline_potential.up&active_enemies<3&(buff.incarnation.up|buff.berserk_bear.up)&refreshable)" );
bear->add_action( "raze,if=variable.If_build=1&buff.vicious_cycle_maul.stack=3&!talent.ravage.enabled" );
bear->add_action( "mangle,if=buff.gore.up&active_enemies<11|buff.incarnation_guardian_of_ursoc.up&buff.feline_potential_counter.stack<6&talent.wildpower_surge.enabled" );
bear->add_action( "raze,if=variable.If_build=0" );
bear->add_action( "shred,if=cooldown.rage_of_the_sleeper.remains<=52&buff.feline_potential_counter.stack=6&!buff.cat_form.up&!dot.rake.refreshable&active_enemies<3&talent.fluid_form.enabled" );
bear->add_action( "rake,if=cooldown.rage_of_the_sleeper.remains<=52&buff.feline_potential_counter.stack=6&!buff.cat_form.up&active_enemies<3&talent.fluid_form.enabled" );
bear->add_action( "mangle,if=buff.cat_form.up&talent.fluid_form.enabled" );
bear->add_action( "maul,if=variable.If_build=1&(((buff.tooth_and_claw.stack>1|buff.tooth_and_claw.up&buff.tooth_and_claw.remains<1+gcd)&active_enemies<=5&!talent.raze.enabled)|((buff.tooth_and_claw.stack>1|buff.tooth_and_claw.up&buff.tooth_and_claw.remains<1+gcd)&active_enemies=1&talent.raze.enabled)|((buff.tooth_and_claw.stack>1|buff.tooth_and_claw.up&buff.tooth_and_claw.remains<1+gcd)&active_enemies<=5&!talent.raze.enabled))" );
bear->add_action( "maul,if=variable.If_build=0&((buff.tooth_and_claw.up&active_enemies<=5&!talent.raze.enabled)|(buff.tooth_and_claw.up&active_enemies=1&talent.raze.enabled))" );
bear->add_action( "maul,if=(active_enemies<=5&!talent.raze.enabled&variable.If_build=0)|(active_enemies=1&talent.raze.enabled&variable.If_build=0)|buff.vicious_cycle_maul.stack=3&active_enemies<=5&!talent.raze.enabled" );
bear->add_action( "thrash_bear,if=active_enemies>=5" );
bear->add_action( "mangle,if=(buff.incarnation.up&active_enemies<=4)|(buff.incarnation.up&talent.soul_of_the_forest.enabled&active_enemies<=5)|((rage<88)&active_enemies<11)|((rage<83)&active_enemies<11&talent.soul_of_the_forest.enabled)" );
bear->add_action( "thrash_bear,if=active_enemies>1" );
bear->add_action( "pulverize,target_if=dot.thrash_bear.stack>2" );
bear->add_action( "thrash_bear" );
bear->add_action( "moonfire,if=buff.galactic_guardian.up&buff.bear_form.up&talent.boundless_moonlight.enabled" );
bear->add_action( "rake,if=cooldown.rage_of_the_sleeper.remains<=52&rage<40&active_enemies<3&!talent.lunar_insight.enabled&talent.fluid_form.enabled&energy>70&refreshable" );
bear->add_action( "shred,if=cooldown.rage_of_the_sleeper.remains<=52&rage<40&active_enemies<3&!talent.lunar_insight.enabled&talent.fluid_form.enabled&energy>70&!buff.rage_of_the_sleeper.up" );
bear->add_action( "rip,if=buff.cat_form.up&!dot.rip.ticking&active_enemies<3" );
bear->add_action( "ferocious_bite,if=dot.rip.ticking&combo_points>4&active_enemies<3" );
bear->add_action( "starsurge,if=talent.starsurge.enabled&rage<20" );
bear->add_action( "swipe_bear,if=(talent.lunar_insight.enabled&active_enemies>4)|!talent.lunar_insight.enabled|talent.lunar_insight.enabled&active_enemies<2" );
bear->add_action( "moonfire,if=(talent.lunar_insight.enabled&active_enemies>1)&buff.bear_form.up" );
