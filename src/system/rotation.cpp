#include "rotation.hpp"

#include "component/actor/begun_casting_skills.hpp"
#include "component/actor/casting_skills.hpp"
#include "component/actor/finished_casting_skills.hpp"
#include "component/actor/no_more_rotation.hpp"
#include "component/actor/rotation_component.hpp"
#include "component/counter/is_counter.hpp"
#include "component/damage/effects_pipeline.hpp"
#include "component/damage/strikes_pipeline.hpp"
#include "component/encounter/encounter_configuration_component.hpp"
#include "component/equipment/bundle.hpp"
#include "component/equipment/weapons.hpp"
#include "component/hierarchy/owner_component.hpp"
#include "component/lifecycle/destroy_entity.hpp"
#include "component/skill/is_skill.hpp"
#include "component/temporal/animation_component.hpp"

#include "utils/actor_utils.hpp"
#include "utils/condition_utils.hpp"
#include "utils/counter_utils.hpp"
#include "utils/entity_utils.hpp"
#include "utils/side_effect_utils.hpp"
#include "utils/skill_utils.hpp"

namespace gw2combat::system {

void perform_rotations(registry_t& registry) {
    registry.view<component::rotation_component>(entt::exclude<component::no_more_rotation>)
        .each([&](entity_t entity, component::rotation_component& rotation_component) {
            auto current_tick = utils::get_current_tick(registry);
            bool is_in_animation = registry.any_of<component::animation_component>(entity);

            if (rotation_component.current_idx >=
                static_cast<int>(rotation_component.rotation.skill_casts.size())) {
                if (rotation_component.repeat) {
                    rotation_component.current_idx = 0;
                    rotation_component.tick_offset = current_tick;
                } else {
                    registry.emplace<component::no_more_rotation>(entity);
                    spdlog::info("[{}] {} has no more rotation",
                                 utils::get_current_tick(registry),
                                 utils::get_entity_name(entity, registry));
                    return;
                }
            }

            auto& next_skill_cast =
                rotation_component.rotation.skill_casts[rotation_component.current_idx];

            // Make sure this skill can only be cast at or after the time specified in the rotation
            // configuration
            if (current_tick < next_skill_cast.cast_time_ms + rotation_component.tick_offset) {
                return;
            }

            auto skill_entity = utils::get_skill_entity(next_skill_cast.skill, entity, registry);

            auto& skill_configuration =
                utils::get_skill_configuration(next_skill_cast.skill, entity, registry);
            bool is_instant_cast_skill = skill_configuration.cast_duration[0] == 0;
            if (!is_instant_cast_skill && is_in_animation) {
                return;
            }
            if (!registry
                     .get<component::encounter_configuration_component>(
                         utils::get_singleton_entity())
                     .encounter.require_afk_skills) {
                if (registry.get<component::ammo>(skill_entity).current_ammo <= 0 &&
                    !(next_skill_cast.skill == "Weapon Swap" &&
                      registry.any_of<component::bundle_component>(entity))) {
                    return;
                }
            }

            utils::assert_can_cast_skill(next_skill_cast.skill, entity, registry);

            int pulse_no_quickness_duration =
                std::max(skill_configuration.pulse_on_tick_list[0].empty()
                             ? 0
                             : skill_configuration.pulse_on_tick_list[0].back(),
                         skill_configuration.cast_duration[0]);
            int pulse_quickness_duration =
                std::max(skill_configuration.pulse_on_tick_list[1].empty()
                             ? 0
                             : skill_configuration.pulse_on_tick_list[1].back(),
                         skill_configuration.cast_duration[1]);
            int strike_no_quickness_duration =
                std::max(skill_configuration.strike_on_tick_list[0].empty()
                             ? 0
                             : skill_configuration.strike_on_tick_list[0].back(),
                         skill_configuration.cast_duration[0]);
            int strike_quickness_duration =
                std::max(skill_configuration.strike_on_tick_list[1].empty()
                             ? 0
                             : skill_configuration.strike_on_tick_list[1].back(),
                         skill_configuration.cast_duration[1]);

            auto& casting_skills_component =
                registry.get_or_emplace<component::casting_skills_component>(entity);
            casting_skills_component.skills.emplace_back(
                component::casting_skills_component::skill_state_t{
                    skill_entity,
                    {0, 0},
                    {pulse_no_quickness_duration, pulse_quickness_duration},
                    {0, 0},
                    {strike_no_quickness_duration, strike_quickness_duration},
                    0,
                    0});
            auto& begun_casting_skills_component =
                registry.get_or_emplace<component::begun_casting_skills>(entity);
            begun_casting_skills_component.skill_entities.emplace_back(skill_entity);

            rotation_component.current_idx += 1;

            if (is_instant_cast_skill) {
                spdlog::info("[{}] {} casting instant skill {} rotation index {}",
                             utils::get_current_tick(registry),
                             utils::get_entity_name(entity, registry),
                             utils::to_string(next_skill_cast.skill),
                             rotation_component.current_idx);
            } else {
                registry.emplace<component::animation_component>(
                    entity,
                    component::animation_component{skill_configuration.cast_duration, {0, 0}});
                spdlog::info("[{}] {} casting skill {} rotation index {}",
                             utils::get_current_tick(registry),
                             utils::get_entity_name(entity, registry),
                             utils::to_string(next_skill_cast.skill),
                             rotation_component.current_idx);
            }
        });
}

void perform_skills(registry_t& registry) {
    registry.view<component::casting_skills_component>().each(
        [&](entity_t entity, component::casting_skills_component& casting_skills_component) {
            for (auto& casting_skill : casting_skills_component.skills) {
                auto& skill_configuration =
                    registry.get<component::is_skill>(casting_skill.skill_entity)
                        .skill_configuration;

                double pulse_no_quickness_progress_pct =
                    casting_skill.pulse_duration[0] == 0
                        ? 100
                        : (casting_skill.pulse_progress[0] * 100) /
                              (double)casting_skill.pulse_duration[0];
                double pulse_quickness_progress_pct =
                    casting_skill.pulse_duration[1] == 0
                        ? 100
                        : (casting_skill.pulse_progress[1] * 100) /
                              (double)casting_skill.pulse_duration[1];
                double pulse_effective_progress_pct =
                    pulse_no_quickness_progress_pct + pulse_quickness_progress_pct;
                int pulse_effective_tick = utils::round_down(
                    (double)casting_skill.pulse_duration[0] * pulse_effective_progress_pct / 100.0);

                double strike_no_quickness_progress_pct =
                    casting_skill.strike_duration[0] == 0
                        ? 100
                        : (casting_skill.strike_progress[0] * 100) /
                              (double)casting_skill.strike_duration[0];
                double strike_quickness_progress_pct =
                    casting_skill.strike_duration[1] == 0
                        ? 100
                        : (casting_skill.strike_progress[1] * 100) /
                              (double)casting_skill.strike_duration[1];
                double strike_effective_progress_pct =
                    strike_no_quickness_progress_pct + strike_quickness_progress_pct;
                int strike_effective_tick =
                    utils::round_down((double)casting_skill.strike_duration[0] *
                                      strike_effective_progress_pct / 100.0);

                while (
                    casting_skill.next_pulse_idx <
                        static_cast<int>(skill_configuration.pulse_on_tick_list[0].size()) &&
                    pulse_effective_tick >=
                        skill_configuration.pulse_on_tick_list[0][casting_skill.next_pulse_idx]) {
                    auto& outgoing_effects_component =
                        registry.get_or_emplace<component::outgoing_effects_component>(entity);
                    for (auto& effect_application :
                         skill_configuration.on_pulse_effect_applications) {
                        outgoing_effects_component.effect_applications.emplace_back(
                            component::effect_application_t{
                                .condition = effect_application.condition,
                                .source_skill = skill_configuration.skill_key,
                                .effect = effect_application.effect,
                                .unique_effect = effect_application.unique_effect,
                                .direction = component::effect_application_t::convert_direction(
                                    effect_application.direction),
                                .base_duration_ms = effect_application.base_duration_ms,
                                .num_stacks = effect_application.num_stacks,
                                .num_targets = effect_application.num_targets});
                    }
                    ++casting_skill.next_pulse_idx;
                }

                while (
                    casting_skill.next_strike_idx <
                        static_cast<int>(skill_configuration.strike_on_tick_list[0].size()) &&
                    strike_effective_tick >=
                        skill_configuration.strike_on_tick_list[0][casting_skill.next_strike_idx]) {
                    auto& outgoing_strikes_component =
                        registry.get_or_emplace<component::outgoing_strikes_component>(entity);
                    auto this_strike = component::strike_t{casting_skill.skill_entity,
                                                           skill_configuration.num_targets};
                    outgoing_strikes_component.strikes.emplace_back(this_strike);
                    ++casting_skill.next_strike_idx;
                }

                if (strike_effective_progress_pct >= 100 && pulse_effective_progress_pct >= 100) {
                    auto& finished_casting_skills =
                        registry.get_or_emplace<component::finished_casting_skills>(entity);
                    finished_casting_skills.skill_entities.emplace_back(casting_skill.skill_entity);

                    if (skill_configuration.cooldown[0] != 0 &&
                        !(skill_configuration.skill_key == "Weapon Swap" &&
                          registry.any_of<component::bundle_component>(entity))) {
                        utils::put_skill_on_cooldown(
                            skill_configuration.skill_key, entity, registry);
                        auto owner_entity = utils::get_owner(entity, registry);
                        if (entity != owner_entity) {
                            utils::put_skill_on_cooldown(
                                skill_configuration.skill_key, owner_entity, registry);
                        }
                    }
                    spdlog::info("[{}] {}: finished casting skill {}",
                                 utils::get_current_tick(registry),
                                 utils::get_entity_name(entity, registry),
                                 utils::to_string(skill_configuration.skill_key));
                }
            }
        });

    registry.view<component::finished_casting_skills>().each(
        [&](entity_t actor_entity,
            const component::finished_casting_skills& finished_casting_skills) {
            for (auto finished_casting_skill_entity : finished_casting_skills.skill_entities) {
                auto& skill_configuration =
                    registry.get<component::is_skill>(finished_casting_skill_entity)
                        .skill_configuration;

                if (!skill_configuration.equip_bundle.empty()) {
                    registry.emplace<component::bundle_component>(
                        actor_entity,
                        component::bundle_component{skill_configuration.equip_bundle});
                    registry.emplace_or_replace<component::equipped_bundle>(
                        actor_entity, skill_configuration.equip_bundle);
                    spdlog::info("[{}] {}: equipped bundle {}",
                                 utils::get_current_tick(registry),
                                 utils::get_entity_name(actor_entity, registry),
                                 skill_configuration.equip_bundle);
                } else if (skill_configuration.skill_key == "Weapon Swap") {
                    if (auto bundle_ptr =
                            registry.try_get<component::bundle_component>(actor_entity);
                        bundle_ptr) {
                        registry.emplace_or_replace<component::dropped_bundle>(actor_entity,
                                                                               bundle_ptr->name);
                        registry.remove<component::bundle_component>(actor_entity);
                        spdlog::info("[{}] {}: dropped bundle {}",
                                     utils::get_current_tick(registry),
                                     utils::get_entity_name(actor_entity, registry),
                                     bundle_ptr->name);
                    } else {
                        if (!registry.any_of<component::current_weapon_set>(actor_entity)) {
                            throw std::runtime_error("no equipped_weapon_set on entity");
                        }
                        auto& equipped_weapons =
                            registry.get<component::equipped_weapons>(actor_entity);
                        if (equipped_weapons.weapons.size() == 1) {
                            throw std::runtime_error(
                                "cannot weapon swap when there is only 1 weapon set equipped");
                        }

                        auto current_set =
                            registry.get<component::current_weapon_set>(actor_entity).set;
                        if (current_set == actor::weapon_set::SET_1) {
                            registry.replace<component::current_weapon_set>(
                                actor_entity,
                                component::current_weapon_set{actor::weapon_set::SET_2});
                        } else {
                            registry.replace<component::current_weapon_set>(
                                actor_entity,
                                component::current_weapon_set{actor::weapon_set::SET_1});
                        }
                    }
                }

                auto side_effect_condition_fn = [&](const configuration::condition_t& condition) {
                    return utils::on_finished_casting_conditions_satisfied(
                        condition, actor_entity, skill_configuration, registry);
                };
                utils::apply_side_effects(registry, actor_entity, side_effect_condition_fn);

                utils::enqueue_child_skills(
                    actor_entity,
                    "Temporary " + skill_configuration.skill_key + " Entity",
                    skill_configuration.child_skill_keys,
                    registry);
            }
        });
}

void cleanup_casting_skills(registry_t& registry) {
    registry.view<component::casting_skills_component, component::finished_casting_skills>().each(
        [&](entity_t entity,
            component::casting_skills_component& casting_skills_component,
            const component::finished_casting_skills& finished_casting_skills) {
            for (auto skill_entity : finished_casting_skills.skill_entities) {
                auto skill_pos = std::find_if(
                    casting_skills_component.skills.cbegin(),
                    casting_skills_component.skills.cend(),
                    [&](const component::casting_skills_component::skill_state_t& skill_state) {
                        return skill_state.skill_entity == skill_entity;
                    });
                if (skill_pos != casting_skills_component.skills.cend()) {
                    casting_skills_component.skills.erase(skill_pos);
                }
            }
            if (casting_skills_component.skills.empty()) {
                registry.remove<component::casting_skills_component>(entity);
            }
            registry.remove<component::finished_casting_skills>(entity);
        });
}

void destroy_actors_with_no_rotation(registry_t& registry) {
    registry
        .view<component::destroy_after_rotation, component::no_more_rotation>(
            entt::exclude<component::finished_casting_skills, component::casting_skills_component>)
        .each([&](entity_t entity) {
            registry.emplace_or_replace<component::destroy_entity>(entity);
        });
}

}  // namespace gw2combat::system
